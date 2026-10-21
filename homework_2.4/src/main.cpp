
/*
 * Модуль 2.4 — Усунення брязкоту контактів кнопки
 * ESP32-S3 DevKitC-1, Arduino Framework (arduino-esp32 3.x)
 *
 * Канал A: GPIO4 — кнопка без RC (зовнішній pull-up 10k)
 * Канал B: GPIO5 — кнопка з RC (10k pull-up, 100nF вузол->GND, 100R послідовно з кнопкою)
 *
 * На кожному каналі паралельно працюють:
 *   1) Без debounce — ISR на FALLING, лічильник + лог
 *   2) Time-based   — ігнор подій ближче 50 мс до попередньої прийнятої (поза ISR)
 *   3) State-based  — ISR лише сигналізує, loop() перевіряє, чи кнопка досі натиснута
 *   4) Polling      — без IRQ, опитування кожні 5 мс, FSM-debounce
 *   5) Hardware RC  — = метод 1 на каналі B
 *
 * Serial 115200:
 *   n20  — скільки разів ти фізично натиснув кожну кнопку (за замовч. 20)
 *   t    — вивести порівняльну таблицю
 *   r    — скинути статистику
 * Таблиця також виводиться автоматично після 10 с без натискань.
 */
#include <Arduino.h>

// ---------- Налаштування ----------
constexpr uint8_t  PIN_A           = 4;
constexpr uint8_t  PIN_B           = 5;
constexpr uint32_t TB_DEBOUNCE_US  = 50000;   // time-based: 50 мс
constexpr uint32_t SB_CHECK_US     = 20000;   // state-based: перевірити рівень через 20 мс
constexpr uint32_t SB_RELEASE_US   = 30000;   // state-based: відпущено стабільно 30 мс
constexpr uint32_t POLL_PERIOD_US  = 5000;    // polling: кожні 5 мс
constexpr uint8_t  POLL_STABLE_N   = 4;       // 4 x 5 мс = 20 мс стабільного рівня
constexpr uint32_t BURST_GAP_US    = 100000;  // 100 мс тиші = нова пачка фронтів
constexpr uint32_t AUTO_TABLE_MS   = 10000;
constexpr bool     LOG_EACH_IRQ    = true;    // false = чистіші заміри затримки
constexpr size_t   RB_SIZE         = 128;     // степінь 2

uint32_t expectedPresses = 20;

// ---------- Статистика затримки ----------
struct Lat {
  uint32_t sum = 0, n = 0, max = 0;
  void add(uint32_t us) { sum += us; n++; if (us > max) max = us; }
  float avgMs() const { return n ? (sum / 1000.0f) / n : 0.0f; }
  float maxMs() const { return max / 1000.0f; }
};

enum PollState : uint8_t { REL, PRESS_CHK, PRS, REL_CHK };

// ---------- Канал (одна кнопка) ----------
struct Channel {
  const char* name;
  uint8_t pin;

  // ISR -> loop
  volatile uint32_t irqCount;
  volatile uint32_t rb[RB_SIZE];
  volatile uint16_t head, tail;
  volatile uint32_t rbOverflow;

  uint32_t irqProcessed;
  uint32_t lastEdgeUs, burstStartUs;

  // 2) time-based
  bool tbHasLast; uint32_t tbLastAccUs, tbCount; Lat tbLat;

  // 3) state-based
  bool sbPending, sbLatched, sbHighSeen;
  uint32_t sbEventUs, sbHighSinceUs, sbCount, sbIgnored; Lat sbLat;

  // 4) polling
  PollState ps; uint8_t pcnt; uint32_t pollCount; Lat pollLat;

  Channel(const char* n, uint8_t p) : name(n), pin(p) { reset(); }

  void reset() {
    irqCount = 0; head = 0; tail = 0; rbOverflow = 0;
    irqProcessed = 0; lastEdgeUs = 0; burstStartUs = 0;
    tbHasLast = false; tbLastAccUs = 0; tbCount = 0; tbLat = Lat();
    sbPending = sbLatched = sbHighSeen = false;
    sbEventUs = sbHighSinceUs = sbCount = sbIgnored = 0; sbLat = Lat();
    ps = REL; pcnt = 0; pollCount = 0; pollLat = Lat();
  }
};

Channel chA("no RC", PIN_A);
Channel chB("RC",    PIN_B);
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t lastActivityMs = 0;
bool     tablePending   = false;

// ---------- ISR: мінімум роботи ----------
void IRAM_ATTR pushEdge(Channel& c) {
  uint32_t t = micros();
  c.irqCount = c.irqCount + 1;
  uint16_t next = (c.head + 1) & (RB_SIZE - 1);
  if (next != c.tail) { c.rb[c.head] = t; c.head = next; }
  else c.rbOverflow = c.rbOverflow + 1;
}
void IRAM_ATTR isrA() { pushEdge(chA); }
void IRAM_ATTR isrB() { pushEdge(chB); }

bool popEdge(Channel& c, uint32_t& t) {
  if (c.tail == c.head) return false;
  t = c.rb[c.tail];
  c.tail = (c.tail + 1) & (RB_SIZE - 1);
  return true;
}

void markActivity() { lastActivityMs = millis(); tablePending = true; }

// ---------- Завдання 1, 2, 3 ----------
void processChannel(Channel& c) {
  uint32_t t;
  while (popEdge(c, t)) {
    markActivity();

    // 1) Без debounce: кожне переривання = подія
    c.irqProcessed++;
    if (LOG_EACH_IRQ)
      Serial.printf("[%-5s] RAW   IRQ #%lu\n", c.name, (unsigned long)c.irqProcessed);

    // початок "пачки" фронтів — точка відліку затримки
    if (t - c.lastEdgeUs > BURST_GAP_US) c.burstStartUs = t;
    c.lastEdgeUs = t;

    // 2) Time-based: приймаємо, якщо від попередньої прийнятої >= 50 мс
    if (!c.tbHasLast || (t - c.tbLastAccUs) >= TB_DEBOUNCE_US) {
      c.tbHasLast = true;
      c.tbLastAccUs = t;
      c.tbCount++;
      uint32_t lat = micros() - c.burstStartUs;
      c.tbLat.add(lat);
      Serial.printf("[%-5s] TIME  press #%lu (%.2f ms)\n",
                    c.name, (unsigned long)c.tbCount, lat / 1000.0f);
    }

    // 3) State-based: IRQ лише ставить прапорець "щось сталося"
    if (!c.sbPending && !c.sbLatched) {
      c.sbPending = true;
      c.sbEventUs = t;
    }
  }

  // 3) State-based: через 20 мс дивимось, чи кнопка ДОСІ натиснута
  uint32_t now = micros();
  if (c.sbPending && (now - c.sbEventUs) >= SB_CHECK_US) {
    c.sbPending = false;
    if (digitalRead(c.pin) == LOW) {
      c.sbLatched = true;  // до відпускання нові події ігноруються
      c.sbCount++;
      uint32_t lat = now - c.burstStartUs;
      c.sbLat.add(lat);
      Serial.printf("[%-5s] STATE press #%lu (%.2f ms)\n",
                    c.name, (unsigned long)c.sbCount, lat / 1000.0f);
    } else {
      c.sbIgnored++;       // це був брязкіт при відпусканні
    }
  }
  // зняти "засувку", коли кнопка стабільно відпущена
  if (c.sbLatched) {
    if (digitalRead(c.pin) == HIGH) {
      if (!c.sbHighSeen) { c.sbHighSeen = true; c.sbHighSinceUs = now; }
      else if (now - c.sbHighSinceUs >= SB_RELEASE_US) { c.sbLatched = false; c.sbHighSeen = false; }
    } else {
      c.sbHighSeen = false;
    }
  }
}

// ---------- Завдання 4: Polling + FSM (без interrupts) ----------
void pollChannel(Channel& c) {
  bool low = (digitalRead(c.pin) == LOW);
  switch (c.ps) {
    case REL:
      if (low) { c.ps = PRESS_CHK; c.pcnt = 1; }
      break;
    case PRESS_CHK:
      if (!low) { c.ps = REL; break; }                 // це був брязкіт
      if (++c.pcnt >= POLL_STABLE_N) {
        c.ps = PRS;
        c.pollCount++;
        uint32_t lat = micros() - c.burstStartUs;      // burstStart лише для заміру
        c.pollLat.add(lat);
        markActivity();
        Serial.printf("[%-5s] POLL  press #%lu (%.2f ms)\n",
                      c.name, (unsigned long)c.pollCount, lat / 1000.0f);
      }
      break;
    case PRS:
      if (!low) { c.ps = REL_CHK; c.pcnt = 1; }
      break;
    case REL_CHK:
      if (low) { c.ps = PRS; break; }
      if (++c.pcnt >= POLL_STABLE_N) c.ps = REL;
      break;
  }
}

// ---------- Порівняльна таблиця ----------
void row(const char* method, uint32_t cnt, const char* lat,
         const char* complexity, const char* note) {
  char fb[32];
  long diff = (long)cnt - (long)expectedPresses;
  if (diff >= 0) snprintf(fb, sizeof fb, "%ld", diff);
  else           snprintf(fb, sizeof fb, "0 (пропущено %ld)", -diff);
  Serial.printf("| %s | %lu | %s | %s | %s | %s |\n",
                method, (unsigned long)cnt, fb, lat, complexity, note);
}

void latStr(char* buf, size_t n, const Lat& l) {
  snprintf(buf, n, "%.1f / %.1f", l.avgMs(), l.maxMs());
}

void printChannel(Channel& c, bool rc) {
  char l[32];
  row(rc ? "Hardware RC (без софту)" : "Без debounce",
      c.irqCount, "~0.005 (ISR)", rc ? "Низька (залізо)" : "Мінімальна",
      rc ? "RC згладжує брязкіт, але без софту не гарантує 1 подію"
         : "Кілька IRQ на натискання + спрацювання на відпусканні");

  latStr(l, sizeof l, c.tbLat);
  row(rc ? "Time-based + RC" : "Time-based", c.tbCount, l, "Низька",
      "Брязкіт відсічено, але release через >50 мс = хибне");

  latStr(l, sizeof l, c.sbLat);
  row(rc ? "State-based + RC" : "State-based", c.sbCount, l, "Середня",
      "1 натискання = 1 подія, release ігнорується");

  latStr(l, sizeof l, c.pollLat);
  row(rc ? "Polling + RC" : "Polling", c.pollCount, l, "Середня (FSM)",
      "Без IRQ, найстабільніше, затримка ~20-25 мс");
}

void printTable() {
  Serial.printf("\n=== Порівняльна таблиця (фізичних натискань N = %lu на кожну кнопку) ===\n",
                (unsigned long)expectedPresses);
  Serial.println("| Метод | Спрацювань | Хибних | Затримка avg/max, мс | Складність | Коментар |");
  Serial.println("|---|---|---|---|---|---|");
  printChannel(chA, false);
  printChannel(chB, true);
  if (chA.rbOverflow || chB.rbOverflow)
    Serial.printf("! Переповнення буфера IRQ: A=%lu B=%lu\n",
                  (unsigned long)chA.rbOverflow, (unsigned long)chB.rbOverflow);
  Serial.printf("State-based відкинув подій відпускання: A=%lu, B=%lu\n\n",
                (unsigned long)chA.sbIgnored, (unsigned long)chB.sbIgnored);
  tablePending = false;
}

void resetAll() {
  portENTER_CRITICAL(&mux);
  chA.reset();
  chB.reset();
  portEXIT_CRITICAL(&mux);
  Serial.println("Статистику скинуто. Натискай кожну кнопку N разів.");
}

void handleSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 't') printTable();
    else if (ch == 'r') resetAll();
    else if (ch == 'n') {
      long v = Serial.parseInt();
      if (v > 0) expectedPresses = v;
      Serial.printf("N = %lu\n", (unsigned long)expectedPresses);
    }
  }
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(PIN_A, INPUT);   // підтяжка зовнішня 10k
  pinMode(PIN_B, INPUT);

  attachInterrupt(digitalPinToInterrupt(PIN_A), isrA, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_B), isrB, FALLING);

  Serial.println("\nDebounce lab: A=GPIO4 (без RC), B=GPIO5 (з RC)");
  Serial.printf("Натисни кожну кнопку %lu разів. Команди: n<число>, t, r\n",
                (unsigned long)expectedPresses);
}

void loop() {
  handleSerial();

  processChannel(chA);
  processChannel(chB);

  static uint32_t lastPollUs = 0;
  uint32_t now = micros();
  if (now - lastPollUs >= POLL_PERIOD_US) {
    lastPollUs = now;
    pollChannel(chA);
    pollChannel(chB);
  }

  if (tablePending && millis() - lastActivityMs > AUTO_TABLE_MS) printTable();
}
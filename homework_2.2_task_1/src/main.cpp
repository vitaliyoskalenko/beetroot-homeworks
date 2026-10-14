// Завдання 1: вимірювання часу спрацювання / відпускання реле
// Плата: ESP32-S3 (Arduino core for ESP32), модуль реле 5 В (SRD-05VDC-SL-C)
//
// Модуль реле:  DC+ -> +5V (рейка від модуля живлення), DC- -> GND, IN -> GPIO4
// Контакти:     COM -> GND, NO -> R 1 кОм -> GPIO5 (INPUT_PULLUP)
// GND ESP32 обов'язково з'єднати з GND макетки (спільна земля)!
//
//   реле увімкнене  -> NO замкнений  -> GPIO5 = LOW
//   реле вимкнене   -> NO розімкнений -> GPIO5 = HIGH (підтяжка)
//
// Брязкіт: ISR фіксує ПЕРШИЙ фронт (момент першого торкання = механічна затримка)
// і лише рахує решту фронтів. Після SETTLE_MS перевіряємо, що контакт стабільно
// в очікуваному стані. Тривалість брязкоту = останній фронт - перший.
//
// Serial Monitor: 115200. Надішли 'r', щоб повторити серію.

#include <Arduino.h>

constexpr uint8_t  RELAY_PIN         = 4;
constexpr uint8_t  CONTACT_PIN       = 5;
constexpr bool     RELAY_ACTIVE_HIGH = true;   // перемичка H/L на модулі: H -> true, L -> false
constexpr int      N_MEAS            = 10;     // кількість вимірювань
constexpr uint32_t TIMEOUT_US        = 100000; // 100 мс — реле так довго не їде
constexpr uint32_t SETTLE_MS         = 50;     // чекаємо, поки брязкіт затихне
constexpr uint32_t PAUSE_MS          = 300;    // пауза між циклами

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool     armed     = false;
volatile uint32_t firstEdge = 0;
volatile uint32_t lastEdge  = 0;
volatile uint16_t edgeCount = 0;

void IRAM_ATTR onContact() {
  uint32_t t = micros();
  portENTER_CRITICAL_ISR(&mux);
  if (armed) {
    if (edgeCount == 0) firstEdge = t;   // перше торкання — час спрацювання
    lastEdge = t;
    edgeCount++;                          // решта фронтів — брязкіт
  }
  portEXIT_CRITICAL_ISR(&mux);
}

void relaySet(bool on) {
  digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_HIGH) ? HIGH : LOW);
}

struct Result {
  bool     ok;
  uint32_t delayUs;    // від команди до першого фронту
  uint32_t bounceUs;   // від першого до останнього фронту
  uint16_t edges;
};

Result measure(bool energize) {
  const int expected = energize ? LOW : HIGH;

  portENTER_CRITICAL(&mux);
  edgeCount = 0; firstEdge = 0; lastEdge = 0;
  armed = true;
  portEXIT_CRITICAL(&mux);

  uint32_t t0 = micros();
  relaySet(energize);

  while (edgeCount == 0 && (micros() - t0) < TIMEOUT_US) { /* чекаємо перший фронт */ }
  delay(SETTLE_MS);

  portENTER_CRITICAL(&mux);
  armed = false;
  uint16_t n = edgeCount;
  uint32_t f = firstEdge, l = lastEdge;
  portEXIT_CRITICAL(&mux);

  Result r{};
  r.edges = n;
  r.ok = (n > 0) && (digitalRead(CONTACT_PIN) == expected);
  if (r.ok) {
    r.delayUs  = f - t0;
    r.bounceUs = l - f;
  }
  return r;
}

struct Stats {
  uint64_t sum = 0; uint32_t mn = UINT32_MAX, mx = 0; int n = 0;
  void add(uint32_t v) { sum += v; n++; if (v < mn) mn = v; if (v > mx) mx = v; }
  float avgMs() const { return n ? (sum / (float)n) / 1000.0f : 0; }
};

void runSeries() {
  Stats on, off, bOn, bOff;

  Serial.println();
  Serial.println(F(" #  | ON, мкс  брязкіт  фронтів | OFF, мкс  брязкіт  фронтів"));
  Serial.println(F("----+---------------------------+----------------------------"));

  for (int i = 1; i <= N_MEAS; i++) {
    Result a = measure(true);
    delay(PAUSE_MS);
    Result b = measure(false);
    delay(PAUSE_MS);

    Serial.printf("%3d | ", i);
    if (a.ok) { Serial.printf("%7lu  %7lu  %5u   | ", a.delayUs, a.bounceUs, a.edges); on.add(a.delayUs); bOn.add(a.bounceUs); }
    else        Serial.print (F("  помилка / таймаут      | "));
    if (b.ok) { Serial.printf("%7lu  %7lu  %5u\n", b.delayUs, b.bounceUs, b.edges); off.add(b.delayUs); bOff.add(b.bounceUs); }
    else        Serial.println(F("  помилка / таймаут"));
  }

  Serial.println(F("------------------------------------------------------------"));
  Serial.printf("Увімкнення: середнє %.3f мс (min %.3f, max %.3f), брязкіт сер. %.3f мс, n=%d\n",
                on.avgMs(), on.mn / 1000.0f, on.mx / 1000.0f, bOn.avgMs(), on.n);
  Serial.printf("Вимкнення:  середнє %.3f мс (min %.3f, max %.3f), брязкіт сер. %.3f мс, n=%d\n",
                off.avgMs(), off.mn / 1000.0f, off.mx / 1000.0f, bOff.avgMs(), off.n);
  Serial.println(F("Надішли 'r' для повтору."));
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // час відкрити Serial Monitor (USB CDC на S3)

  pinMode(RELAY_PIN, OUTPUT);
  relaySet(false);
  pinMode(CONTACT_PIN, INPUT_PULLUP);
  delay(200);   // реле точно відпущене

  attachInterrupt(digitalPinToInterrupt(CONTACT_PIN), onContact, CHANGE);

  Serial.println(F("Вимірювання часу спрацювання реле, ESP32-S3"));
  runSeries();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' || c == 'R') runSeries();
  }
}
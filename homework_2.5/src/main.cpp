/*
 * Витяжний вентилятор: кожні FAN_PERIOD вмикається на FAN_ON_TIME.
 * ESP32-S3, Arduino core 2.x та 3.x
 *
 * Логіка повністю на esp_timer (незалежно від loop()):
 *   periodicTimer -> кожен період запускає цикл (вмикає реле)
 *   offTimer      -> one-shot, вимикає реле через FAN_ON_TIME
 *   guardTimer    -> раз на секунду перевіряє, що нічого не "залипло"
 *
 * Підключення:
 *   GPIO4 -> 1 кОм -> база NPN, колектор -> IN реле, емітер -> GND
 *   GPIO5 -> 220 Ом -> зелений LED (вентилятор працює)
 *   GPIO6 -> 220 Ом -> червоний LED (очікування)
 *   GND ESP -> "-" рейка бредборда
 *   Реле: COM -> +5V, NO -> мотор, мотор -> GND
 */

#include <Arduino.h>
#include <atomic>
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_idf_version.h"

// ================== НАЛАШТУВАННЯ ==================
#define DEBUG_FAST 1   // 1 = швидкий режим для налагодження

#if DEBUG_FAST
  // 1 год -> 12 с, 15 хв -> 3 с (та сама пропорція 1:4)
  #define FAN_PERIOD_MS   (12UL * 1000UL)
  #define FAN_ON_TIME_MS  (3UL  * 1000UL)
#else
  #define FAN_PERIOD_MS   (60UL * 60UL * 1000UL)   // 1 година
  #define FAN_ON_TIME_MS  (15UL * 60UL * 1000UL)   // 15 хвилин
#endif

#define START_IMMEDIATELY  1      // перший цикл одразу після старту
#define SAFETY_MARGIN_MS   2000UL // допуск для аварійного вимкнення
#define WDT_TIMEOUT_S      5
#define STATUS_PRINT_MS    1000UL

// Піни
#define PIN_RELAY     4   // через NPN-транзистор
#define PIN_LED_ON    5   // зелений: вентилятор працює
#define PIN_LED_IDLE  6   // червоний: очікування

// З NPN-транзистором керування пряме: HIGH на GPIO4 = реле увімкнене
#define RELAY_ACTIVE_LOW    0
// 1 = у стані OFF пін переводиться у вхід (лише для підключення реле напряму, без транзистора)
#define RELAY_OFF_AS_INPUT  0

static_assert(FAN_ON_TIME_MS < FAN_PERIOD_MS, "Час роботи має бути меншим за період");
static_assert(FAN_ON_TIME_MS > 0, "Час роботи має бути > 0");

static const uint64_t PERIOD_US = (uint64_t)FAN_PERIOD_MS  * 1000ULL;
static const uint64_t ON_US     = (uint64_t)FAN_ON_TIME_MS * 1000ULL;

// ================== СТАН ==================
static esp_timer_handle_t periodicTimer = nullptr;
static esp_timer_handle_t offTimer      = nullptr;
static esp_timer_handle_t guardTimer    = nullptr;

static std::atomic<bool>     fanRunning{false};
static std::atomic<int64_t>  fanOnSinceUs{0};
static std::atomic<int64_t>  nextStartUs{0};   // коли буде наступний запуск
static std::atomic<uint32_t> cycleCount{0};
static std::atomic<uint32_t> skippedStarts{0};
static std::atomic<uint32_t> forcedStops{0};

// ================== ЛОГУВАННЯ ==================
static void logState(const char* event) {
  uint32_t t = (uint32_t)(esp_timer_get_time() / 1000ULL);
  Serial.printf("[%8lu ms] %-22s | fan=%s cycles=%lu skipped=%lu forced=%lu\n",
                (unsigned long)t, event,
                fanRunning ? "ON " : "OFF",
                (unsigned long)cycleCount.load(),
                (unsigned long)skippedStarts.load(),
                (unsigned long)forcedStops.load());
}

// ================== АПАРАТНИЙ РІВЕНЬ ==================
static void relayWrite(bool on) {
#if RELAY_ACTIVE_LOW
  #if RELAY_OFF_AS_INPUT
    if (on) {
      pinMode(PIN_RELAY, OUTPUT);
      digitalWrite(PIN_RELAY, LOW);
    } else {
      pinMode(PIN_RELAY, INPUT);   // пін "відпущено", модуль сам підтягує IN до VCC
    }
  #else
    digitalWrite(PIN_RELAY, on ? LOW : HIGH);
  #endif
#else
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
#endif
}

static void applyOutputs(bool on) {
  relayWrite(on);
  digitalWrite(PIN_LED_ON,   on ? HIGH : LOW);
  digitalWrite(PIN_LED_IDLE, on ? LOW  : HIGH);
}

// ================== ЛОГІКА ВЕНТИЛЯТОРА ==================
static void fanStop(const char* reason) {
  // exchange: вимикаємо лише якщо реально був увімкнений
  if (!fanRunning.exchange(false)) return;
  esp_timer_stop(offTimer);            // на випадок аварійної зупинки
  applyOutputs(false);
  logState(reason);
}

static void fanStartCycle() {
  // Захист від повторного запуску: стартуємо лише зі стану OFF
  bool expected = false;
  if (!fanRunning.compare_exchange_strong(expected, true)) {
    skippedStarts++;
    logState("SKIP: already running");
    return;
  }

  fanOnSinceUs = esp_timer_get_time();
  cycleCount++;
  applyOutputs(true);

  esp_err_t err = esp_timer_start_once(offTimer, ON_US);
  if (err != ESP_OK) {
    // Не змогли завести таймер вимкнення — не лишаємо вентилятор увімкненим
    fanRunning = false;
    applyOutputs(false);
    Serial.printf("ERROR: offTimer start failed: %s\n", esp_err_to_name(err));
    return;
  }
  logState("FAN ON");
}

// ================== CALLBACK-и ТАЙМЕРІВ ==================
// Усі callback-и esp_timer виконуються послідовно в одній задачі esp_timer,
// тому між собою вони не конкурують.
static void onPeriod(void*) {
  nextStartUs = esp_timer_get_time() + (int64_t)PERIOD_US;
  fanStartCycle();
}

static void onOff(void*) { fanStop("FAN OFF (timeout)"); }

static void onGuard(void*) {
  // 1) Аварійне вимкнення, якщо реле "залипло" довше дозволеного
  if (fanRunning) {
    int64_t onMs = (esp_timer_get_time() - fanOnSinceUs.load()) / 1000;
    if (onMs > (int64_t)(FAN_ON_TIME_MS + SAFETY_MARGIN_MS)) {
      forcedStops++;
      fanStop("FORCED OFF (guard)");
    }
  }
  // 2) Періодичний таймер має бути активним — інакше перезапускаємо
  if (!esp_timer_is_active(periodicTimer)) {
    esp_timer_start_periodic(periodicTimer, PERIOD_US);
    nextStartUs = esp_timer_get_time() + (int64_t)PERIOD_US;
    logState("RESTART periodic timer");
  }
}

// ================== ІНІЦІАЛІЗАЦІЯ ==================
static esp_timer_handle_t makeTimer(esp_timer_cb_t cb, const char* name) {
  esp_timer_create_args_t args = {};
  args.callback        = cb;
  args.arg             = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name            = name;
  esp_timer_handle_t h = nullptr;
  ESP_ERROR_CHECK(esp_timer_create(&args, &h));
  return h;
}

static void createTimers() {
  periodicTimer = makeTimer(&onPeriod, "fan_period");
  offTimer      = makeTimer(&onOff,    "fan_off");
  guardTimer    = makeTimer(&onGuard,  "fan_guard");

  ESP_ERROR_CHECK(esp_timer_start_periodic(periodicTimer, PERIOD_US));
  nextStartUs = esp_timer_get_time() + (int64_t)PERIOD_US;
  ESP_ERROR_CHECK(esp_timer_start_periodic(guardTimer, 1000ULL * 1000ULL)); // 1 с
}

static void setupWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  // Arduino core 3.x (ESP-IDF 5.x)
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&cfg);
  }
#else
  // Arduino core 2.x (ESP-IDF 4.4): якщо WDT вже запущений, init оновлює налаштування
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);   // підписуємо loopTask
}

void setup() {
  // Спочатку безпечний стан виходів — до будь-якої логіки
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_LED_ON, OUTPUT);
  pinMode(PIN_LED_IDLE, OUTPUT);
  applyOutputs(false);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) { /* чекаємо USB CDC, без delay */ }

  Serial.printf("\nFan controller: period=%lu ms, on=%lu ms, reset reason=%d\n",
                (unsigned long)FAN_PERIOD_MS, (unsigned long)FAN_ON_TIME_MS,
                (int)esp_reset_reason());

  setupWatchdog();
  createTimers();

#if START_IMMEDIATELY
  fanStartCycle();
#endif
  logState("BOOT DONE");
}

void loop() {
  // loop() не керує вентилятором — лише watchdog і статус
  esp_task_wdt_reset();

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= STATUS_PRINT_MS) {
    lastPrint = millis();
    int64_t now = esp_timer_get_time();
    if (fanRunning) {
      int64_t left = (int64_t)FAN_ON_TIME_MS - (now - fanOnSinceUs.load()) / 1000;
      Serial.printf("  status: ON, ~%lld ms до вимкнення\n", (long long)left);
    } else {
      int64_t next = (nextStartUs.load() - now) / 1000;
      Serial.printf("  status: IDLE, ~%lld ms до запуску\n", (long long)next);
    }
  }
}
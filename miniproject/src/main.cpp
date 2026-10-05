// Сутінковий перемикач на ESP32-S3 (Arduino framework)
//
// GPIO4 (ADC) <- середня точка дільника R1 (LDR) / R2 10k
// GPIO5       -> R3 10k -> база VT1 (BC547B) -> IN модуля реле
// GPIO1       <- кнопка на GND (внутрішня підтяжка)
//
// Кнопка перемикає режими по колу: АВТО -> УВІМК -> ВИМК -> АВТО
//
// Індикація режиму світлодіодами на платі (PWR горить завжди, TX керуємо):
//   АВТО  - горить 1 (тільки PWR)
//   УВІМК - горять 2 (PWR + TX)
//   ВИМК  - TX блимає
// Світлодіод RX не використовуємо: його лінію тримає мікросхема USB-UART.

#include <Arduino.h>

const int PIN_LDR    = 4;   // GPIO In, ADC1_CH3
const int PIN_RELAY  = 5;   // GPIO Out
const int PIN_BUTTON = 1;   // натиснута = LOW

// Світлодіод TX сидить на лінії UART0 і світиться, коли на лінії LOW.
const int PIN_LED_TX = 43;
const int LED_ON     = LOW;
const int LED_OFF    = HIGH;

// Пороги підбери під свою кімнату: подивись ADC у Serial Monitor
// при світлі та коли фоторезистор закритий рукою.
const int THRESHOLD_DARK  = 2200;  // нижче — темно, вмикаємо
const int THRESHOLD_LIGHT = 2700;  // вище — світло, вимикаємо

const unsigned long DEBOUNCE_MS = 30;
const unsigned long PRINT_MS    = 200;
const unsigned long BLINK_MS    = 300;

// GPIO43 зайнятий світлодіодом, тому лог іде тільки через рідний USB
// (Tools -> USB CDC On Boot -> Enabled). Без цього Serial = UART0 на тому
// самому піні, і друк вимикається, щоб не збивати індикацію.
#if ARDUINO_USB_CDC_ON_BOOT
  #define LOG_BEGIN()  Serial.begin(115200)
  #define LOG(...)     Serial.printf(__VA_ARGS__)
#else
  #define LOG_BEGIN()
  #define LOG(...)
#endif

enum Mode { MODE_AUTO, MODE_ON, MODE_OFF };
const char* const MODE_NAME[] = { "AUTO", "ON", "OFF" };

Mode mode = MODE_AUTO;
bool relayOn = false;

int btnStable = HIGH;            // стан після антибрязкоту
int btnLastRaw = HIGH;
unsigned long btnChangedAt = 0;
unsigned long printedAt = 0;

void setRelay(bool on) {
  relayOn = on;
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
}

// АВТО - TX погашений, УВІМК - горить, ВИМК - блимає
void updateModeLed() {
  bool on = false;
  if (mode == MODE_ON) {
    on = true;
  } else if (mode == MODE_OFF) {
    on = (millis() / BLINK_MS) % 2 == 0;
  }
  digitalWrite(PIN_LED_TX, on ? LED_ON : LED_OFF);
}

// true один раз на кожне натискання
bool buttonPressed() {
  int raw = digitalRead(PIN_BUTTON);
  if (raw != btnLastRaw) {
    btnLastRaw = raw;
    btnChangedAt = millis();
  }
  if (raw != btnStable && millis() - btnChangedAt >= DEBOUNCE_MS) {
    btnStable = raw;
    return btnStable == LOW;
  }
  return false;
}

void setup() {
  LOG_BEGIN();

  analogReadResolution(12);          // 0...4095
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RELAY, OUTPUT);
  setRelay(false);

  pinMode(PIN_LED_TX, OUTPUT);
  updateModeLed();
}

void loop() {
  // Кнопка: наступний режим
  if (buttonPressed()) {
    mode = (Mode)((mode + 1) % 3);
    LOG("MODE -> %s\n", MODE_NAME[mode]);
  }

  updateModeLed();

  int adc = analogRead(PIN_LDR);

  if (mode == MODE_ON) {
    setRelay(true);
  } else if (mode == MODE_OFF) {
    setRelay(false);
  } else {
    // АВТО: пороги з гістерезисом
    if (adc < THRESHOLD_DARK) {
      setRelay(true);                // темно -> увімкнути
    } else if (adc > THRESHOLD_LIGHT) {
      setRelay(false);               // світло -> вимкнути
    }
    // між порогами нічого не чіпаємо
  }

  // Відладка
  if (millis() - printedAt >= PRINT_MS) {
    printedAt = millis();
    LOG("MODE=%-4s ADC=%4d  GPIO_OUT=%s\n",
        MODE_NAME[mode], adc, relayOn ? "HIGH" : "LOW");
  }

  delay(5);
}
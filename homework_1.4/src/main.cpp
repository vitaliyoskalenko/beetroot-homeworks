#include <Arduino.h>

// --- Піни ---
const int LED1 = 1;       // світлодіод 1 (через резистор 220 Ом)
const int LED2 = 2;       // світлодіод 2 (через резистор 220 Ом)
const int BTN_EXT = 21;   // зовнішня кнопка: GPIO21 <-> GND
const int BTN_BOOT = 0;   // вбудована кнопка BOOT

int mode = 1;             // за замовчуванням Режим №1

// Перевіряє кнопки (натиснута = LOW) і перемикає режим
void checkButtons() {
  if (digitalRead(BTN_EXT) == LOW && mode != 1) {
    mode = 1;
    Serial.println("Режим 1: синхронно, 200 мс");
  }
  if (digitalRead(BTN_BOOT) == LOW && mode != 2) {
    mode = 2;
    Serial.println("Режим 2: по черзі, 1000 мс");
  }
}

// Затримка, під час якої ми продовжуємо опитувати кнопки,
// щоб натискання не губилось навіть під час delay(1000)
void waitMs(int ms) {
  for (int t = 0; t < ms; t += 10) {
    checkButtons();
    delay(10);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);
  pinMode(BTN_EXT, INPUT_PULLUP);  // внутрішня підтяжка до 3.3V
  pinMode(BTN_BOOT, INPUT);        // на платі вже є зовнішня підтяжка

  Serial.println("Старт. Режим 1");
}

void loop() {
  if (mode == 1) {
    // Режим 1: обидва разом, швидко
    digitalWrite(LED1, HIGH);
    digitalWrite(LED2, HIGH);
    waitMs(200);
    digitalWrite(LED1, LOW);
    digitalWrite(LED2, LOW);
    waitMs(200);
  } else {
    // Режим 2: по черзі, повільно (залізничний переїзд)
    digitalWrite(LED1, HIGH);
    digitalWrite(LED2, LOW);
    waitMs(1000);
    digitalWrite(LED1, LOW);
    digitalWrite(LED2, HIGH);
    waitMs(1000);
  }
}
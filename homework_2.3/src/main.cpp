#include <Arduino.h>

/*
 * Superloop: 3 незалежні світлодіоди без delay()
 * Плата: ESP32-S3 DevKitC-1 (Arduino core for ESP32)
 *
 * LED1 (GPIO4) — перемикається кожні 200 мс
 * LED2 (GPIO5) — перемикається кожні 500 мс
 * LED3 (GPIO6) — перемикається кожні 1000 мс
 *
 * Кожен LED — окрема "задача" зі своїм таймером.
 * loop() ніколи не блокується: просто перевіряє, чи настав час.
 */

struct BlinkTask {
  uint8_t pin;                 // GPIO світлодіода
  unsigned long interval;      // період перемикання, мс
  unsigned long lastToggle;    // коли востаннє перемикали
  bool state;                  // поточний стан LED
  const char* name;
};

BlinkTask tasks[] = {
  {4,  200, 0, false, "LED1"},
  {5,  500, 0, false, "LED2"},
  {6, 1000, 0, false, "LED3"},
};

const uint8_t TASK_COUNT = sizeof(tasks) / sizeof(tasks[0]);

void updateTask(BlinkTask& t, unsigned long now) {
  // Віднімання безпечне при переповненні millis() (~49 днів)
  if (now - t.lastToggle >= t.interval) {
    t.lastToggle += t.interval;   // += замість = now, щоб період не "плив"
    t.state = !t.state;
    digitalWrite(t.pin, t.state ? HIGH : LOW);

    Serial.printf("[%lu ms] %s %s\n", now, t.name, t.state ? "ON" : "OFF");
  }
}

void setup() {
  Serial.begin(115200);
  for (uint8_t i = 0; i < TASK_COUNT; i++) {
    pinMode(tasks[i].pin, OUTPUT);
    digitalWrite(tasks[i].pin, LOW);
  }
  Serial.println("Superloop started (ESP32-S3)");
}

void loop() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < TASK_COUNT; i++) {
    updateTask(tasks[i], now);
  }
}
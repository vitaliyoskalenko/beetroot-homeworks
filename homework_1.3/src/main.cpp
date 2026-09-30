#include <Arduino.h>

const int RED   = 1;
const int BLUE  = 2;
const int GREEN = 42;

const int leds[] = {RED, BLUE, GREEN};
const int N = 3;

void setLed(int pin, int value) { analogWrite(pin, value); }  // 0..255
void allSet(int value) { for (int i = 0; i < N; i++) setLed(leds[i], value); }
void allOff() { allSet(0); }

void flash(int pin, int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    setLed(pin, 255); delay(onMs);
    setLed(pin, 0);   delay(offMs);
  }
}

// 1. Поліцейська мигалка: червоне-синє стробоскопічне миготіння, зелена спалах наприкінці
void police() {
  for (int r = 0; r < 4; r++) {
    flash(RED, 3, 50, 50);
    flash(BLUE, 3, 50, 50);
  }
  flash(GREEN, 2, 80, 80);
}

// 2. Бігучий вогонь туди-назад
void chaser() {
  for (int r = 0; r < 5; r++) {
    for (int i = 0; i < N; i++)      flash(leds[i], 1, 110, 0);
    for (int i = N - 2; i > 0; i--)  flash(leds[i], 1, 110, 0);
  }
}

// 3. Заповнення: світлодіоди загоряються по одному та гаснуть у зворотному порядку
void fill() {
  for (int r = 0; r < 3; r++) {
    for (int i = 0; i < N; i++)       { setLed(leds[i], 255); delay(200); }
    delay(300);
    for (int i = N - 1; i >= 0; i--)  { setLed(leds[i], 0);   delay(200); }
    delay(200);
  }
}

// 4. Двійковий лічильник від 0 до 7
void binaryCounter() {
  for (int n = 0; n < 8; n++) {
    for (int i = 0; i < N; i++) setLed(leds[i], (n >> i) & 1 ? 255 : 0);
    delay(400);
  }
  allOff();
}

// 5. Плавне «дихання» по черзі
void breathe() {
  for (int r = 0; r < 2; r++) {
    for (int i = 0; i < N; i++) {
      for (int v = 0; v <= 255; v += 5)  { setLed(leds[i], v); delay(6); }
      for (int v = 255; v >= 0; v -= 5)  { setLed(leds[i], v); delay(6); }
    }
  }
}

// 6. Серцебиття: подвійний спалах усіма світлодіодами одночасно
void heartbeat() {
  for (int r = 0; r < 4; r++) {
    allSet(255); delay(90);
    allOff();    delay(110);
    allSet(255); delay(90);
    allOff();    delay(600);
  }
}

// 7. Іскри: випадкові короткі спалахи
void sparkle() {
  for (int r = 0; r < 30; r++) {
    flash(leds[random(N)], 1, 30, random(30, 150));
  }
}

void setup() {
  for (int i = 0; i < N; i++) pinMode(leds[i], OUTPUT);
  randomSeed(analogRead(4));
}

void loop() {
  police();        allOff(); delay(400);
  chaser();        allOff(); delay(400);
  fill();          allOff(); delay(400);
  binaryCounter(); allOff(); delay(400);
  breathe();       allOff(); delay(400);
  heartbeat();     allOff(); delay(400);
  sparkle();       allOff(); delay(400);
}

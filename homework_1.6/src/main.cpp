#include <Arduino.h>

const int   LDR_PIN  = 1;       // GPIO1 = ADC1_CH0
const float ADC_MAX  = 4095.0;
const float U_REF_MV = 3100.0;  // Uref = 3.1 В
const unsigned long PERIOD_MS = 100;

unsigned long lastRead = 0;
int rowCount = 0;

void printHeader() {
  Serial.println();
  Serial.println("+--------+-------------+-------------+----------+");
  Serial.println("|  RAW   | Ucalc, мВ   | Umeas, мВ   | Похибка  |");
  Serial.println("+--------+-------------+-------------+----------+");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);        // 0..4095
  analogSetAttenuation(ADC_11db);  // діапазон ~0..3.1 В
  printHeader();
}

void loop() {
  if (millis() - lastRead < PERIOD_MS) return;
  lastRead = millis();

  int   raw   = analogRead(LDR_PIN);            // RAW з АЦП
  float uCalc = raw / ADC_MAX * U_REF_MV;       // Ucalc = RAW / ADCmax * Uref
  int   uMeas = analogReadMilliVolts(LDR_PIN);  // напруга з калібруванням

  float err = (uMeas > 0) ? fabs(uCalc - uMeas) / uMeas * 100.0 : 0.0;

  Serial.printf("| %6d | %11.1f | %11d | %7.2f%% |\n", raw, uCalc, uMeas, err);

  if (++rowCount % 20 == 0) printHeader();
}
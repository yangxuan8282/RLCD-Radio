#include <stdio.h>
#include <Arduino.h>

#include "adc_bsp.h"

static constexpr uint8_t BATTERY_ADC_PIN = 4;
static constexpr float BATTERY_DIVIDER_RATIO = 3.0f;

void Adc_PortInit(void) {
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
}

float Adc_GetBatteryVoltage(int *data) {
  int value = analogRead(BATTERY_ADC_PIN);
  float vol = analogReadMilliVolts(BATTERY_ADC_PIN) * 0.001f * BATTERY_DIVIDER_RATIO;
  if (data) {
    *data = value;
  }
  return vol;
}

uint8_t Adc_GetBatteryLevel(void) {
  float vol = Adc_GetBatteryVoltage(NULL);
  if (vol < 3.0) {
    return 0;
  }
  if (vol > 4.12) {
    return 100;
  }
  float level = ((vol - 3.0) / 1.12) * 100;
  return (uint8_t)level;
}

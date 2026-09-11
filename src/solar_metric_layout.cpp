#include "solar_metric_layout.h"

SolarOptionalMetricSlots solar_optional_metric_slots(uint8_t supplied) {
  SolarOptionalMetricSlots slots;
  int8_t next = 1;  // slot zero is always today's total forecast
  if ((supplied & SOLAR_FIGURE_REMAINING) != 0) {
    slots.remaining = next++;
  }
  if ((supplied & SOLAR_FIGURE_VS_FORECAST) != 0) {
    slots.vs_forecast = next++;
  }
  if ((supplied & SOLAR_FIGURE_PEAK) != 0) {
    slots.peak = next;
  }
  return slots;
}

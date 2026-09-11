#include "solar_metric_layout.h"

SolarOptionalMetricSlots solar_optional_metric_slots(const Snapshot& snapshot) {
  SolarOptionalMetricSlots slots;
  if (!snapshot.valid || !snapshot.solar.configured) {
    return slots;
  }

  int8_t next = 1;  // slot zero is always today's total forecast
  if (snapshot.solar.remaining_kwh.known) {
    slots.remaining = next++;
  }
  if (snapshot.solar.vs_forecast_pct.known) {
    slots.vs_forecast = next++;
  }
  if (snapshot.solar.peak_kw.known) {
    slots.peak = next;
  }
  return slots;
}

#pragma once

#include <stdint.h>

#include "snapshot.h"

// Slots are packed left-to-right, top-to-bottom after today's total forecast.
// An unknown optional value has no slot, so the Solar screen does not leave a
// blank row behind for information the selected source cannot supply.
struct SolarOptionalMetricSlots {
  static constexpr int8_t Hidden = -1;

  int8_t remaining = Hidden;
  int8_t vs_forecast = Hidden;
  int8_t peak = Hidden;
};

SolarOptionalMetricSlots solar_optional_metric_slots(const Snapshot& snapshot);

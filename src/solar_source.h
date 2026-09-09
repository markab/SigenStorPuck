#pragma once

#include <stdint.h>

#include "data_source.h"

// Persisted only as the Home Assistant forecast preference. Direct Modbus
// always uses the Puck forecast and Server always keeps the forecast in its
// summary payload, regardless of this setting.
enum class SolarForecastSource : uint8_t {
  Disabled = 0,
  Puck = 1,
  HomeAssistant = 2,
};

// Missing or unknown values preserve the original HA behaviour: no forecast.
SolarForecastSource solar_forecast_source_from_stored(uint8_t value);
const char* solar_forecast_source_name(SolarForecastSource source);

bool solar_forecast_uses_puck(DataSource source, SolarForecastSource ha_source);
bool solar_forecast_uses_home_assistant(DataSource source,
                                        SolarForecastSource ha_source);

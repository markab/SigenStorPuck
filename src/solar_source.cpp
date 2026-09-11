#include "solar_source.h"

static_assert(static_cast<uint8_t>(SolarForecastSource::Disabled) == 0,
              "persisted solar source changed");
static_assert(static_cast<uint8_t>(SolarForecastSource::Puck) == 1,
              "persisted solar source changed");
static_assert(static_cast<uint8_t>(SolarForecastSource::HomeAssistant) == 2,
              "persisted solar source changed");

SolarForecastSource solar_forecast_source_from_stored(uint8_t value) {
  switch (value) {
    case 1:
      return SolarForecastSource::Puck;
    case 2:
      return SolarForecastSource::HomeAssistant;
    default:
      return SolarForecastSource::Disabled;
  }
}

const char* solar_forecast_source_name(SolarForecastSource source) {
  switch (source) {
    case SolarForecastSource::Disabled:
      return "disabled";
    case SolarForecastSource::Puck:
      return "puck";
    case SolarForecastSource::HomeAssistant:
      return "home assistant";
  }
  return "disabled";
}

bool solar_forecast_uses_puck(DataSource source, SolarForecastSource ha_source) {
  return source == DataSource::Modbus ||
         (source == DataSource::HomeAssistant && ha_source == SolarForecastSource::Puck);
}

bool solar_forecast_uses_home_assistant(DataSource source,
                                        SolarForecastSource ha_source) {
  return source == DataSource::HomeAssistant &&
         ha_source == SolarForecastSource::HomeAssistant;
}

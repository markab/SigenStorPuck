#include "solar_source.h"

#include "solar_metric_layout.h"

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

uint8_t solar_figures_supplied(DataSource source, SolarForecastSource ha_source,
                               bool ha_remaining_mapped, bool ha_percentage_mapped,
                               bool ha_peak_mapped) {
  if (source != DataSource::HomeAssistant) {
    return SOLAR_FIGURES_ALL;
  }
  switch (ha_source) {
    case SolarForecastSource::Puck:
      return SOLAR_FIGURES_ALL;
    case SolarForecastSource::HomeAssistant:
      return static_cast<uint8_t>((ha_remaining_mapped ? SOLAR_FIGURE_REMAINING : 0) |
                                  (ha_percentage_mapped ? SOLAR_FIGURE_VS_FORECAST : 0) |
                                  (ha_peak_mapped ? SOLAR_FIGURE_PEAK : 0));
    case SolarForecastSource::Disabled:
      break;
  }
  return 0;
}

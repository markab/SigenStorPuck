#include "data_source.h"

static_assert(static_cast<uint8_t>(DataSource::Server) == 0, "persisted source changed");
static_assert(static_cast<uint8_t>(DataSource::Modbus) == 1, "persisted source changed");
static_assert(static_cast<uint8_t>(DataSource::HomeAssistant) == 2,
              "Home Assistant must remain appended");

DataSource data_source_from_stored(uint8_t value) {
  switch (value) {
    case 1:
      return DataSource::Modbus;
    case 2:
      return DataSource::HomeAssistant;
    default:
      return DataSource::Server;
  }
}

DataSource data_source_restore(bool has_stored_value, uint8_t stored_value,
                               bool has_legacy_server_credentials) {
  if (has_stored_value) {
    return data_source_from_stored(stored_value);
  }
  return has_legacy_server_credentials ? DataSource::Server : DataSource::Modbus;
}

const char* data_source_name(DataSource source) {
  switch (source) {
    case DataSource::Server:
      return "server";
    case DataSource::Modbus:
      return "modbus";
    case DataSource::HomeAssistant:
      return "home assistant";
  }
  return "server";
}

SourceCapabilities data_source_capabilities(DataSource source) {
  SourceCapabilities capabilities;
  capabilities.live = true;
  capabilities.daily_totals = true;
  capabilities.local_history = true;

  switch (source) {
    case DataSource::Server:
      capabilities.historical_days = true;
      capabilities.full_day_series = true;
      capabilities.forecast = true;
      capabilities.tariff_cost = true;
      capabilities.detailed_flows = true;
      break;
    case DataSource::Modbus:
      capabilities.forecast = true;
      break;
    case DataSource::HomeAssistant:
      break;
  }
  return capabilities;
}

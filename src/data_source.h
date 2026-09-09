#pragma once

#include <stdint.h>

// Persisted in NVS. Values are append-only: changing either existing value would
// silently switch the source on installed devices.
enum class DataSource : uint8_t {
  Server = 0,
  Modbus = 1,
  HomeAssistant = 2,
};

struct SourceCapabilities {
  bool live = false;
  bool daily_totals = false;
  bool local_history = false;
  bool historical_days = false;
  bool full_day_series = false;
  bool forecast = false;
  bool tariff_cost = false;
  bool detailed_flows = false;
};

// Unknown stored values retain the pre-HA fallback to Server. This is separate
// from the fresh-install default, which remains Modbus in Settings.
DataSource data_source_from_stored(uint8_t value);
// Reproduces settings migration: before the source key existed, stored server
// credentials meant Server; a genuinely fresh namespace keeps the Modbus default.
DataSource data_source_restore(bool has_stored_value, uint8_t stored_value,
                               bool has_legacy_server_credentials);
const char* data_source_name(DataSource source);
SourceCapabilities data_source_capabilities(DataSource source);

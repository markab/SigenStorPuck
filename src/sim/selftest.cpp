// Checks for acquisition and shared logic that do not need a plant or HA instance.
//
// Run with `.pio/build/sim/program --selftest`. Desktop-only, like the rest of
// src/sim/, but everything it exercises is shared with the firmware — which is
// the reason modbus_regs.cpp and history.cpp live outside src/device/.
//
// What is deliberately *not* covered, because only real hardware can settle it:
// whether Sigenergy's reversed function codes are the right way round on this
// firmware, whether the inverter tolerates a second Modbus client alongside the
// Pi, and the sign convention of plant_active_power.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "button_gesture.h"
#include "day_series.h"
#include "history.h"
#include "history_backfill_retry.h"
#include "home_assistant.h"
#include "home_assistant_history.h"
#include "data_source.h"
#include "modbus_regs.h"
#include "screen_window.h"
#include "solar_forecast.h"
#include "solar_metric_layout.h"
#include "solar_source.h"

namespace {

int s_failures = 0;
int s_checks = 0;

void check(bool condition, const char* what) {
  ++s_checks;
  if (!condition) {
    ++s_failures;
    printf("  FAIL  %s\n", what);
  }
}

void check_near(float actual, float expected, const char* what) {
  ++s_checks;
  if (fabsf(actual - expected) > 0.005f) {
    ++s_failures;
    printf("  FAIL  %s (got %.4f, wanted %.4f)\n", what, actual, expected);
  }
}

// A proportional tolerance, for figures compared against a reference computed in
// double precision. The firmware is float throughout — an S3 has hardware FP32
// and nothing else — so a day's worth of accumulated slots cannot match to the
// absolute 5 mWh check_near() wants.
void check_within(float actual, float expected, float fraction, const char* what) {
  ++s_checks;
  if (fabsf(actual - expected) > fabsf(expected) * fraction) {
    ++s_failures;
    printf("  FAIL  %s (got %.5f, wanted %.5f)\n", what, actual, expected);
  }
}

void test_data_sources() {
  printf("data sources\n");
  check(static_cast<uint8_t>(DataSource::Server) == 0, "server persisted value remains 0");
  check(static_cast<uint8_t>(DataSource::Modbus) == 1, "modbus persisted value remains 1");
  check(static_cast<uint8_t>(DataSource::HomeAssistant) == 2, "HA is appended as value 2");
  check(data_source_from_stored(0) == DataSource::Server, "stored server decodes");
  check(data_source_from_stored(1) == DataSource::Modbus, "stored modbus decodes");
  check(data_source_from_stored(2) == DataSource::HomeAssistant, "stored HA decodes");
  check(data_source_from_stored(99) == DataSource::Server,
        "unknown stored source preserves legacy server fallback");
  check(data_source_restore(false, 0, false) == DataSource::Modbus,
        "older empty NVS retains fresh-install Modbus default");
  check(data_source_restore(false, 0, true) == DataSource::Server,
        "older NVS with server credentials remains on Server");
  check(data_source_restore(true, 1, true) == DataSource::Modbus,
        "explicit stored source wins over legacy credential inference");

  const SourceCapabilities server = data_source_capabilities(DataSource::Server);
  const SourceCapabilities modbus = data_source_capabilities(DataSource::Modbus);
  const SourceCapabilities ha = data_source_capabilities(DataSource::HomeAssistant);
  check(server.historical_days && server.full_day_series && server.detailed_flows &&
            server.tariff_cost,
        "server capabilities include history and detailed features");
  check(modbus.live && modbus.daily_totals && modbus.local_history && modbus.forecast &&
            !modbus.historical_days && !modbus.detailed_flows,
        "modbus capabilities are local/live");
  check(ha.live && ha.daily_totals && ha.local_history && ha.today_history_backfill &&
            ha.forecast &&
            !ha.historical_days && !ha.full_day_series && !ha.detailed_flows &&
            !ha.tariff_cost,
        "HA capabilities include optional today backfill but not past days or detail");

  check(static_cast<uint8_t>(SolarForecastSource::Disabled) == 0,
        "disabled forecast source persists as 0");
  check(static_cast<uint8_t>(SolarForecastSource::Puck) == 1,
        "Puck forecast source persists as 1");
  check(static_cast<uint8_t>(SolarForecastSource::HomeAssistant) == 2,
        "HA forecast source persists as 2");
  check(solar_forecast_source_from_stored(0) == SolarForecastSource::Disabled,
        "stored disabled forecast source decodes");
  check(solar_forecast_source_from_stored(1) == SolarForecastSource::Puck,
        "stored Puck forecast source decodes");
  check(solar_forecast_source_from_stored(2) == SolarForecastSource::HomeAssistant,
        "stored HA forecast source decodes");
  check(solar_forecast_source_from_stored(99) == SolarForecastSource::Disabled,
        "missing or unknown forecast setting preserves disabled behaviour");
  check(solar_forecast_uses_puck(DataSource::Modbus, SolarForecastSource::Disabled),
        "Modbus keeps its native forecast regardless of the HA preference");
  check(!solar_forecast_uses_puck(DataSource::Server, SolarForecastSource::Puck),
        "Server forecast is never replaced by the Puck cache");
  check(solar_forecast_uses_puck(DataSource::HomeAssistant, SolarForecastSource::Puck),
        "HA can select the native forecast cache");
  check(!solar_forecast_uses_puck(DataSource::HomeAssistant,
                                  SolarForecastSource::HomeAssistant),
        "HA entity forecast does not invoke the native forecast");
  check(solar_forecast_uses_home_assistant(DataSource::HomeAssistant,
                                           SolarForecastSource::HomeAssistant),
        "HA entity forecast is selected only for HA acquisition");
  check(!solar_forecast_uses_home_assistant(DataSource::Modbus,
                                            SolarForecastSource::HomeAssistant),
        "HA forecast preference cannot change Modbus acquisition");
}

void test_home_assistant_template() {
  printf("home assistant template\n");
  check(ha_entity_id_valid("sensor.pv_power"), "normal entity id accepted");
  check(ha_entity_id_valid("binary_sensor.off_grid_2"), "binary entity id accepted");
  check(!ha_entity_id_valid("sensor.PV Power"), "unsafe entity id rejected");
  check(!ha_entity_id_valid("sensor"), "entity id needs a domain separator");
  check(!ha_entity_id_valid("sensor.bad' }}"), "template injection rejected");

  const char* entities[HA_ENTITY_COUNT] = {};
  entities[static_cast<size_t>(HaEntity::PvPower)] = "sensor.pv_power";
  entities[static_cast<size_t>(HaEntity::BatterySoc)] = "sensor.battery_soc";
  char output[HA_TEMPLATE_MAX];
  check(ha_template_build(entities, output, sizeof(output)), "template builds");
  check(strstr(output, "states('sensor.pv_power')") != nullptr,
        "template references configured PV only");
  check(strstr(output, "states('sensor.battery_soc')") != nullptr,
        "template references configured SOC");
  check(strstr(output, "as_timestamp(today_at())") != nullptr &&
            strstr(output, "timedelta(days=1)") != nullptr,
        "template asks HA for DST-aware local day boundaries");
  check(strstr(output, "\"gp\"") == nullptr, "unconfigured grid omitted");
  check(strstr(output, "Authorization") == nullptr, "template contains no credentials");

  entities[static_cast<size_t>(HaEntity::ForecastToday)] =
      "sensor.solar_forecast_today";
  check(ha_template_build(entities, output, sizeof(output)),
        "live and forecast mappings share one template");
  check(strstr(output, "states('sensor.solar_forecast_today')") != nullptr,
        "template includes configured forecast entity");

  char longest_id[HA_ENTITY_ID_MAX + 1];
  memcpy(longest_id, "sensor.", 7);
  memset(longest_id + 7, 'a', HA_ENTITY_ID_MAX - 7);
  longest_id[HA_ENTITY_ID_MAX] = '\0';
  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    entities[i] = longest_id;
  }
  check(ha_template_build(entities, output, sizeof(output)),
        "template buffer holds every maximum-length mapping");

  char too_small[32];
  check(!ha_template_build(entities, too_small, sizeof(too_small)),
        "short template buffer is rejected");
}

void test_home_assistant_settings_metadata() {
  printf("home assistant settings metadata\n");
  const char* expected_form_order[HA_ENTITY_COUNT] = {
      "ha_pp",  "ha_gp",  "ha_bp",  "ha_hp",  "ha_ep",  "ha_xp",
      "ha_og",  "ha_soc", "ha_soh", "ha_cap", "ha_tmp", "ha_dpv",
      "ha_dld", "ha_dim", "ha_dex", "ha_dch", "ha_dds", "ha_sft",
      "ha_sfr", "ha_sfp", "ha_sfk",
  };
  const char* expected_placeholders[HA_ENTITY_COUNT] = {
      "sensor.sigen_plant_pv_power",
      "sensor.sigen_plant_grid_active_power",
      "sensor.sigen_plant_battery_power",
      "sensor.sigen_plant_consumed_power",
      "sensor.sigen_inverter_dc_charger_output_power",
      "sensor.sigen_plant_plant_active_power",
      "sensor.sigen_plant_grid_connection_status",
      "sensor.sigen_plant_battery_state_of_charge",
      "sensor.sigen_plant_battery_state_of_health",
      "sensor.sigen_plant_rated_energy_capacity",
      "sensor.sigen_inverter_battery_average_cell_temperature",
      "sensor.sigen_plant_pv_daily_generation",
      "sensor.sigen_plant_daily_load_consumption",
      "sensor.sigen_plant_daily_grid_import_energy",
      "sensor.sigen_plant_daily_grid_export_energy",
      "sensor.sigen_plant_daily_battery_charge_energy",
      "sensor.sigen_plant_daily_battery_discharge_energy",
      "sensor.example_solar_forecast_today",
      "sensor.example_solar_forecast_remaining",
      "sensor.example_solar_forecast_percentage",
      "sensor.example_solar_forecast_peak",
  };

  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    check(strcmp(HA_ENTITIES[i].form_name, expected_form_order[i]) == 0,
          "HA settings fields retain semantic tab order");
    check(strcmp(HA_ENTITIES[i].placeholder, expected_placeholders[i]) == 0,
          "HA settings field has its example placeholder");
  }

  // Placeholders are presentation metadata, not entity defaults. The API
  // template is built only from the values supplied by persisted settings.
  const char* empty[HA_ENTITY_COUNT] = {};
  char output[HA_TEMPLATE_MAX];
  check(ha_template_build(empty, output, sizeof(output)),
        "empty mappings still produce a valid base template");
  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    check(strstr(output, HA_ENTITIES[i].placeholder) == nullptr,
          "placeholder is not treated as a configured mapping");
  }

  const char* configured[HA_ENTITY_COUNT] = {};
  configured[static_cast<size_t>(HaEntity::PvPower)] = "sensor.persisted_pv_power";
  check(ha_template_build(configured, output, sizeof(output)),
        "persisted mapping builds a template");
  check(strstr(output, "states('sensor.persisted_pv_power')") != nullptr,
        "persisted mapping remains the actual entity value");
  check(strstr(output, HA_ENTITIES[static_cast<size_t>(HaEntity::PvPower)].placeholder) == nullptr,
        "persisted mapping is not replaced by its placeholder");
}

void test_home_assistant_forecast() {
  printf("home assistant forecast\n");
  Snapshot snapshot;
  HaParseInfo info;
  const char* converted =
      "{\"v\":1,\"ts\":1788970000,"
      "\"pp\":{\"s\":\"1.5\",\"u\":\"kW\"},"
      "\"sft\":{\"s\":\"12.5\",\"u\":\"kWh\"},"
      "\"sfr\":{\"s\":\"3200\",\"u\":\"Wh\"},"
      "\"sfp\":{\"s\":\"108\",\"u\":\"%\"},"
      "\"sfk\":{\"s\":\"6500\",\"u\":\"W\"}}";
  check(ha_payload_parse(converted, strlen(converted), &snapshot, &info),
        "HA forecast payload parses");
  check(snapshot.solar.configured, "known total makes HA forecast configured");
  check_near(snapshot.solar.forecast_kwh.value, 12.5f, "forecast accepts kWh");
  check_near(snapshot.solar.remaining_kwh.value, 3.2f, "forecast converts Wh to kWh");
  check_near(snapshot.solar.vs_forecast_pct.value, 108.0f, "forecast accepts percentage");
  check_near(snapshot.solar.peak_kw.value, 6.5f, "forecast converts W to kW");

  const char* direct_peak =
      "{\"v\":1,\"ts\":1788970000,"
      "\"sft\":{\"s\":\"0\",\"u\":\"Wh\"},"
      "\"sfp\":{\"s\":\"0\",\"u\":\"%\"},"
      "\"sfk\":{\"s\":\"4.2\",\"u\":\"kW\"}}";
  check(ha_payload_parse(direct_peak, strlen(direct_peak), &snapshot, &info),
        "zero and direct-kW forecast payload parses");
  check(snapshot.solar.configured && snapshot.solar.forecast_kwh.known &&
            snapshot.solar.forecast_kwh.value == 0.0f,
        "genuine zero forecast remains known and configured");
  check_near(snapshot.solar.peak_kw.value, 4.2f, "forecast accepts kW");
  check(snapshot.solar.vs_forecast_pct.known && snapshot.solar.vs_forecast_pct.value == 0.0f,
        "genuine zero forecast percentage remains known");
  check(!snapshot.solar.remaining_kwh.known,
        "unmapped optional forecast value remains unknown");

  const char* partial =
      "{\"v\":1,\"ts\":1788970000,"
      "\"sft\":{\"s\":\"9.1\",\"u\":\"kWh\"}}";
  check(ha_payload_parse(partial, strlen(partial), &snapshot, &info),
        "partial HA forecast parses");
  check(snapshot.solar.configured && snapshot.solar.forecast_kwh.known,
        "today total alone is a useful configured forecast");

  const char* missing_total =
      "{\"v\":1,\"ts\":1788970000,"
      "\"sfr\":{\"s\":\"2.0\",\"u\":\"kWh\"}}";
  check(ha_payload_parse(missing_total, strlen(missing_total), &snapshot, &info),
        "forecast without total still parses");
  check(!snapshot.solar.configured && snapshot.solar.remaining_kwh.known,
        "supporting value alone does not claim a configured forecast");

  const char* bad =
      "{\"v\":1,\"ts\":1788970000,"
      "\"sft\":{\"s\":\"unknown\",\"u\":\"kWh\"},"
      "\"sfr\":{\"s\":\"unavailable\",\"u\":\"kWh\"},"
      "\"sfp\":{\"s\":\"not-a-number\",\"u\":\"%\"},"
      "\"sfk\":{\"s\":\"5\",\"u\":\"MW\"}}";
  check(ha_payload_parse(bad, strlen(bad), &snapshot, &info),
        "bad individual forecast values do not spoil the snapshot");
  check(!snapshot.solar.configured && !snapshot.solar.forecast_kwh.known &&
            !snapshot.solar.remaining_kwh.known && !snapshot.solar.vs_forecast_pct.known &&
            !snapshot.solar.peak_kw.known,
        "unknown unavailable malformed and unsupported forecast values stay unknown");
  check(info.unavailable == 2 && info.invalid_number == 1 && info.unsupported_unit == 1,
        "bad forecast states are classified accurately");

  const char* unsupported_total =
      "{\"v\":1,\"ts\":1788970000,"
      "\"sft\":{\"s\":\"12\",\"u\":\"MJ\"}}";
  check(ha_payload_parse(unsupported_total, strlen(unsupported_total), &snapshot, &info),
        "unsupported total unit leaves a valid snapshot");
  check(!snapshot.solar.configured && !snapshot.solar.forecast_kwh.known,
        "unsupported total unit cannot configure the forecast");
}

void test_server_forecast() {
  printf("server forecast\n");
  const char* payload =
      "{\"v\":1,\"ts\":1788970000,\"ok\":true,"
      "\"solar\":{\"configured\":true,\"forecast\":14.2,\"remaining\":5.1,"
      "\"vs_forecast\":103,\"peak_kw\":4.8}}";
  Snapshot snapshot;
  check(snapshot_parse(payload, strlen(payload), &snapshot),
        "server snapshot with forecast parses");
  check(snapshot.solar.configured, "server keeps its own configured forecast");
  check_near(snapshot.solar.forecast_kwh.value, 14.2f, "server forecast total unchanged");
  check_near(snapshot.solar.remaining_kwh.value, 5.1f, "server forecast remaining unchanged");
  check_near(snapshot.solar.vs_forecast_pct.value, 103.0f,
             "server forecast percentage unchanged");
  check_near(snapshot.solar.peak_kw.value, 4.8f, "server forecast peak unchanged");
}

void test_solar_metric_layout() {
  printf("solar metric layout\n");
  Snapshot snapshot;
  SolarOptionalMetricSlots slots = solar_optional_metric_slots(snapshot);
  check(slots.remaining == SolarOptionalMetricSlots::Hidden &&
            slots.vs_forecast == SolarOptionalMetricSlots::Hidden &&
            slots.peak == SolarOptionalMetricSlots::Hidden,
        "unconfigured forecast hides every optional metric");

  snapshot.valid = true;
  snapshot.solar.configured = true;
  snapshot.solar.forecast_kwh = {true, 8.0f};
  slots = solar_optional_metric_slots(snapshot);
  check(slots.remaining == SolarOptionalMetricSlots::Hidden &&
            slots.vs_forecast == SolarOptionalMetricSlots::Hidden &&
            slots.peak == SolarOptionalMetricSlots::Hidden,
        "total-only forecast reserves no optional rows");

  snapshot.solar.remaining_kwh = {true, 0.0f};
  snapshot.solar.peak_kw = {true, 4.5f};
  slots = solar_optional_metric_slots(snapshot);
  check(slots.remaining == 1 && slots.vs_forecast == SolarOptionalMetricSlots::Hidden &&
            slots.peak == 2,
        "known optional metrics pack without a gap and preserve genuine zero");

  snapshot.solar.vs_forecast_pct = {true, 93.0f};
  slots = solar_optional_metric_slots(snapshot);
  check(slots.remaining == 1 && slots.vs_forecast == 2 && slots.peak == 3,
        "all optional metrics use the three available slots in semantic order");
}

void test_home_assistant_payload() {
  printf("home assistant payload\n");
  const char* valid =
      "{\"v\":1,\"ts\":1788970000,\"tz\":60,"
      "\"mid\":1788908400,\"next\":1788994800,"
      "\"pp\":{\"s\":\"3420\",\"u\":\"W\"},"
      "\"gp\":{\"s\":\"-1.1\",\"u\":\"kW\"},"
      "\"bp\":{\"s\":\"-500\",\"u\":\"W\"},"
      "\"hp\":{\"s\":\"2.75\",\"u\":\"kW\"},"
      "\"ep\":{\"s\":\"0\",\"u\":\"W\"},"
      "\"og\":{\"s\":\"off\",\"u\":null},"
      "\"soc\":{\"s\":\"64.5\",\"u\":\"%\"},"
      "\"tmp\":{\"s\":\"24.2\",\"u\":\"°C\"},"
      "\"dpv\":{\"s\":\"8450\",\"u\":\"Wh\"},"
      "\"dim\":{\"s\":\"3.2\",\"u\":\"kWh\"}}";
  Snapshot snapshot;
  HaParseInfo info;
  check(ha_payload_parse(valid, strlen(valid), &snapshot, &info), "valid HA payload parses");
  check_near(snapshot.power.pv.value, 3.42f, "W converts to kW");
  check_near(snapshot.power.grid.value, -1.1f, "negative grid power retained");
  check_near(snapshot.power.batt.value, -0.5f, "negative battery W retained and converted");
  check_near(snapshot.power.home.value, 2.75f, "direct home mapping is preferred");
  check(snapshot.power.ev.known && snapshot.power.ev.value == 0.0f,
        "genuine mapped EV zero remains known");
  check(snapshot.power.off_grid_known && !snapshot.power.off_grid,
        "on-grid boolean remains known false");
  check_near(snapshot.today.solar.value, 8.45f, "Wh converts to kWh");
  check_near(snapshot.today.imported.value, 3.2f, "kWh remains kWh");
  check(snapshot.today.present, "configured daily entity makes today present");
  check(info.local_midnight_ts == 1788908400u &&
            info.next_local_midnight_ts == 1788994800u,
        "HA payload retains authoritative local midnight boundaries");
  check(info.units[static_cast<size_t>(HaEntity::PvPower)] == HaUnit::Watts &&
            info.units[static_cast<size_t>(HaEntity::GridPower)] == HaUnit::Kilowatts,
        "live parse retains units for Recorder history");
  check(!snapshot.today.exported.known, "missing daily entity remains unknown");
  check(!snapshot.solar.configured, "HA forecast disabled payload behaves as before");

  const char* fallback =
      "{\"v\":1,\"ts\":1788970000,"
      "\"pp\":{\"s\":\"4\",\"u\":\"kW\"},"
      "\"gp\":{\"s\":\"-1\",\"u\":\"kW\"},"
      "\"bp\":{\"s\":\"1.5\",\"u\":\"kW\"}}";
  check(ha_payload_parse(fallback, strlen(fallback), &snapshot, &info),
        "payload without optional EV parses");
  check(snapshot.power.ev.known && snapshot.power.ev.value == 0.0f,
        "unconfigured optional EV explicitly means no EV leg");
  check_near(snapshot.power.home.value, 1.5f, "home fallback uses Puck sign equation");

  const char* bad_states =
      "{\"v\":1,\"ts\":1788970000,"
      "\"pp\":{\"s\":\"unknown\",\"u\":\"W\"},"
      "\"gp\":{\"s\":\"unavailable\",\"u\":\"W\"},"
      "\"bp\":{\"s\":\"not-a-number\",\"u\":\"kW\"},"
      "\"ep\":{\"s\":\"unavailable\",\"u\":\"kW\"},"
      "\"soc\":{\"s\":\"50\",\"u\":\"widgets\"},"
      "\"hp\":{\"s\":\"0\",\"u\":\"kW\"}}";
  check(ha_payload_parse(bad_states, strlen(bad_states), &snapshot, &info),
        "individual bad HA states do not corrupt the payload");
  check(!snapshot.power.pv.known && !snapshot.power.grid.known &&
            !snapshot.power.batt.known,
        "unknown unavailable and non-numeric remain unknown");
  check(snapshot.power.home.known && snapshot.power.home.value == 0.0f,
        "genuine direct home zero remains known");
  check(!snapshot.power.ev.known, "configured unavailable EV remains unknown");
  check(!snapshot.battery.soc_pct.known && info.unsupported_unit == 1,
        "unsupported unit remains unknown and is reported");
  check(info.unavailable == 3 && info.invalid_number == 1,
        "unavailable and malformed states are diagnosed");

  const char* ev_unavailable =
      "{\"v\":1,\"ts\":1788970000,"
      "\"pp\":{\"s\":\"4\",\"u\":\"kW\"},"
      "\"gp\":{\"s\":\"0\",\"u\":\"kW\"},"
      "\"bp\":{\"s\":\"1\",\"u\":\"kW\"},"
      "\"ep\":{\"s\":\"unavailable\",\"u\":\"kW\"}}";
  check(ha_payload_parse(ev_unavailable, strlen(ev_unavailable), &snapshot, &info),
        "unavailable configured EV payload parses");
  check(!snapshot.power.home.known,
        "home is not derived when configured EV is unavailable");

  const char* home_unavailable =
      "{\"v\":1,\"ts\":1788970000,"
      "\"pp\":{\"s\":\"4\",\"u\":\"kW\"},"
      "\"gp\":{\"s\":\"0\",\"u\":\"kW\"},"
      "\"bp\":{\"s\":\"1\",\"u\":\"kW\"},"
      "\"hp\":{\"s\":\"unavailable\",\"u\":\"kW\"}}";
  check(ha_payload_parse(home_unavailable, strlen(home_unavailable), &snapshot, &info),
        "unavailable direct home payload parses");
  check(!snapshot.power.home.known,
        "configured direct home is not replaced by a derived value while unavailable");

  snapshot.ts = 1234;
  check(!ha_payload_parse("{bad", 4, &snapshot, &info), "malformed returned payload rejected");
  check(snapshot.ts == 1234, "malformed payload preserves last good Snapshot");
  check(!ha_payload_parse("{\"v\":1}", 7, &snapshot, &info),
        "payload without timestamp rejected");

  check(ha_http_status(200) == HaHttpStatus::Ok, "HTTP 200 accepted");
  check(ha_http_status(401) == HaHttpStatus::Unauthorised, "HTTP 401 is unauthorised");
  check(ha_http_status(403) == HaHttpStatus::Unauthorised, "HTTP 403 is unauthorised");
  check(ha_http_status(500) == HaHttpStatus::HttpError, "other HTTP errors stay distinct");
}

float decode_one(uint8_t key, const uint16_t* words, size_t count) {
  const ModbusReg& reg = MODBUS_REGS[key];
  float value = 0.0f;
  if (!modbus_decode(reg, reg.address, words, count, &value)) {
    return NAN;
  }
  return value;
}

// --- decoding -------------------------------------------------------------

void test_decode() {
  printf("decode\n");

  // U16 with a gain: SOC of 78.5 % is raw 785 at gain 10.
  const uint16_t soc[] = {785};
  check_near(decode_one(MB_PLANT_ESS_SOC, soc, 1), 78.5f, "U16 gain 10");

  // S16 negative: -5.5 C is raw -55 at gain 10, two's complement.
  const uint16_t temp[] = {0xFFC9};
  check_near(decode_one(MB_INV_ESS_MAX_TEMP, temp, 1), -5.5f, "S16 negative");

  // S32 positive, big-endian word order: 3420 W -> 3.42 kW at gain 1000.
  const uint16_t pv[] = {0x0000, 0x0D5C};
  check_near(decode_one(MB_PLANT_PV_POWER, pv, 2), 3.42f, "S32 positive");

  // S32 negative: exporting 1.1 kW is raw -1100.
  const uint16_t grid[] = {0xFFFF, 0xFBB4};
  check_near(decode_one(MB_PLANT_GRID_POWER, grid, 2), -1.1f, "S32 negative");

  // U32: 16.00 kWh capacity is raw 1600 at gain 100.
  const uint16_t capacity[] = {0x0000, 0x0640};
  check_near(decode_one(MB_PLANT_ESS_CAPACITY, capacity, 2), 16.0f, "U32 gain 100");

  // U64 accumulator, past 32 bits so a truncating decode would show up:
  // 0x0000000100000000 = 4294967296 raw, /100 = 42949672.96 kWh.
  const uint16_t total[] = {0x0000, 0x0001, 0x0000, 0x0000};
  const float imported = decode_one(MB_PLANT_TOTAL_IMPORTED, total, 4);
  check(imported > 42949000.0f && imported < 42950500.0f, "U64 beyond 32 bits");

  // A register that does not fit inside the words it was given must refuse
  // rather than read past the end.
  const ModbusReg& reg = MODBUS_REGS[MB_PLANT_PV_POWER];
  float ignored = 0.0f;
  check(!modbus_decode(reg, reg.address, pv, 1, &ignored), "short span refused");
  check(!modbus_decode(reg, reg.address + 1, pv, 2, &ignored), "span starting late refused");
}

// --- batching -------------------------------------------------------------

void test_plan() {
  printf("plan\n");
  ModbusSpan spans[8];

  // The plant's fast set is one read of 94 words, 30000-30093, exactly as §D2
  // works out. If this splits, the gap tolerance has crept back in from the
  // server's batcher and every poll costs an extra second.
  size_t count = modbus_plan(ModbusScope::Plant, ModbusCadence::Fast, spans, 8);
  check(count == 1, "plant fast is a single request");
  if (count == 1) {
    check(spans[0].start == 30000, "plant fast starts at 30000");
    check(spans[0].words == 94, "plant fast is 94 words");
  }

  count = modbus_plan(ModbusScope::Plant, ModbusCadence::Slow, spans, 8);
  check(count == 1, "plant slow is a single request");
  if (count == 1) {
    check(spans[0].start == 30216, "plant slow starts at 30216");
    check(spans[0].words == 8, "plant slow is 8 words");
  }

  // The inverter's slow set spans 30566-30620; its DC output at 31502 is nearly
  // a thousand registers away and cannot join it.
  count = modbus_plan(ModbusScope::Inverter, ModbusCadence::Slow, spans, 8);
  check(count == 2, "inverter slow needs two requests");
  if (count == 2) {
    check(spans[0].start == 30566 && spans[0].words == 55, "inverter slow first span");
    check(spans[1].start == 31509 && spans[1].words == 2, "inverter slow second span");
  }

  count = modbus_plan(ModbusScope::Inverter, ModbusCadence::Fast, spans, 8);
  check(count == 1 && spans[0].start == 31502 && spans[0].words == 2, "inverter fast span");

  count = modbus_plan(ModbusScope::AcCharger, ModbusCadence::Fast, spans, 8);
  check(count == 1 && spans[0].start == 32003 && spans[0].words == 2, "charger span");

  // No span may exceed the protocol's own ceiling.
  for (int scope = 0; scope < 3; ++scope) {
    for (int cadence = 0; cadence < 2; ++cadence) {
      const size_t n = modbus_plan(static_cast<ModbusScope>(scope),
                                   static_cast<ModbusCadence>(cadence), spans, 8);
      for (size_t i = 0; i < n; ++i) {
        check(spans[i].words <= MODBUS_MAX_WORDS, "span within the 124-word limit");
      }
    }
  }
}

// --- derivation -----------------------------------------------------------

void test_snapshot() {
  printf("snapshot\n");

  ModbusValues values;
  // A plant read: PV 3.42, grid -1.1 (exporting), battery +1.2 (charging).
  const uint16_t plant[] = {
      0x0000, 0x0D5C,  // 30035 pv 3420 W
  };
  modbus_apply(&values, ModbusScope::Plant, 30035, plant, 2);

  const uint16_t grid[] = {0xFFFF, 0xFBB4};  // 30005 -1100 W
  modbus_apply(&values, ModbusScope::Plant, 30005, grid, 2);

  const uint16_t batt[] = {0x0000, 0x04B0};  // 30037 1200 W
  modbus_apply(&values, ModbusScope::Plant, 30037, batt, 2);

  Snapshot snapshot;
  modbus_to_snapshot(values, &snapshot);

  // home = pv - batt + grid - ev = 3.42 - 1.2 + (-1.1) - 0 = 1.12, which is the
  // figure 01_normal.json carries for the same inputs.
  check(snapshot.power.home.known, "home derived");
  check_near(snapshot.power.home.value, 1.12f, "home matches the server's formula");
  check(snapshot.power.ev.known && snapshot.power.ev.value == 0.0f, "no charger means 0 kW EV");

  // EV must come out of the house figure, or a charging car is counted twice.
  ModbusValues with_ev = values;
  const uint16_t charger[] = {0x0000, 0x1B58};  // 32003, 7000 W
  modbus_apply(&with_ev, ModbusScope::AcCharger, 32003, charger, 2);
  modbus_to_snapshot(with_ev, &snapshot);
  check_near(snapshot.power.ev.value, 7.0f, "EV power read");
  check_near(snapshot.power.home.value, 0.0f, "home floors at zero, never negative");

  // Two inverters: temperatures average, daily generation sums.
  ModbusValues two;
  const uint16_t temp_a[] = {0x00FA};  // 25.0 C
  const uint16_t temp_b[] = {0x0136};  // 31.0 C
  modbus_apply(&two, ModbusScope::Inverter, 30620, temp_a, 1);
  modbus_apply(&two, ModbusScope::Inverter, 30620, temp_b, 1);
  const uint16_t gen[] = {0x0000, 0x0258};  // 6.00 kWh
  modbus_apply(&two, ModbusScope::Inverter, 31509, gen, 2);
  modbus_apply(&two, ModbusScope::Inverter, 31509, gen, 2);
  modbus_to_snapshot(two, &snapshot);
  check_near(snapshot.battery.temp_c.value, 28.0f, "inverter temperatures average");
  check_near(snapshot.today.solar.value, 12.0f, "inverter generation sums");

  // Alarms are bitfields, counted not summed.
  ModbusValues alarmed;
  const uint16_t bits[] = {0x0005};  // two bits set
  modbus_apply(&alarmed, ModbusScope::Plant, 30027, bits, 1);
  modbus_to_snapshot(alarmed, &snapshot);
  check(snapshot.alarms == 2, "alarm bits are counted");

  // Nothing read at all must not claim a day of zeroes.
  ModbusValues empty;
  modbus_to_snapshot(empty, &snapshot);
  check(!snapshot.today.present, "no day block without any daily register");
  check(!snapshot.power.home.known, "home unknown when its inputs are");
}

void test_day_baseline() {
  printf("day baseline\n");

  ModbusDayBaseline baseline;
  MaybeFloat imported;
  MaybeFloat exported;

  // First read of the day latches, so today starts at zero rather than at the
  // lifetime total.
  modbus_day_totals(&baseline, 20000, 1000.0f, 400.0f, true, &imported, &exported);
  check_near(imported.value, 0.0f, "first read latches import");
  check_near(exported.value, 0.0f, "first read latches export");

  modbus_day_totals(&baseline, 20000, 1004.5f, 402.25f, true, &imported, &exported);
  check_near(imported.value, 4.5f, "import accumulates through the day");
  check_near(exported.value, 2.25f, "export accumulates through the day");

  // Midnight: the day rolls and the baseline moves with it.
  modbus_day_totals(&baseline, 20001, 1004.5f, 402.25f, true, &imported, &exported);
  check_near(imported.value, 0.0f, "new day re-latches");

  // A firmware upgrade zeroes the counters. Re-latch instead of reporting a
  // day of minus a thousand kilowatt-hours.
  modbus_day_totals(&baseline, 20001, 0.0f, 0.0f, true, &imported, &exported);
  check_near(imported.value, 0.0f, "counter reset re-latches rather than going negative");
  modbus_day_totals(&baseline, 20001, 1.5f, 0.5f, true, &imported, &exported);
  check_near(imported.value, 1.5f, "accumulates again after a reset");

  // Unknown totals must stay unknown, not become zero.
  MaybeFloat none_in;
  modbus_day_totals(&baseline, 20001, 0.0f, 0.0f, false, &none_in, nullptr);
  check(!none_in.known, "unread totals stay unknown");
}

// --- history --------------------------------------------------------------

void test_history() {
  printf("history\n");

  history_reset(HistoryBank::Live);
  check(history_head_minute(HistoryBank::Live) == 0, "empty ring has no head");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 0, "empty ring has no samples");

  uint32_t from = 0;
  uint32_t to = 0;
  check(!history_window(HistoryBank::Live, &from, &to), "no window before anything is recorded");

  // A day anchored to UTC midnight: minute 30000000 is a round day boundary
  // only by construction, so pick one and work relative to it.
  const uint32_t midnight = (30000000u / 1440u) * 1440u;
  history_set_timezone(HistoryBank::Live, 0);
  for (uint32_t m = 0; m <= 600; ++m) {
    history_put(HistoryBank::Live, HistorySeries::Pv, midnight + m, static_cast<float>(m) / 100.0f);
  }
  check(history_head_minute(HistoryBank::Live) == midnight + 600, "head follows the newest sample");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 601, "every minute recorded");

  const uint32_t revision = history_revision(HistoryBank::Live);
  history_put(HistoryBank::Live, HistorySeries::Soc, midnight + 100, 55.0f);
  check(history_head_minute(HistoryBank::Live) == midnight + 600 &&
            history_revision(HistoryBank::Live) != revision,
        "a late backfill sample changes revision without moving the live head");

  // Model a chart having consumed that revision, followed by interaction and a
  // later Recorder window writing behind the same live head. The second write
  // must publish a new cache key and both samples must survive reduction.
  HistoryColumn backfill_columns[144];
  const uint32_t handled_revision = history_revision(HistoryBank::Live);
  history_reduce(HistoryBank::Live, HistorySeries::Soc, midnight,
                 midnight + 1440, backfill_columns, 144);
  check(backfill_columns[10].known, "first late sample reaches reduced chart data");
  history_put(HistoryBank::Live, HistorySeries::Soc, midnight + 200, 65.0f);
  check(history_head_minute(HistoryBank::Live) == midnight + 600 &&
            history_revision(HistoryBank::Live) != handled_revision,
        "a later Recorder window publishes another cache revision");
  history_reduce(HistoryBank::Live, HistorySeries::Soc, midnight,
                 midnight + 1440, backfill_columns, 144);
  check(backfill_columns[10].known && backfill_columns[20].known,
        "refresh after interaction includes both late Recorder windows");

  Snapshot same_minute_live;
  same_minute_live.valid = true;
  same_minute_live.ts = (midnight + 600) * 60;
  same_minute_live.power.pv.known = true;
  same_minute_live.power.pv.value = 7.0f;
  const uint32_t before_live_refresh = history_revision(HistoryBank::Live);
  history_record(same_minute_live);
  check(history_revision(HistoryBank::Live) == before_live_refresh,
        "same-minute live refresh does not invalidate the reduced chart cache");

  check(history_window(HistoryBank::Live, &from, &to), "window available once recorded");
  check(from == midnight, "window anchors to local midnight");
  check(to == midnight + 1440, "window covers the whole day");

  // Reduction: 1440 minutes onto 144 columns is ten minutes each, and only the
  // first 601 minutes hold anything.
  HistoryColumn columns[144];
  history_reduce(HistoryBank::Live, HistorySeries::Pv, from, to, columns, 144);
  check(columns[0].known, "first column has data");
  check_near(columns[0].min_value, 0.0f, "first column min");
  check_near(columns[0].max_value, 0.09f, "first column max");
  check(!columns[100].known, "columns past the last sample stay empty");
  check(columns[59].known, "last populated column");

  // A gap must not be bridged.
  history_reset(HistoryBank::Live);
  history_set_timezone(HistoryBank::Live, 0);
  history_put(HistoryBank::Live, HistorySeries::Soc, midnight + 10, 50.0f);
  history_put(HistoryBank::Live, HistorySeries::Soc, midnight + 400, 60.0f);
  history_reduce(HistoryBank::Live, HistorySeries::Soc, midnight, midnight + 1440, columns, 144);
  check(columns[1].known, "sample at minute 10 lands in column 1");
  check(!columns[20].known, "the gap between them stays a gap");

  // A clock jump backwards of more than a day starts again rather than
  // interleaving two eras of samples in the same ring.
  const uint32_t before = history_generation(HistoryBank::Live);
  history_put(HistoryBank::Live, HistorySeries::Soc, midnight - 5000, 42.0f);
  check(history_generation(HistoryBank::Live) != before, "a backwards clock jump resets the ring");

  history_reset(HistoryBank::Live);
}

HaHistoryParseResult parse_history_in_chunks(HaHistoryParser* parser, const char* json,
                                             size_t chunk_size,
                                             HaHistoryStats* stats = nullptr) {
  const size_t length = strlen(json);
  for (size_t offset = 0; offset < length; offset += chunk_size) {
    const size_t remaining = length - offset;
    const size_t count = remaining < chunk_size ? remaining : chunk_size;
    if (!parser->feed(reinterpret_cast<const uint8_t*>(json + offset), count)) {
      break;
    }
  }
  return parser->finish(stats);
}

void test_home_assistant_history() {
  printf("home assistant history\n");
  constexpr uint32_t MIDNIGHT = 1786838400u;  // 2026-08-16 00:00:00Z
  constexpr uint32_t NEXT_MIDNIGHT = MIDNIGHT + 86400u;
  constexpr uint32_t CUTOFF = MIDNIGHT + 10 * 60u;

  const char* entity_ids[HA_ENTITY_COUNT] = {};
  HaUnit units[HA_ENTITY_COUNT] = {};
  entity_ids[static_cast<size_t>(HaEntity::PvPower)] = "sensor.pv";
  units[static_cast<size_t>(HaEntity::PvPower)] = HaUnit::Watts;
  entity_ids[static_cast<size_t>(HaEntity::GridPower)] = "sensor.grid";
  units[static_cast<size_t>(HaEntity::GridPower)] = HaUnit::Watts;
  entity_ids[static_cast<size_t>(HaEntity::BatteryPower)] = "sensor.battery";
  units[static_cast<size_t>(HaEntity::BatteryPower)] = HaUnit::Watts;
  entity_ids[static_cast<size_t>(HaEntity::BatterySoc)] = "sensor.soc";
  units[static_cast<size_t>(HaEntity::BatterySoc)] = HaUnit::Percent;

  HaHistoryField fields[HA_HISTORY_MAX_FIELDS];
  const size_t field_count = ha_history_fields_build(entity_ids, units, fields);
  check(field_count == 4, "history requests only PV, SOC and fallback load inputs");
  check(fields[0].entity == HaEntity::PvPower &&
            fields[1].entity == HaEntity::BatterySoc &&
            fields[2].entity == HaEntity::GridPower &&
            fields[3].entity == HaEntity::BatteryPower,
        "fallback history field order is deterministic");

  entity_ids[static_cast<size_t>(HaEntity::HomePower)] = "sensor.home";
  units[static_cast<size_t>(HaEntity::HomePower)] = HaUnit::Kilowatts;
  entity_ids[static_cast<size_t>(HaEntity::EvPower)] = "sensor.ev";
  units[static_cast<size_t>(HaEntity::EvPower)] = HaUnit::Kilowatts;
  const size_t direct_count = ha_history_fields_build(entity_ids, units, fields);
  check(direct_count == 4 && fields[2].entity == HaEntity::HomePower &&
            fields[3].entity == HaEntity::EvPower,
        "direct home history fetches home and optional EV, not derivation inputs");
  entity_ids[static_cast<size_t>(HaEntity::HomePower)] = nullptr;
  entity_ids[static_cast<size_t>(HaEntity::EvPower)] = nullptr;
  entity_ids[static_cast<size_t>(HaEntity::BatteryPower)] = nullptr;
  check(ha_history_fields_build(entity_ids, units, fields) == 3,
        "partially configured chart mappings remain independently usable");
  entity_ids[static_cast<size_t>(HaEntity::BatteryPower)] = "sensor.battery";
  ha_history_fields_build(entity_ids, units, fields);

  const char* valid =
      "[[{\"entity_id\":\"sensor.pv\",\"state\":\"1000\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"},"
      "{\"state\":\"unknown\",\"last_changed\":\"2026-08-16T00:03:00Z\"},"
      "{\"state\":\"unavailable\",\"last_changed\":\"2026-08-16T00:04:00Z\"},"
      "{\"state\":\"not-a-number\",\"last_changed\":\"2026-08-16T00:04:30Z\"},"
      "{\"state\":\"2000\",\"last_changed\":\"2026-08-16T00:05:00Z\"}],"
      "[{\"entity_id\":\"sensor.soc\",\"state\":\"55\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"}],"
      "[{\"entity_id\":\"sensor.grid\",\"state\":\"-1000\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"}],"
      "[{\"entity_id\":\"sensor.battery\",\"state\":\"-500\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"}]]";
  history_reset(HistoryBank::Live);
  HaHistoryParser parser(fields, field_count, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  check(parser.ready(), "bounded history workspace allocates");
  HaHistoryStats stats;
  check(parse_history_in_chunks(&parser, valid, 7, &stats) ==
            HaHistoryParseResult::Applied,
        "valid Recorder payload parses incrementally");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 8,
        "unknown and unavailable PV intervals remain gaps");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Soc) == 10,
        "stable SOC is carried forward at one-minute cadence");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Load) == 8,
        "derived load follows availability of every required input");
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv, MIDNIGHT / 60).value,
             1.0f, "historical W converts to kW");
  check_near(history_value(HistoryBank::Live, HistorySeries::Load, MIDNIGHT / 60).value,
             0.5f, "signed grid and battery values use the live load equation");
  check_near(history_value(HistoryBank::Live, HistorySeries::Load,
                           MIDNIGHT / 60 + 5).value,
             1.5f, "derived load resumes after a usable PV state");
  check(stats.usable_states == 5 && stats.points_written == 26,
        "backfill statistics count states and downsampled chart points");

  // Representative REST minimal_response shape: the first object establishes
  // an inner array's identity, compact intermediate objects inherit it, and a
  // full final object may repeat it. Series order is independent of the request
  // order, and an empty series remains structurally valid.
  const char* minimal_response =
      "[[{\"entity_id\":\"sensor.grid\",\"state\":\"-1000\","
      "\"last_changed\":\"2026-08-16T01:00:00.123456+01:00\","
      "\"last_updated\":\"2026-08-16T01:00:00.123456+01:00\","
      "\"attributes\":null,\"context\":null},"
      "{\"state\":\"-2000\","
      "\"last_changed\":\"2026-08-16T01:02:00.5+01:00\"},"
      "{\"entity_id\":\"sensor.grid\",\"state\":\"-500\","
      "\"last_changed\":null,"
      "\"last_updated\":\"2026-08-16T00:04:00+00:00\"}],[],"
      "[{\"entity_id\":\"sensor.pv\",\"state\":\"1000\","
      "\"last_changed\":\"2026-08-16T00:00:00.000001Z\"},"
      "{\"state\":\"unknown\","
      "\"last_changed\":\"2026-08-16T00:01:00+00:00\"},"
      "{\"state\":\"unavailable\","
      "\"last_changed\":\"2026-08-16T00:02:00+00:00\"},"
      "{\"entity_id\":\"sensor.pv\",\"state\":\"2000\","
      "\"last_changed\":\"2026-08-16T00:03:00+00:00\","
      "\"last_updated\":\"2026-08-16T00:03:00+00:00\"}],"
      "[{\"entity_id\":\"sensor.soc\",\"state\":\"55\","
      "\"last_changed\":\"2026-08-16T00:00:00+00:00\"}],"
      "[{\"entity_id\":\"sensor.battery\",\"state\":\"-500\","
      "\"last_changed\":\"2026-08-16T00:00:00+00:00\"}]]";
  history_reset(HistoryBank::Live);
  HaHistoryParser minimal_parser(fields, field_count, MIDNIGHT, NEXT_MIDNIGHT,
                                 CUTOFF);
  check(parse_history_in_chunks(&minimal_parser, minimal_response, 11) ==
            HaHistoryParseResult::Applied,
        "documented HA minimal response shape parses incrementally");
  check(history_value(HistoryBank::Live, HistorySeries::Pv,
                      MIDNIGHT / 60).known &&
            !history_value(HistoryBank::Live, HistorySeries::Pv,
                           MIDNIGHT / 60 + 1).known &&
            history_value(HistoryBank::Live, HistorySeries::Pv,
                          MIDNIGHT / 60 + 3).known,
        "minimal unknown and unavailable states remain gaps within inherited series");
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv,
                           MIDNIGHT / 60 + 3).value,
             2.0f, "full last object may repeat the current series entity");
  check(minimal_parser.error() == HaHistoryParseError::None,
        "valid minimal response has no parser diagnostic");

  history_reset(HistoryBank::Live);
  HaHistoryField home_ev_fields[] = {
      {HaEntity::HomePower, "sensor.home", HaUnit::Kilowatts},
      {HaEntity::EvPower, "sensor.ev", HaUnit::Watts},
  };
  const char* direct_load =
      "[[{\"entity_id\":\"sensor.home\",\"state\":\"1.25\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"}],"
      "[{\"entity_id\":\"sensor.ev\",\"state\":\"500\","
      "\"last_updated\":\"2026-08-16T00:00:00Z\"}]]";
  HaHistoryParser direct_parser(home_ev_fields, 2, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  check(parse_history_in_chunks(&direct_parser, direct_load, 17) ==
            HaHistoryParseResult::Applied,
        "direct home and EV history parse with their live units");
  check_near(history_value(HistoryBank::Live, HistorySeries::Load,
                           MIDNIGHT / 60 + 9).value,
             1.75f, "direct mapped home is preferred and EV is added to chart load");

  history_reset(HistoryBank::Live);
  HaHistoryField direct_fields[] = {
      {HaEntity::HomePower, "sensor.home", HaUnit::Watts},
  };
  const char* zero =
      "[[{\"entity_id\":\"sensor.home\",\"state\":\"0\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"}]]";
  HaHistoryParser zero_parser(direct_fields, 1, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  check(parse_history_in_chunks(&zero_parser, zero, strlen(zero)) ==
            HaHistoryParseResult::Applied,
        "genuine historical zero is applied");
  const MaybeFloat zero_value =
      history_value(HistoryBank::Live, HistorySeries::Load, MIDNIGHT / 60 + 9);
  check(zero_value.known && zero_value.value == 0.0f,
        "genuine historical zero remains a known zero");

  history_reset(HistoryBank::Live);
  HaHistoryParser empty_parser(direct_fields, 1, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  check(parse_history_in_chunks(&empty_parser, "[]", 1) ==
            HaHistoryParseResult::NoData,
        "empty outer Recorder history is a permanent no-data result");
  HaHistoryParser empty_series_parser(direct_fields, 1, MIDNIGHT,
                                      NEXT_MIDNIGHT, CUTOFF);
  check(parse_history_in_chunks(&empty_series_parser, "[[],[]]", 1) ==
            HaHistoryParseResult::NoData,
        "empty Recorder entity arrays are structurally valid no-data results");
  const char* missing =
      "[[{\"entity_id\":\"sensor.not_recorded\",\"state\":\"1\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"}]]";
  HaHistoryParser missing_parser(direct_fields, 1, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  check(parse_history_in_chunks(&missing_parser, missing, 13) == HaHistoryParseResult::NoData,
        "missing or entity-excluded history stays optional");

  history_put(HistoryBank::Live, HistorySeries::Pv, CUTOFF / 60, 9.0f);
  HaHistoryParser malformed_parser(direct_fields, 1, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  check(parse_history_in_chunks(&malformed_parser, "[[{bad}]]", 3) ==
            HaHistoryParseResult::BadPayload,
        "malformed Recorder JSON is rejected");
  HaHistoryParser structural_parser(direct_fields, 1, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  check(parse_history_in_chunks(&structural_parser, "[[],]", 2) ==
            HaHistoryParseResult::BadPayload,
        "malformed Recorder array structure is rejected");
  HaHistoryParser missing_id_parser(direct_fields, 1, MIDNIGHT, NEXT_MIDNIGHT,
                                    CUTOFF);
  const char* missing_first_id =
      "[[{\"state\":\"1\","
      "\"last_changed\":\"2026-08-16T00:00:00+00:00\"}]]";
  check(parse_history_in_chunks(&missing_id_parser, missing_first_id, 8) ==
            HaHistoryParseResult::BadPayload &&
            missing_id_parser.error() == HaHistoryParseError::MissingEntityId &&
            missing_id_parser.error_series() == 1,
        "minimal series must establish entity identity on its first object");
  HaHistoryParser timestamp_parser(direct_fields, 1, MIDNIGHT, NEXT_MIDNIGHT,
                                   CUTOFF);
  const char* invalid_timestamp =
      "[[{\"entity_id\":\"sensor.home\",\"state\":\"1\","
      "\"last_changed\":\"not-a-timestamp\"}]]";
  check(parse_history_in_chunks(&timestamp_parser, invalid_timestamp, 9) ==
            HaHistoryParseResult::BadPayload &&
            timestamp_parser.error() == HaHistoryParseError::InvalidTimestamp,
        "invalid state timestamp has a specific parser diagnostic");
  HaHistoryParser top_level_parser(direct_fields, 1, MIDNIGHT, NEXT_MIDNIGHT,
                                   CUTOFF);
  check(parse_history_in_chunks(&top_level_parser, "{}", 1) ==
            HaHistoryParseResult::BadPayload &&
            top_level_parser.error() ==
                HaHistoryParseError::UnexpectedTopLevelToken,
        "non-array response has a specific parser diagnostic");
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv, CUTOFF / 60).value,
             9.0f, "malformed history preserves existing live samples");

  HaHistoryResponseLimiter below_limit;
  check(below_limit.accept(HA_HISTORY_RESPONSE_MAX_BYTES / 2) &&
            below_limit.accept(HA_HISTORY_RESPONSE_MAX_BYTES / 2) &&
            below_limit.received() == HA_HISTORY_RESPONSE_MAX_BYTES &&
            !below_limit.exceeded(),
        "an individual window may stream up to the fixed response ceiling");
  check(!below_limit.accept(1) && below_limit.exceeded() &&
            below_limit.received() == HA_HISTORY_RESPONSE_MAX_BYTES,
        "an oversized individual window fails without accepting a truncated byte");

  HaHistoryField unsupported[] = {
      {HaEntity::PvPower, "sensor.pv", HaUnit::Unknown},
  };
  HaHistoryParser unsupported_parser(unsupported, 1, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  const char* one_pv =
      "[[{\"entity_id\":\"sensor.pv\",\"state\":\"42\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"}]]";
  check(parse_history_in_chunks(&unsupported_parser, one_pv, 9) ==
            HaHistoryParseResult::NoData,
        "unsupported live unit cannot turn Recorder states into chart values");

  history_reset(HistoryBank::Live);
  history_put(HistoryBank::Live, HistorySeries::Pv, CUTOFF / 60, 9.0f);
  HaHistoryField pv_field[] = {
      {HaEntity::PvPower, "sensor.pv", HaUnit::Watts},
  };
  const char* overlap =
      "[[{\"entity_id\":\"sensor.pv\",\"state\":\"1000\","
      "\"last_changed\":\"2026-08-16T00:09:00Z\"},"
      "{\"state\":\"2000\",\"last_changed\":\"2026-08-16T00:10:00Z\"}]]";
  HaHistoryParser overlap_parser(pv_field, 1, MIDNIGHT, NEXT_MIDNIGHT, CUTOFF);
  check(parse_history_in_chunks(&overlap_parser, overlap, 11) ==
            HaHistoryParseResult::Applied,
        "history up to the first live minute is applied");
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv, CUTOFF / 60).value,
             9.0f, "Recorder overlap cannot replace the first live sample");

  constexpr uint32_t WINDOW_BOUNDARY = MIDNIGHT + 2 * 60 * 60u;
  constexpr uint32_t WINDOW_CUTOFF = MIDNIGHT + 4 * 60 * 60u;
  const char* first_window =
      "[[{\"entity_id\":\"sensor.pv\",\"state\":\"1000\","
      "\"last_changed\":\"2026-08-16T00:00:00Z\"},"
      "{\"state\":\"2000\","
      "\"last_changed\":\"2026-08-16T01:59:00Z\"},"
      "{\"state\":\"99000\","
      "\"last_changed\":\"2026-08-16T02:00:00Z\"}]]";
  const char* second_window =
      "[[{\"entity_id\":\"sensor.pv\",\"state\":\"3000\","
      "\"last_changed\":\"2026-08-16T02:00:00Z\"},"
      "{\"state\":\"4000\","
      "\"last_changed\":\"2026-08-16T03:59:00Z\"},"
      "{\"state\":\"99000\","
      "\"last_changed\":\"2026-08-16T04:00:00Z\"}]]";
  history_reset(HistoryBank::Live);
  history_put(HistoryBank::Live, HistorySeries::Pv, WINDOW_CUTOFF / 60, 9.0f);
  HaHistoryParser first_window_parser(pv_field, 1, MIDNIGHT, NEXT_MIDNIGHT,
                                      MIDNIGHT, WINDOW_BOUNDARY);
  HaHistoryParser second_window_parser(pv_field, 1, MIDNIGHT, NEXT_MIDNIGHT,
                                       WINDOW_BOUNDARY, WINDOW_CUTOFF);
  check(parse_history_in_chunks(&first_window_parser, first_window, 13) ==
            HaHistoryParseResult::Applied &&
            parse_history_in_chunks(&second_window_parser, second_window, 17) ==
                HaHistoryParseResult::Applied,
        "adjacent bounded history windows apply chronologically");
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv,
                           WINDOW_BOUNDARY / 60 - 1).value,
             2.0f, "first history window owns the minute before its boundary");
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv,
                           WINDOW_BOUNDARY / 60).value,
             3.0f, "adjacent window owns the boundary minute without duplication");
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv,
                           WINDOW_CUTOFF / 60).value,
             9.0f, "final window cannot overwrite the exact live cutoff minute");

  history_reset(HistoryBank::Live);
  history_put(HistoryBank::Live, HistorySeries::Pv, WINDOW_CUTOFF / 60, 9.0f);
  HaHistoryParser retained_window_parser(pv_field, 1, MIDNIGHT,
                                         NEXT_MIDNIGHT, MIDNIGHT,
                                         WINDOW_BOUNDARY);
  check(parse_history_in_chunks(&retained_window_parser, first_window, 19) ==
            HaHistoryParseResult::Applied,
        "completed earlier window restores chart data");
  HaHistoryParser failed_later_parser(pv_field, 1, MIDNIGHT, NEXT_MIDNIGHT,
                                      WINDOW_BOUNDARY, WINDOW_CUTOFF);
  check(parse_history_in_chunks(&failed_later_parser, "[[{bad}]]", 3) ==
            HaHistoryParseResult::BadPayload &&
            history_value(HistoryBank::Live, HistorySeries::Pv,
                          MIDNIGHT / 60).known,
        "later malformed window leaves an earlier restored window intact");
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv,
                           WINDOW_CUTOFF / 60).value,
             9.0f, "later history failure leaves the good live sample valid");

  uint32_t spring_midnight = 0;
  uint32_t spring_next = 0;
  uint32_t spring_sample = 0;
  uint32_t spring_cutoff = 0;
  check(ha_history_timestamp_parse("2026-03-29T00:00:00+00:00", &spring_midnight) &&
            ha_history_timestamp_parse("2026-03-30T00:00:00+01:00", &spring_next) &&
            ha_history_timestamp_parse("2026-03-29T02:15:00+01:00", &spring_sample) &&
            ha_history_timestamp_parse("2026-03-29T03:00:00+01:00", &spring_cutoff),
        "ISO timestamps retain their explicit DST offsets");
  check(spring_next - spring_midnight == 23 * 3600u,
        "local-midnight boundary may be a 23-hour DST day");
  uint32_t autumn_midnight = 0;
  uint32_t autumn_next = 0;
  check(ha_history_timestamp_parse("2026-10-25T00:00:00+01:00", &autumn_midnight) &&
            ha_history_timestamp_parse("2026-10-26T00:00:00+00:00", &autumn_next) &&
            autumn_next - autumn_midnight == 25 * 3600u,
        "local-midnight boundary may be a 25-hour DST day");
  const char* autumn_payload =
      "[[{\"entity_id\":\"sensor.pv\",\"state\":\"1000\","
      "\"last_changed\":\"2026-10-25T00:00:00+01:00\"}]]";
  history_reset(HistoryBank::Live);
  HaHistoryParser autumn_parser(pv_field, 1, autumn_midnight, autumn_next,
                                 autumn_next - 60);
  check(parse_history_in_chunks(&autumn_parser, autumn_payload, 19) ==
            HaHistoryParseResult::Applied &&
            history_value(HistoryBank::Live, HistorySeries::Pv,
                          autumn_midnight / 60).known &&
            history_value(HistoryBank::Live, HistorySeries::Pv,
                          autumn_next / 60 - 2).known,
        "25-hour Recorder day retains both its first and final historical hours");
  char dst_payload[256];
  snprintf(dst_payload, sizeof(dst_payload),
           "[[{\"entity_id\":\"sensor.pv\",\"state\":\"1000\","
           "\"last_changed\":\"2026-03-29T02:15:00+01:00\"}]]");
  history_reset(HistoryBank::Live);
  HaHistoryParser dst_parser(pv_field, 1, spring_midnight, spring_next, spring_cutoff);
  check(parse_history_in_chunks(&dst_parser, dst_payload, 5) ==
            HaHistoryParseResult::Applied,
        "DST-day Recorder sample is backfilled");
  uint32_t from = 0;
  uint32_t to = 0;
  check(history_window(HistoryBank::Live, &from, &to) &&
            from == spring_midnight / 60 && to == spring_next / 60,
        "chart uses HA's exact local-midnight window across DST");
  check(history_value(HistoryBank::Live, HistorySeries::Pv, spring_sample / 60).known,
        "offset timestamp lands in the correct absolute minute");

  // Existing fallback remains: if Recorder never supplies anything, normal
  // snapshots still build the chart from boot onward.
  history_reset(HistoryBank::Live);
  Snapshot live;
  live.valid = true;
  live.ts = CUTOFF;
  live.power.pv = {true, 3.0f};
  history_record(live);
  check_near(history_value(HistoryBank::Live, HistorySeries::Pv, CUTOFF / 60).value,
             3.0f, "live-from-boot recording is independent of Recorder");
}

void test_history_backfill_retry() {
  printf("history backfill retry\n");
  check(history_backfill_outcome(FetchResult::NoNetwork) ==
            HistoryBackfillOutcome::TransientFailure &&
            history_backfill_outcome(FetchResult::HttpError, 503) ==
                HistoryBackfillOutcome::TransientFailure &&
            history_backfill_outcome(FetchResult::HttpError, 429) ==
                HistoryBackfillOutcome::TransientFailure,
        "network, server and rate-limit failures are retryable");
  check(history_backfill_outcome(FetchResult::BadPayload) ==
            HistoryBackfillOutcome::PermanentFailure &&
            history_backfill_outcome(FetchResult::HttpError, 404) ==
                HistoryBackfillOutcome::PermanentFailure &&
            history_backfill_outcome(FetchResult::EntityUnavailable) ==
                HistoryBackfillOutcome::NoData,
        "malformed, missing-endpoint and no-data results are not retried");
  constexpr uint32_t DAY_START = 1786838400u;
  constexpr uint32_t DAY_END = DAY_START + 24 * 60 * 60u;

  HistoryBackfillRetry full_day;
  check(!full_day.due(0), "Recorder waits for the first successful live poll");
  full_day.activate(DAY_START, DAY_END, DAY_END);
  uint32_t previous_end = DAY_START;
  while (full_day.active()) {
    check(full_day.window_start_ts() == previous_end,
          "backfill windows are consecutive and chronological");
    const uint32_t end = full_day.window_end_ts();
    check(end > previous_end &&
              end - previous_end <= HISTORY_BACKFILL_WINDOW_SECONDS,
          "each history request is bounded to two hours");
    previous_end = end;
    full_day.record(HistoryBackfillOutcome::Success, 0, 1);
  }
  check(full_day.finished() && full_day.succeeded() &&
            full_day.windows_completed() == 12 &&
            full_day.points_written() == 12 && previous_end == DAY_END,
        "a full 24-hour day is split into twelve bounded windows");

  HistoryBackfillRetry partial_day;
  const uint32_t partial_cutoff = DAY_START + 5 * 60 * 60u + 37 * 60u + 19u;
  partial_day.activate(DAY_START, DAY_END, partial_cutoff);
  check(partial_day.window_start_ts() == DAY_START &&
            partial_day.window_end_ts() ==
                DAY_START + HISTORY_BACKFILL_WINDOW_SECONDS,
        "partial current day begins with the normal fixed window");
  partial_day.record(HistoryBackfillOutcome::Success, 0);
  partial_day.record(HistoryBackfillOutcome::Success, 0);
  check(partial_day.window_start_ts() == DAY_START + 4 * 60 * 60u &&
            partial_day.window_end_ts() == partial_cutoff,
        "final partial window ends exactly at the latched live cutoff");
  partial_day.record(HistoryBackfillOutcome::NoData, 0);
  check(partial_day.succeeded() && partial_day.windows_completed() == 3,
        "an empty final window completes without retrying forever");

  HistoryBackfillRetry spring_day;
  const uint32_t spring_end = DAY_START + 23 * 60 * 60u;
  spring_day.activate(DAY_START, spring_end, spring_end);
  while (spring_day.active()) {
    spring_day.record(HistoryBackfillOutcome::Success, 0);
  }
  HistoryBackfillRetry autumn_day;
  const uint32_t autumn_end = DAY_START + 25 * 60 * 60u;
  autumn_day.activate(DAY_START, autumn_end, autumn_end);
  while (autumn_day.active()) {
    autumn_day.record(HistoryBackfillOutcome::Success, 0);
  }
  check(spring_day.windows_completed() == 12 &&
            autumn_day.windows_completed() == 13,
        "DST-aware 23- and 25-hour day bounds produce complete window sets");

  HistoryBackfillRetry retry;
  retry.activate(DAY_START, DAY_END, DAY_START + 6 * 60 * 60u);
  retry.record(HistoryBackfillOutcome::Success, 0, 10);
  const uint32_t middle_start = retry.window_start_ts();
  retry.record(HistoryBackfillOutcome::TransientFailure, 100);
  check(middle_start == DAY_START + 2 * 60 * 60u &&
            retry.window_start_ts() == middle_start && !retry.due(30099) &&
            retry.due(30100) && retry.attempts() == 1 &&
            retry.points_written() == 10,
        "transient middle-window failure retains completed work and retries in place");
  retry.record(HistoryBackfillOutcome::TransientFailure, 30100);
  check(retry.due(60100), "a second transient failure permits the final retry");
  retry.record(HistoryBackfillOutcome::TransientFailure, 60100);
  check(retry.finished() && !retry.active() && !retry.succeeded() &&
            retry.attempts() == 3 && retry.windows_completed() == 1,
        "transient failures stop after three attempts without restarting the day");

  HistoryBackfillRetry malformed;
  malformed.activate(DAY_START, DAY_END, DAY_START + 4 * 60 * 60u);
  malformed.record(HistoryBackfillOutcome::Success, 0, 7);
  malformed.record(HistoryBackfillOutcome::PermanentFailure, 0);
  check(malformed.finished() && malformed.windows_completed() == 1 &&
            malformed.points_written() == 7,
        "malformed later window stops safely while retaining earlier totals");
}

// A day payload straight from /api/day/series, at the shape the device asks for.
// Four 15-minute slots is enough to exercise every branch and short enough to
// read; the device uses 5-minute slots and 288 of them.
void test_day_series() {
  printf("day series\n");
  history_reset(HistoryBank::Live);

  // A real local midnight for the +60 offset below: minute % 1440 must be 1380,
  // so that adding the offset lands on a UTC midnight. Picking a round-looking
  // number instead just tests that the window code disagrees with the fixture.
  const uint32_t midnight_minute = 28928100;
  const uint32_t day_start = midnight_minute * 60;
  char json[512];
  snprintf(json, sizeof(json),
           "{\"slot_minutes\":15,\"day_start\":%u,\"tz_offset_min\":60,"
           "\"solar_kw\":[0.0,1.5,3.0,4.5],"
           "\"soc_pct\":[null,40.0,55.0,70.0]}",
           static_cast<unsigned>(day_start));

  // "Now" is 40 minutes into the day: slots 0 and 1 are fully elapsed, slot 2 is
  // part way through, slot 3 has not started.
  const uint32_t now = midnight_minute + 40;
  check(day_series_parse(HistoryBank::Live, json, strlen(json), now), "day payload parses");

  check(history_head_minute(HistoryBank::Live) == now, "the ring stops at now, not at the end of the day");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 41,
        "every elapsed minute filled, and none beyond");

  // A null slot is a gap, not a zero: SoC was not recorded for the first quarter
  // hour, and drawing that as 0 % would claim the battery was flat at midnight.
  check(history_sample_count(HistoryBank::Live, HistorySeries::Soc) == 26, "null slots leave gaps");

  HistoryColumn columns[4];
  history_reduce(HistoryBank::Live, HistorySeries::Pv, midnight_minute, midnight_minute + 60, columns, 4);
  check(columns[0].known && columns[0].max_value == 0.0f, "slot 0 is a recorded zero");
  check(columns[1].known && columns[1].max_value == 1.5f, "slot 1 carries its value");
  check(columns[2].known && columns[2].max_value == 3.0f, "the part-elapsed slot is filled");
  check(!columns[3].known, "the slot that has not happened stays empty");

  // The timezone came from the payload, which is the only place the server path
  // gets a trustworthy one.
  uint32_t from = 0;
  uint32_t to = 0;
  check(history_window(HistoryBank::Live, &from, &to), "window available");
  check(from == midnight_minute && to == midnight_minute + 1440,
        "window anchors to the day the payload named");

  check(!day_series_parse(HistoryBank::Live, "{\"slot_minutes\":15}", 19, now), "a payload with no day is rejected");
  check(!day_series_parse(HistoryBank::Live, json, strlen(json), 0), "no clock means nothing is filed");

  history_reset(HistoryBank::Live);
}

// The two rings are the point of HistoryBank: one ring cannot hold two days,
// because a minute more than a day behind the head is indistinguishable from a
// clock jump and rightly wipes it.
void test_history_banks() {
  printf("history banks\n");
  history_reset(HistoryBank::Live);
  history_reset(HistoryBank::Day);
  history_set_timezone(HistoryBank::Live, 0);
  history_set_timezone(HistoryBank::Day, 0);

  // Computed rather than written out: with the offset at 0 a local midnight is a
  // whole multiple of a day, and picking a round-looking number instead only
  // tests that history_window disagrees with the fixture.
  const uint32_t today = (28928100u / 1440u) * 1440u;
  const uint32_t yesterday = today - 1440;

  for (uint32_t m = 0; m <= 600; ++m) {
    history_put(HistoryBank::Live, HistorySeries::Pv, today + m, 2.0f);
  }
  for (uint32_t m = 0; m < 1440; ++m) {
    history_put(HistoryBank::Day, HistorySeries::Pv, yesterday + m, 5.0f);
  }

  // Filling the day bank must not have touched today, which is exactly what a
  // single ring could not manage.
  check(history_head_minute(HistoryBank::Live) == today + 600, "live head untouched");
  check(history_head_minute(HistoryBank::Day) == yesterday + 1439, "day head is yesterday");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 601,
        "live bank keeps its samples");
  check(history_sample_count(HistoryBank::Day, HistorySeries::Pv) == 1440,
        "day bank holds a whole day");

  uint32_t from = 0;
  uint32_t to = 0;
  check(history_window(HistoryBank::Live, &from, &to) && from == today,
        "live window anchors to today");
  check(history_window(HistoryBank::Day, &from, &to) && from == yesterday,
        "day window anchors to yesterday");

  // Values stay with their own bank.
  HistoryColumn columns[4];
  history_reduce(HistoryBank::Day, HistorySeries::Pv, yesterday, yesterday + 1440, columns, 4);
  check(columns[0].known && columns[0].max_value == 5.0f, "day bank reads back its own value");
  history_reduce(HistoryBank::Live, HistorySeries::Pv, today, today + 1440, columns, 4);
  check(columns[0].known && columns[0].max_value == 2.0f, "live bank reads back its own value");

  // The view is what the charts follow, and switching it must not disturb data.
  history_set_view(HistoryBank::Day);
  check(history_view() == HistoryBank::Day, "view follows the setter");
  history_set_view(HistoryBank::Live);

  // Clearing one leaves the other alone — stepping back to today must not cost
  // the day that was loaded.
  history_reset(HistoryBank::Day);
  check(history_sample_count(HistoryBank::Day, HistorySeries::Pv) == 0, "day bank cleared");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 601,
        "clearing the day bank spares the live one");

  history_reset(HistoryBank::Live);
}

// The button gesture recogniser, driven by synthetic timelines.
//
// Worth testing rather than eyeballing on hardware: it shipped with two bugs a
// finger found and reading did not.
namespace gesture {

// Feeds a level for `ms` milliseconds at a 5 ms tick, the rate loop() polls at,
// and records every gesture it produces.
struct Recorder {
  ButtonGestureState state;
  uint32_t now = 1000;  // not zero, so an uninitialised timestamp would show up
  int presses = 0;
  int holds = 0;
  int long_holds = 0;

  void feed(bool down, uint32_t ms) {
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 5) {
      switch (state.update(down, now)) {
        case ButtonGesture::Press: ++presses; break;
        case ButtonGesture::Hold: ++holds; break;
        case ButtonGesture::LongHold: ++long_holds; break;
        default: break;
      }
      now += 5;
    }
  }
  void tap(uint32_t ms) { feed(true, ms); feed(false, 5); }
};

}  // namespace gesture

void test_button_gestures() {
  printf("button gestures\n");
  // BOOT's shape: a bouncing GPIO, two seconds and five.
  const ButtonGestureConfig boot{40, 2000, 5000};

  {  // A press fires the moment the button comes up, with nothing to wait for.
    // This is what dropping the double press bought.
    gesture::Recorder r{{boot}};
    r.tap(120);
    check(r.presses == 1, "a press fires on release");
    r.feed(false, 600);
    check(r.presses == 1, "and only once");
    check(r.holds == 0 && r.long_holds == 0, "and is not a hold");
  }

  {  // Two taps in quick succession are simply two presses now.
    gesture::Recorder r{{boot}};
    r.tap(120);
    r.feed(false, 150);
    r.tap(120);
    r.feed(false, 300);
    check(r.presses == 2, "quick taps are two presses, not a double");
  }

  {  // Held past two seconds and let go before five.
    gesture::Recorder r{{boot}};
    r.feed(true, 2500);
    r.feed(false, 600);
    check(r.holds == 1, "a two second hold fires once");
    check(r.long_holds == 0, "and does not reach the long one");
    check(r.presses == 0, "and its release is not also a press");
  }

  {  // Held through both. The short one fires on the way, which is deliberate:
    // it is the feedback that says the button is working, and both long actions
    // end with the device restarting or powering off anyway.
    gesture::Recorder r{{boot}};
    r.feed(true, 5500);
    r.feed(false, 600);
    check(r.holds == 1 && r.long_holds == 1, "holding through fires both, once each");
    check(r.presses == 0, "and never a press");
  }

  {  // Just short of the threshold is still a press, not a hold.
    gesture::Recorder r{{boot}};
    r.feed(true, 1900);
    r.feed(false, 300);
    check(r.presses == 1 && r.holds == 0, "just under two seconds is a press");
  }

  {  // Debounce rejects contact noise on the raw GPIO...
    gesture::Recorder r{{boot}};
    r.feed(true, 10);
    r.feed(false, 300);
    check(r.presses == 0, "a bounce is not a press");
  }

  {  // ...but the PMIC button has none, because power.cpp can only report a tap
    // seen whole between two polls as lasting a single loop iteration.
    gesture::Recorder r{{ButtonGestureConfig{0, 2000, 5000}}};
    r.feed(true, 5);
    r.feed(false, 300);
    check(r.presses == 1, "an undebounced button keeps its shortest tap");
  }

  {  // A poll gap wide enough to skip both thresholds reports the one the finger
    // earned rather than the one it passed through.
    gesture::Recorder r{{boot}};
    r.state.update(true, 10000);
    check(r.state.update(true, 16000) == ButtonGesture::LongHold,
          "a skipped poll still reports the long hold");
  }
}

// --- solar forecast --------------------------------------------------------
//
// Every expected figure here was produced by running the server's own
// app/solar.py over the same inputs, not derived independently. That is the
// point of the test: the model is a port, and what matters is not that it is a
// defensible forecast but that it is the *same* forecast — one house showing two
// different numbers on two screens would be worse than showing none.

void test_solar_forecast() {
  printf("solar forecast\n");

  PvArray arrays[SOLAR_MAX_ARRAYS];
  check(!solar_site_configured(false, arrays, SOLAR_MAX_ARRAYS),
        "native forecast needs a location");
  check(!solar_site_configured(true, arrays, SOLAR_MAX_ARRAYS),
        "native forecast needs a positive-size array");
  arrays[2].kwp = 4.5f;
  check(solar_site_configured(true, arrays, SOLAR_MAX_ARRAYS),
        "native forecast accepts a location and one array");

  constexpr float LAT = 51.5072f;   // London, the reference site
  constexpr float LON = -0.1276f;

  float elevation = 0.0f;
  float azimuth = 0.0f;

  // Summer solstice, near solar noon: the year's highest sun, and almost exactly
  // due south — which is the check that the azimuth convention did not come
  // across 180 degrees out.
  solar_position(1782043200u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, 61.9399f, "solstice noon elevation");
  check_near(azimuth, 179.0198f, "solstice noon azimuth is due south");

  // A day either side, to catch a declination that is out by one day: the
  // difference is small and a whole-day slip would still look like a plausible
  // number on its own.
  solar_position(1781956800u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, 61.9352f, "day before the solstice");

  // Midnight, and midnight in the far half of the year — the sun is well below
  // the horizon and the elevation has to go negative rather than clamp.
  solar_position(1755302400u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, -25.0308f, "midnight is below the horizon");
  solar_position(1798848000u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, -61.4097f, "midwinter midnight");

  // Morning, sun low in the east.
  solar_position(1755324000u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, 9.8175f, "morning elevation");
  check_near(azimuth, 80.6894f, "morning azimuth is east of south");

  check_near(solar_poa_factor(40.0f, 180.0f, 35.0f, 180.0f), 0.965926f,
             "sun square-on to a south-facing array");
  check_near(solar_poa_factor(20.0f, 120.0f, 35.0f, 180.0f), 0.549659f, "sun off to one side");
  check_near(solar_poa_factor(5.0f, 90.0f, 90.0f, 270.0f), 0.0f, "sun behind the panel");
  check_near(solar_poa_factor(-3.0f, 180.0f, 35.0f, 180.0f), 0.0f, "sun below the horizon");

  // A synthetic day: a triangular irradiance profile peaking at midday. Made up
  // rather than fetched, so the test needs no network and the same numbers can be
  // put through the Python.
  constexpr uint32_t DAY = 1755302400u;  // 2025-08-16 00:00 UTC
  SolarHour hours[24];
  for (int h = 0; h < 24; ++h) {
    const float distance = static_cast<float>(h < 12 ? 12 - h : h - 12);
    hours[h].direct_normal = 900.0f - distance * 120.0f;
    hours[h].diffuse = 200.0f - distance * 20.0f;
    if (hours[h].direct_normal < 0.0f) {
      hours[h].direct_normal = 0.0f;
    }
    if (hours[h].diffuse < 0.0f) {
      hours[h].diffuse = 0.0f;
    }
  }

  SolarSite site;
  site.latitude = LAT;
  site.longitude = LON;
  site.system_loss = 0.85f;
  site.array_count = 2;
  site.array[0] = {5.4f, 35.0f, 180.0f};
  site.array[1] = {2.0f, 20.0f, 90.0f};  // a second roof facing east

  float slots[SOLAR_SLOTS_PER_DAY];
  solar_forecast_day(site, DAY, hours, 24, DAY, slots);
  SolarSummary summary = solar_summarise(slots, DAY, DAY);
  check_within(summary.forecast_kwh, 40.924067f, 0.001f, "two arrays over a day");
  check_within(summary.peak_kw, 6.393418f, 0.001f, "peak slot as kW");
  check(summary.remaining_kwh == summary.forecast_kwh, "all of it is still to come at midnight");

  summary = solar_summarise(slots, DAY, DAY + 12 * 3600);
  check_within(summary.remaining_kwh, 21.676796f, 0.001f, "remaining from midday");

  // The cap clips the combined throughput, so it shows up in the peak as well as
  // the total — an AC-only reading of it would leave the peak alone.
  site.inverter_cap_kw = 3.0f;
  solar_forecast_day(site, DAY, hours, 24, DAY, slots);
  summary = solar_summarise(slots, DAY, DAY);
  check_within(summary.forecast_kwh, 29.768547f, 0.001f, "clipped by the inverter cap");
  check_within(summary.peak_kw, 3.0f, 0.001f, "peak is the cap itself");

  site.inverter_cap_kw = 0.0f;
  site.array_count = 1;
  solar_forecast_day(site, DAY, hours, 24, DAY, slots);
  summary = solar_summarise(slots, DAY, DAY);
  check_within(summary.forecast_kwh, 31.161450f, 0.001f, "one array alone");

  // Hours that were not fetched contribute nothing rather than repeating the last
  // one: a truncated response should shorten the curve, not extend a plateau.
  solar_forecast_day(site, DAY, hours, 13, DAY, slots);
  summary = solar_summarise(slots, DAY, DAY);
  check(summary.forecast_kwh > 0.0f && summary.forecast_kwh < 31.0f,
        "a short response forecasts only the hours it covers");
  check(slots[SOLAR_SLOTS_PER_DAY - 1] == 0.0f, "slots past the response stay empty");

  // No arrays is not a dark day, it is nothing to forecast — and it must not read
  // back whatever the previous call left in the buffer.
  site.array_count = 0;
  solar_forecast_day(site, DAY, hours, 24, DAY, slots);
  summary = solar_summarise(slots, DAY, DAY);
  check(summary.forecast_kwh == 0.0f, "no arrays forecasts nothing");

  // The same cached native result can augment an HA Snapshot without replacing
  // any HA-acquired values or involving the Modbus representation.
  Snapshot ha_snapshot;
  ha_snapshot.valid = true;
  ha_snapshot.power.pv = {true, 2.4f};
  ha_snapshot.today.present = true;
  ha_snapshot.today.solar = {true, 6.0f};
  const SolarSummary cached = {12.0f, 4.0f, 5.5f};
  solar_summary_apply(cached, 60, &ha_snapshot);
  check(ha_snapshot.power.pv.known && ha_snapshot.power.pv.value == 2.4f,
        "native forecast leaves HA live PV intact");
  check(ha_snapshot.solar.configured, "native cache configures an HA Snapshot");
  check_near(ha_snapshot.solar.forecast_kwh.value, 12.0f,
             "native cache supplies HA forecast total");
  check_near(ha_snapshot.solar.remaining_kwh.value, 4.0f,
             "native cache supplies HA remaining forecast");
  check_near(ha_snapshot.solar.vs_forecast_pct.value, 75.0f,
             "native cache uses HA daily PV for forecast percentage");
  check_near(ha_snapshot.solar.peak_kw.value, 5.5f,
             "native cache supplies HA forecast peak");
  check(ha_snapshot.tz_offset_min.known && ha_snapshot.tz_offset_min.value == 60,
        "native cache supplies the forecast location timezone");
}

// --- screen-off window -----------------------------------------------------

void test_screen_window() {
  printf("screen window\n");

  // 22:30 to 07:00 — the case anyone actually sets, and the one that wraps.
  constexpr uint16_t NIGHT_FROM = 22 * 60 + 30;
  constexpr uint16_t NIGHT_TO = 7 * 60;
  check(screen_window_contains(NIGHT_FROM, NIGHT_TO, 23 * 60), "23:00 is inside an overnight window");
  check(screen_window_contains(NIGHT_FROM, NIGHT_TO, 3 * 60), "03:00 is inside it");
  check(screen_window_contains(NIGHT_FROM, NIGHT_TO, NIGHT_FROM), "the start minute is inside");
  check(!screen_window_contains(NIGHT_FROM, NIGHT_TO, NIGHT_TO), "the end minute is outside");
  check(!screen_window_contains(NIGHT_FROM, NIGHT_TO, 12 * 60), "midday is outside");
  check(!screen_window_contains(NIGHT_FROM, NIGHT_TO, NIGHT_FROM - 1),
        "the minute before the start is outside");

  // Wholly within a day, which is the easy case but still has to work.
  check(screen_window_contains(60, 360, 120), "02:00 is inside 01:00-06:00");
  check(!screen_window_contains(60, 360, 30), "00:30 is outside it");
  check(!screen_window_contains(60, 360, 400), "06:40 is outside it");

  // An empty window blanks nothing. The alternative reading — a whole day — is
  // the worst thing a mistyped pair of equal times could do.
  check(!screen_window_contains(600, 600, 600), "equal times are an empty window");
  check(!screen_window_contains(600, 600, 0), "equal times blank nothing at all");

  // Midnight itself, at both ends.
  check(screen_window_contains(1380, 0, 1400), "a window ending at midnight holds 23:20");
  check(!screen_window_contains(1380, 0, 0), "and lets go at midnight exactly");

  // 2026-08-16 00:00:00 UTC. The offsets below are the ones that break a naive
  // implementation: an hour east pushes into the next day, and anything west
  // pushes into the previous one, where unsigned arithmetic wraps to seventy
  // million minutes and puts the device in the afternoon.
  constexpr uint32_t MIDNIGHT_UTC = 1786924800u;
  check(screen_local_minute(MIDNIGHT_UTC, 0) == 0, "UTC midnight is minute 0");
  check(screen_local_minute(MIDNIGHT_UTC, 60) == 60, "one hour east is 01:00");
  check(screen_local_minute(MIDNIGHT_UTC, -300) == 19 * 60, "New York is 19:00 the day before");
  check(screen_local_minute(MIDNIGHT_UTC, -1) == 1439, "one minute west wraps to 23:59");
  check(screen_local_minute(MIDNIGHT_UTC, 345) == 345, "Kathmandu's three quarters of an hour");
  check(screen_local_minute(MIDNIGHT_UTC + 13 * 3600 + 37 * 60, 60) == 14 * 60 + 37,
        "a time mid-afternoon");
  // Sydney in summer is +11, so UTC 14:00 is 01:00 the next day — the wrap in
  // the other direction.
  check(screen_local_minute(MIDNIGHT_UTC + 14 * 3600, 660) == 60, "wrapping forward past midnight");
}

}  // namespace

int run_selftest() {
  s_failures = 0;
  s_checks = 0;
  test_data_sources();
  test_home_assistant_template();
  test_home_assistant_settings_metadata();
  test_home_assistant_forecast();
  test_home_assistant_payload();
  test_server_forecast();
  test_solar_metric_layout();
  test_decode();
  test_plan();
  test_snapshot();
  test_day_baseline();
  test_history();
  test_home_assistant_history();
  test_history_backfill_retry();
  test_day_series();
  test_history_banks();
  test_button_gestures();
  test_solar_forecast();
  test_screen_window();
  printf("\n%d checks, %d failed\n", s_checks, s_failures);
  return s_failures == 0 ? 0 : 1;
}

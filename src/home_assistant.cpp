#include "home_assistant.h"

#include "power_balance.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const HaEntityDescriptor HA_ENTITIES[HA_ENTITY_COUNT] = {
    {"pp", "ha_pp", "ha_pp", "PV power", "sensor.sigen_plant_pv_power", HaValueKind::Power},
    {"gp", "ha_gp", "ha_gp", "Grid power", "sensor.sigen_plant_grid_active_power",
     HaValueKind::Power},
    {"bp", "ha_bp", "ha_bp", "Battery power", "sensor.sigen_plant_battery_power",
     HaValueKind::Power},
    {"hp", "ha_hp", "ha_hp", "Home/load power", "sensor.sigen_plant_consumed_power",
     HaValueKind::Power},
    {"ep", "ha_ep", "ha_ep", "EV power", "sensor.sigen_inverter_dc_charger_output_power",
     HaValueKind::Power},
    {"xp", "ha_xp", "ha_xp", "Plant/system power", "sensor.sigen_plant_plant_active_power",
     HaValueKind::Power},
    {"og", "ha_og", "ha_og", "Off-grid state", "sensor.sigen_plant_grid_connection_status",
     HaValueKind::Boolean},
    {"soc", "ha_soc", "ha_soc", "Battery SOC", "sensor.sigen_plant_battery_state_of_charge",
     HaValueKind::Percent},
    {"soh", "ha_soh", "ha_soh", "Battery SOH", "sensor.sigen_plant_battery_state_of_health",
     HaValueKind::Percent},
    {"cap", "ha_cap", "ha_cap", "Battery capacity", "sensor.sigen_plant_rated_energy_capacity",
     HaValueKind::Energy},
    {"tmp", "ha_tmp", "ha_tmp", "Battery temperature",
     "sensor.sigen_inverter_battery_average_cell_temperature", HaValueKind::Temperature},
    {"dpv", "ha_dpv", "ha_dpv", "Today: PV generation", "sensor.sigen_plant_pv_daily_generation",
     HaValueKind::Energy},
    {"dld", "ha_dld", "ha_dld", "Today: load/consumption",
     "sensor.sigen_plant_daily_load_consumption", HaValueKind::Energy},
    {"dim", "ha_dim", "ha_dim", "Today: grid import",
     "sensor.sigen_plant_daily_grid_import_energy", HaValueKind::Energy},
    {"dex", "ha_dex", "ha_dex", "Today: grid export",
     "sensor.sigen_plant_daily_grid_export_energy", HaValueKind::Energy},
    {"dch", "ha_dch", "ha_dch", "Today: battery charge",
     "sensor.sigen_plant_daily_battery_charge_energy", HaValueKind::Energy},
    {"dds", "ha_dds", "ha_dds", "Today: battery discharge",
     "sensor.sigen_plant_daily_battery_discharge_energy", HaValueKind::Energy},
    {"s1v", "ha_s1v", "ha_s1v", "PV1 voltage", "sensor.sigen_inverter_pv1_voltage",
     HaValueKind::Voltage},
    {"s1c", "ha_s1c", "ha_s1c", "PV1 current", "sensor.sigen_inverter_pv1_current",
     HaValueKind::Current},
    {"s2v", "ha_s2v", "ha_s2v", "PV2 voltage", "sensor.sigen_inverter_pv2_voltage",
     HaValueKind::Voltage},
    {"s2c", "ha_s2c", "ha_s2c", "PV2 current", "sensor.sigen_inverter_pv2_current",
     HaValueKind::Current},
    {"s3v", "ha_s3v", "ha_s3v", "PV3 voltage", "sensor.sigen_inverter_pv3_voltage",
     HaValueKind::Voltage},
    {"s3c", "ha_s3c", "ha_s3c", "PV3 current", "sensor.sigen_inverter_pv3_current",
     HaValueKind::Current},
    {"s4v", "ha_s4v", "ha_s4v", "PV4 voltage", "sensor.sigen_inverter_pv4_voltage",
     HaValueKind::Voltage},
    {"s4c", "ha_s4c", "ha_s4c", "PV4 current", "sensor.sigen_inverter_pv4_current",
     HaValueKind::Current},
    {"sft", "ha_sft", "ha_sft", "Today forecast",
     "sensor.example_solar_forecast_today", HaValueKind::Energy},
    {"sfr", "ha_sfr", "ha_sfr", "Remaining today",
     "sensor.example_solar_forecast_remaining", HaValueKind::Energy},
    {"sfp", "ha_sfp", "ha_sfp", "Actual vs forecast",
     "sensor.example_solar_forecast_percentage", HaValueKind::Percent},
    {"sfk", "ha_sfk", "ha_sfk", "Peak power",
     "sensor.example_solar_forecast_peak", HaValueKind::Power},
};

namespace {

bool state_unavailable(const char* state) {
  return state == nullptr || strcasecmp(state, "unknown") == 0 ||
         strcasecmp(state, "unavailable") == 0 || state[0] == '\0';
}

bool numeric_state(const char* text, float* out) {
  if (state_unavailable(text)) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const float value = strtof(text, &end);
  while (end != nullptr && isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (end == text || end == nullptr || *end != '\0' || errno == ERANGE || !isfinite(value)) {
    return false;
  }
  *out = value;
  return true;
}

bool normalise(JsonVariantConst entry, HaEntity entity, MaybeFloat* out, HaParseInfo* info) {
  const char* state = entry["s"] | static_cast<const char*>(nullptr);
  const char* unit = entry["u"] | static_cast<const char*>(nullptr);
  const size_t index = static_cast<size_t>(entity);
  info->units[index] = ha_unit_from_text(unit);
  const HaValueStatus status =
      ha_normalise_state(state, info->units[index], HA_ENTITIES[index].kind, out);
  switch (status) {
    case HaValueStatus::Available:
      ++info->available;
      return true;
    case HaValueStatus::Unavailable:
      ++info->unavailable;
      break;
    case HaValueStatus::InvalidNumber:
      ++info->invalid_number;
      break;
    case HaValueStatus::UnsupportedUnit:
      ++info->unsupported_unit;
      break;
  }
  return false;
}

MaybeFloat parse_number(JsonObjectConst root, HaEntity entity, HaParseInfo* info) {
  MaybeFloat value;
  const HaEntityDescriptor& descriptor = HA_ENTITIES[static_cast<size_t>(entity)];
  if (!root[descriptor.payload_key].is<JsonObjectConst>()) {
    return value;
  }
  ++info->configured;
  normalise(root[descriptor.payload_key], entity, &value, info);
  return value;
}

void parse_boolean(JsonObjectConst root, HaEntity entity, bool* known, bool* value,
                   HaParseInfo* info) {
  const char* key = HA_ENTITIES[static_cast<size_t>(entity)].payload_key;
  if (!root[key].is<JsonObjectConst>()) {
    return;
  }
  ++info->configured;
  const char* state = root[key]["s"] | static_cast<const char*>(nullptr);
  if (state_unavailable(state)) {
    ++info->unavailable;
    return;
  }
  if (strcasecmp(state, "on") == 0 || strcasecmp(state, "true") == 0 ||
      strcmp(state, "1") == 0 || strcasecmp(state, "off_grid") == 0) {
    *known = true;
    *value = true;
    ++info->available;
  } else if (strcasecmp(state, "off") == 0 || strcasecmp(state, "false") == 0 ||
             strcmp(state, "0") == 0 || strcasecmp(state, "on_grid") == 0) {
    *known = true;
    *value = false;
    ++info->available;
  } else {
    ++info->invalid_number;
  }
}

}  // namespace

HaUnit ha_unit_from_text(const char* unit) {
  if (unit == nullptr) {
    return HaUnit::Unknown;
  }
  if (strcmp(unit, "W") == 0) {
    return HaUnit::Watts;
  }
  if (strcmp(unit, "kW") == 0) {
    return HaUnit::Kilowatts;
  }
  if (strcmp(unit, "Wh") == 0) {
    return HaUnit::WattHours;
  }
  if (strcmp(unit, "kWh") == 0) {
    return HaUnit::KilowattHours;
  }
  if (strcmp(unit, "%") == 0) {
    return HaUnit::Percent;
  }
  if (strcmp(unit, "°C") == 0 || strcmp(unit, "C") == 0 || strcmp(unit, "degC") == 0) {
    return HaUnit::Celsius;
  }
  if (strcmp(unit, "V") == 0) {
    return HaUnit::Volts;
  }
  if (strcmp(unit, "mV") == 0) {
    return HaUnit::Millivolts;
  }
  if (strcmp(unit, "A") == 0) {
    return HaUnit::Amps;
  }
  if (strcmp(unit, "mA") == 0) {
    return HaUnit::Milliamps;
  }
  return HaUnit::Unknown;
}

HaValueStatus ha_normalise_state(const char* state, HaUnit unit, HaValueKind kind,
                                 MaybeFloat* out) {
  if (out == nullptr) {
    return HaValueStatus::InvalidNumber;
  }
  *out = MaybeFloat{};
  if (state_unavailable(state)) {
    return HaValueStatus::Unavailable;
  }
  float value = 0.0f;
  if (!numeric_state(state, &value)) {
    return HaValueStatus::InvalidNumber;
  }

  bool supported = false;
  switch (kind) {
    case HaValueKind::Power:
      if (unit == HaUnit::Watts) {
        value /= 1000.0f;
        supported = true;
      } else {
        supported = unit == HaUnit::Kilowatts;
      }
      break;
    case HaValueKind::Energy:
      if (unit == HaUnit::WattHours) {
        value /= 1000.0f;
        supported = true;
      } else {
        supported = unit == HaUnit::KilowattHours;
      }
      break;
    case HaValueKind::Percent:
      supported = unit == HaUnit::Percent;
      break;
    case HaValueKind::Temperature:
      supported = unit == HaUnit::Celsius;
      break;
    case HaValueKind::Voltage:
      if (unit == HaUnit::Millivolts) {
        value /= 1000.0f;
        supported = true;
      } else {
        supported = unit == HaUnit::Volts;
      }
      break;
    case HaValueKind::Current:
      if (unit == HaUnit::Milliamps) {
        value /= 1000.0f;
        supported = true;
      } else {
        supported = unit == HaUnit::Amps;
      }
      break;
    case HaValueKind::Boolean:
      break;
  }
  if (!supported) {
    return HaValueStatus::UnsupportedUnit;
  }
  out->known = true;
  out->value = value;
  return HaValueStatus::Available;
}

bool ha_ev_is_dc(HaEvKind kind, const char* entity_id) {
  switch (kind) {
    case HaEvKind::Ac:
      return false;
    case HaEvKind::Dc:
      return true;
    case HaEvKind::Auto:
    default:
      return entity_id != nullptr && strstr(entity_id, "dc_charger") != nullptr;
  }
}

bool ha_entity_id_valid(const char* entity_id) {
  if (entity_id == nullptr || entity_id[0] == '\0' || strlen(entity_id) > HA_ENTITY_ID_MAX) {
    return false;
  }
  bool dot = false;
  bool segment_has_char = false;
  for (const char* p = entity_id; *p != '\0'; ++p) {
    if (*p == '.') {
      if (dot || !segment_has_char) {
        return false;
      }
      dot = true;
      segment_has_char = false;
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_') {
      segment_has_char = true;
    } else {
      return false;
    }
  }
  return dot && segment_has_char;
}

bool ha_template_build(const char* const entity_ids[HA_ENTITY_COUNT], char* out,
                       size_t out_size) {
  if (entity_ids == nullptr || out == nullptr || out_size == 0) {
    return false;
  }
  size_t used = 0;
  const char* prefix =
      "{\"v\":1,\"ts\":{{as_timestamp(now())|int}},"
      "\"tz\":{{(now().utcoffset().total_seconds()/60)|int}},"
      "\"mid\":{{as_timestamp(today_at())|int}},"
      "\"next\":{{as_timestamp(today_at()+timedelta(days=1))|int}}";
  const int prefix_length = snprintf(out, out_size, "%s", prefix);
  if (prefix_length < 0 || static_cast<size_t>(prefix_length) >= out_size) {
    out[0] = '\0';
    return false;
  }
  used = static_cast<size_t>(prefix_length);

  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    const char* entity = entity_ids[i];
    if (entity == nullptr || entity[0] == '\0') {
      continue;
    }
    if (!ha_entity_id_valid(entity)) {
      out[0] = '\0';
      return false;
    }
    const int written = snprintf(
        out + used, out_size - used,
        ",\"%s\":{\"s\":{{states('%s')|to_json}},\"u\":"
        "{{state_attr('%s','unit_of_measurement')|to_json}}}",
        HA_ENTITIES[i].payload_key, entity, entity);
    if (written < 0 || static_cast<size_t>(written) >= out_size - used) {
      out[0] = '\0';
      return false;
    }
    used += static_cast<size_t>(written);
  }
  if (used + 2 > out_size) {
    out[0] = '\0';
    return false;
  }
  out[used++] = '}';
  out[used] = '\0';
  return true;
}

bool ha_payload_parse(const char* json, size_t length, Snapshot* out, HaParseInfo* info,
                      const HaParseOptions* options) {
  if (json == nullptr || out == nullptr) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok ||
      !doc.is<JsonObjectConst>()) {
    return false;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if ((root["v"] | 0) != 1 || !root["ts"].is<uint32_t>()) {
    return false;
  }

  Snapshot built;
  HaParseInfo parsed_info;
  built.valid = true;
  built.version = 1;
  built.ts = root["ts"].as<uint32_t>();
  built.ok = true;
  built.age_s = 0;
  if (root["tz"].is<int>()) {
    built.tz_offset_min.known = true;
    built.tz_offset_min.value = root["tz"].as<int32_t>();
  }
  if (root["mid"].is<uint32_t>()) {
    parsed_info.local_midnight_ts = root["mid"].as<uint32_t>();
  }
  if (root["next"].is<uint32_t>()) {
    parsed_info.next_local_midnight_ts = root["next"].as<uint32_t>();
  }

  built.power.pv = parse_number(root, HaEntity::PvPower, &parsed_info);
  built.power.grid = parse_number(root, HaEntity::GridPower, &parsed_info);
  built.power.batt = parse_number(root, HaEntity::BatteryPower, &parsed_info);
  const bool home_configured =
      root[HA_ENTITIES[static_cast<size_t>(HaEntity::HomePower)].payload_key]
          .is<JsonObjectConst>();
  built.power.home = parse_number(root, HaEntity::HomePower, &parsed_info);
  built.power.plant = parse_number(root, HaEntity::PlantPower, &parsed_info);

  // An absent EV key means the optional mapping was deliberately left blank,
  // i.e. this setup has no separate EV leg. A present key whose state is bad is
  // different: it remains unknown and cannot be used in a derived house load.
  if (!root[HA_ENTITIES[static_cast<size_t>(HaEntity::EvPower)].payload_key]
           .is<JsonObjectConst>()) {
    built.power.ev.known = true;
    built.power.ev.value = 0.0f;
  } else {
    built.power.ev = parse_number(root, HaEntity::EvPower, &parsed_info);
  }
  parse_boolean(root, HaEntity::OffGrid, &built.power.off_grid_known,
                &built.power.off_grid, &parsed_info);

  // The EV is one entity of one kind; the other kind is genuinely absent.
  const bool ev_is_dc = options == nullptr || options->ev_is_dc;
  MaybeFloat no_charger;
  no_charger.known = true;
  const MaybeFloat& ev_ac = ev_is_dc ? no_charger : built.power.ev;
  const MaybeFloat& ev_dc = ev_is_dc ? built.power.ev : no_charger;
  // The strings' own power, from whichever PV pairs are mapped. Known only when
  // at least one pair is and every mapped pair read: a total short by a string
  // would show as a loss. A pair with only one half mapped is not a pair.
  MaybeFloat strings;
  {
    bool any_pair = false;
    bool all_known = true;
    float kw = 0.0f;
    for (size_t n = 0; n < HA_PV_STRINGS; ++n) {
      const HaEntity voltage_entity =
          static_cast<HaEntity>(static_cast<size_t>(HaEntity::Pv1Voltage) + 2 * n);
      const HaEntity current_entity =
          static_cast<HaEntity>(static_cast<size_t>(HaEntity::Pv1Current) + 2 * n);
      const bool voltage_mapped =
          root[HA_ENTITIES[static_cast<size_t>(voltage_entity)].payload_key]
              .is<JsonObjectConst>();
      const bool current_mapped =
          root[HA_ENTITIES[static_cast<size_t>(current_entity)].payload_key]
              .is<JsonObjectConst>();
      const MaybeFloat volts = parse_number(root, voltage_entity, &parsed_info);
      const MaybeFloat amps = parse_number(root, current_entity, &parsed_info);
      if (!voltage_mapped || !current_mapped) {
        continue;
      }
      any_pair = true;
      if (!volts.known || !amps.known) {
        all_known = false;
        continue;
      }
      kw += volts.value * amps.value / 1000.0f;
    }
    if (any_pair && all_known) {
      strings.known = true;
      strings.value = kw;
    }
  }
  const PowerBalance balance = power_balance(built.power.pv, built.power.batt, built.power.grid,
                                             built.power.plant, ev_ac, ev_dc, strings);
  if (!home_configured) {
    built.power.home = balance.home;
  }
  built.power.losses = balance.losses;
  // Solar is the strings where they were read, so the Solar node, its flow line,
  // Generating/Idle and the day chart all agree with the losses beside them.
  built.power.pv = balance.solar;

  built.battery.soc_pct = parse_number(root, HaEntity::BatterySoc, &parsed_info);
  built.battery.soh_pct = parse_number(root, HaEntity::BatterySoh, &parsed_info);
  built.battery.capacity_kwh = parse_number(root, HaEntity::BatteryCapacity, &parsed_info);
  built.battery.temp_c = parse_number(root, HaEntity::BatteryTemperature, &parsed_info);

  const HaEntity daily[] = {HaEntity::TodayPv, HaEntity::TodayLoad,
                            HaEntity::TodayGridImport, HaEntity::TodayGridExport,
                            HaEntity::TodayBatteryCharge, HaEntity::TodayBatteryDischarge};
  for (HaEntity entity : daily) {
    if (root[HA_ENTITIES[static_cast<size_t>(entity)].payload_key].is<JsonObjectConst>()) {
      built.today.present = true;
      break;
    }
  }
  built.today.solar = parse_number(root, HaEntity::TodayPv, &parsed_info);
  built.today.load = parse_number(root, HaEntity::TodayLoad, &parsed_info);
  built.today.imported = parse_number(root, HaEntity::TodayGridImport, &parsed_info);
  built.today.exported = parse_number(root, HaEntity::TodayGridExport, &parsed_info);
  built.today.charge = parse_number(root, HaEntity::TodayBatteryCharge, &parsed_info);
  built.today.discharge = parse_number(root, HaEntity::TodayBatteryDischarge, &parsed_info);

  built.solar.forecast_kwh = parse_number(root, HaEntity::ForecastToday, &parsed_info);
  built.solar.remaining_kwh = parse_number(root, HaEntity::ForecastRemaining, &parsed_info);
  built.solar.vs_forecast_pct = parse_number(root, HaEntity::ForecastPercentage, &parsed_info);
  built.solar.peak_kw = parse_number(root, HaEntity::ForecastPeak, &parsed_info);
  // Today's total is the Solar screen's anchor: it supplies the denominator for
  // the ring and gives the optional remaining/percentage/peak figures context.
  // A total alone is useful; the three supporting mappings need not all exist.
  built.solar.configured = built.solar.forecast_kwh.known;

  *out = built;
  if (info != nullptr) {
    *info = parsed_info;
  }
  return true;
}

HaHttpStatus ha_http_status(int status_code) {
  if (status_code == 200) {
    return HaHttpStatus::Ok;
  }
  if (status_code == 401 || status_code == 403) {
    return HaHttpStatus::Unauthorised;
  }
  return HaHttpStatus::HttpError;
}

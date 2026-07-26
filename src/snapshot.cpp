#include "snapshot.h"

#include <ArduinoJson.h>

namespace {

// isNull() is true for both an explicit JSON null and an absent key, which is
// exactly the distinction we want to collapse: either way the value is unknown.
MaybeFloat maybe_float(JsonVariantConst value) {
  MaybeFloat out;
  if (!value.isNull()) {
    out.known = true;
    out.value = value.as<float>();
  }
  return out;
}

MaybeInt maybe_int(JsonVariantConst value) {
  MaybeInt out;
  if (!value.isNull()) {
    out.known = true;
    out.value = value.as<int32_t>();
  }
  return out;
}

}  // namespace

bool snapshot_parse(const char* json, size_t length, Snapshot* out) {
  if (json == nullptr || out == nullptr) {
    return false;
  }

  // ArduinoJson v7's elastic document. At ~400 bytes this needs no PSRAM
  // allocator, which is the entire point of the server-side /api/summary.
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) {
    return false;
  }
  if (!doc.is<JsonObjectConst>()) {
    return false;
  }

  // Built up separately and only committed on success, so a partial parse never
  // half-overwrites the caller's last good snapshot.
  Snapshot parsed;
  parsed.valid = true;
  parsed.version = doc["v"] | 0;
  parsed.ts = doc["ts"] | 0u;
  parsed.ok = doc["ok"] | false;
  parsed.age_s = doc["age"] | 0u;
  parsed.alarms = doc["alarms"] | 0;
  // Absent from every payload the server sends today, which is why this is a
  // MaybeInt and not a plain 0: an offset of zero is UTC, not "unknown".
  parsed.tz_offset_min = maybe_int(doc["tz"]);

  // Missing sub-objects resolve to null variants, so every field below simply
  // comes out "unknown" rather than needing its own presence check.
  JsonVariantConst power = doc["power"];
  parsed.power.pv = maybe_float(power["pv"]);
  parsed.power.grid = maybe_float(power["grid"]);
  parsed.power.batt = maybe_float(power["batt"]);
  parsed.power.home = maybe_float(power["home"]);
  parsed.power.ev = maybe_float(power["ev"]);
  parsed.power.plant = maybe_float(power["plant"]);
  parsed.power.off_grid = power["off_grid"] | false;

  JsonVariantConst battery = doc["battery"];
  parsed.battery.soc_pct = maybe_float(battery["soc"]);
  parsed.battery.soh_pct = maybe_float(battery["soh"]);
  parsed.battery.capacity_kwh = maybe_float(battery["capacity_kwh"]);
  parsed.battery.temp_c = maybe_float(battery["temp_c"]);
  parsed.battery.eta_min = maybe_int(battery["eta_min"]);

  // The one the payload contract calls out: null, not an object, on a bad day.
  JsonVariantConst today = doc["today"];
  if (today.is<JsonObjectConst>()) {
    parsed.today.present = true;
    parsed.today.solar = maybe_float(today["solar"]);
    parsed.today.imported = maybe_float(today["import"]);
    parsed.today.exported = maybe_float(today["export"]);
    parsed.today.load = maybe_float(today["load"]);
    parsed.today.charge = maybe_float(today["charge"]);
    parsed.today.discharge = maybe_float(today["discharge"]);
  }

  JsonVariantConst cost = doc["cost"];
  parsed.cost.configured = cost["configured"] | false;
  parsed.cost.saving_gbp = maybe_float(cost["saving_gbp"]);
  parsed.cost.rate_p = maybe_float(cost["rate_p"]);
  for (JsonVariantConst slot : cost["next"].as<JsonArrayConst>()) {
    if (parsed.cost.next_count >= SNAPSHOT_MAX_TARIFF_SLOTS) {
      break;
    }
    TariffSlot& target = parsed.cost.next[parsed.cost.next_count];
    target.from = slot["from"] | 0u;
    target.pence = slot["p"] | 0.0f;
    ++parsed.cost.next_count;
  }

  *out = parsed;
  return true;
}

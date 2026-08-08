#include "day_series.h"

#include <ArduinoJson.h>

#include "history.h"

namespace {

// A day at the finest slot the server offers is 288 entries a series. Anything
// claiming more than a day of slots is not a day payload.
constexpr size_t MAX_SLOTS = 1440;

// Writes one slot's value across every minute it covers.
//
// The value is flat within the slot, so the min/max envelope collapses to a line
// there rather than showing the spread a live-recorded minute would. That is
// honest: the server has already reduced those minutes, and inventing a spread
// to make the band look busier would be drawing something nobody measured.
void fill_series(HistoryBank bank, HistorySeries series, JsonArrayConst values,
                 uint32_t first_minute, uint32_t slot_minutes, uint32_t now_minute) {
  size_t index = 0;
  for (JsonVariantConst value : values) {
    if (index >= MAX_SLOTS) {
      break;
    }
    const uint32_t slot_start = first_minute + index * slot_minutes;
    ++index;

    // Past the end of the elapsed day: everything from here on is zero only
    // because it has not happened yet.
    if (slot_start > now_minute) {
      break;
    }
    // Null is "not recorded" — a gap must stay a gap. soc_pct carries these for
    // every slot before the battery was first read today.
    if (value.isNull()) {
      continue;
    }

    const float reading = value.as<float>();
    for (uint32_t offset = 0; offset < slot_minutes; ++offset) {
      const uint32_t minute = slot_start + offset;
      if (minute > now_minute) {
        break;
      }
      history_put(bank, series, minute, reading);
    }
  }
}

}  // namespace

bool day_series_parse(HistoryBank bank, const char* json, size_t length, uint32_t now_minute) {
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) {
    return false;
  }

  const uint32_t day_start = doc["day_start"] | 0u;
  const int slot_minutes = doc["slot_minutes"] | 0;
  if (day_start == 0 || slot_minutes <= 0 || now_minute == 0) {
    return false;
  }

  // The timezone the server anchored the day to. This is the only place the
  // Puck gets a trustworthy one on the server path: /api/summary does not carry
  // it, and the Modbus register that would is documented as reading 0 even on
  // BST. Without it the charts fall back to a rolling 24 h window that spans two
  // part-days and reads as a U.
  JsonVariantConst tz = doc["tz_offset_min"];
  if (!tz.isNull()) {
    history_set_timezone(bank, tz.as<int32_t>());
  }

  const uint32_t first_minute = day_start / 60;
  fill_series(bank, HistorySeries::Pv, doc["solar_kw"].as<JsonArrayConst>(), first_minute,
              static_cast<uint32_t>(slot_minutes), now_minute);
  fill_series(bank, HistorySeries::Soc, doc["soc_pct"].as<JsonArrayConst>(), first_minute,
              static_cast<uint32_t>(slot_minutes), now_minute);
  return true;
}

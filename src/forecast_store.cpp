#include "forecast_store.h"

namespace {

constexpr size_t MAX_SLOTS = 50;  // 48 half-hour slots in a day, plus slack

float s_slot_kw[MAX_SLOTS] = {};
size_t s_slots = 0;
uint32_t s_day_start = 0;   // local midnight, unix-minutes
uint32_t s_slot_minutes = 30;
bool s_valid = false;

}  // namespace

void forecast_store_set(const float* slot_kwh, size_t slots, uint32_t day_start_minute,
                        uint32_t slot_minutes) {
  if (slot_kwh == nullptr || slots == 0 || slot_minutes == 0) {
    forecast_store_clear();
    return;
  }
  if (slots > MAX_SLOTS) {
    slots = MAX_SLOTS;
  }
  const float hours = static_cast<float>(slot_minutes) / 60.0f;
  for (size_t i = 0; i < slots; ++i) {
    s_slot_kw[i] = slot_kwh[i] / hours;  // energy per slot -> average kW in the slot
  }
  s_slots = slots;
  s_day_start = day_start_minute;
  s_slot_minutes = slot_minutes;
  s_valid = true;
}

void forecast_store_clear() {
  s_valid = false;
  s_slots = 0;
}

bool forecast_store_columns(uint32_t from_minute, uint32_t to_minute, HistoryColumn* out,
                            size_t columns, float* out_peak) {
  if (!s_valid || out == nullptr || columns == 0 || to_minute <= from_minute) {
    return false;
  }
  // Only for the forecast's own day: the window must begin at the forecast's day
  // start (a couple of minutes of tolerance for the ring's boundary vs ours).
  const int64_t diff =
      static_cast<int64_t>(from_minute) - static_cast<int64_t>(s_day_start);
  if (diff < -2 || diff > 2) {
    return false;
  }

  const float span_min = static_cast<float>(to_minute - from_minute);
  const float per_min = 1.0f / static_cast<float>(s_slot_minutes);
  const float last = static_cast<float>(s_slots - 1);
  float peak = 0.0f;

  for (size_t c = 0; c < columns; ++c) {
    // Minutes from the forecast day start to this column's centre.
    const float t = (static_cast<float>(from_minute) - static_cast<float>(s_day_start)) +
                    (static_cast<float>(c) + 0.5f) / static_cast<float>(columns) * span_min;
    const float slot_pos = t * per_min;
    float value = 0.0f;
    if (slot_pos >= 0.0f && slot_pos <= last) {
      const size_t i = static_cast<size_t>(slot_pos);
      const float frac = slot_pos - static_cast<float>(i);
      const float a = s_slot_kw[i];
      const float b = (i + 1 < s_slots) ? s_slot_kw[i + 1] : a;
      value = a + (b - a) * frac;
    }
    out[c].known = true;
    out[c].min_value = value;
    out[c].max_value = value;
    if (value > peak) {
      peak = value;
    }
  }
  if (out_peak != nullptr) {
    *out_peak = peak;
  }
  return true;
}

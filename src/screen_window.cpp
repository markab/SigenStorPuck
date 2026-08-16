#include "screen_window.h"

uint16_t screen_local_minute(uint32_t epoch_utc, int32_t tz_offset_min) {
  // Signed throughout: west of Greenwich the offset is negative, and an epoch in
  // the first hour of a UTC day goes below zero before it is reduced. Doing this
  // in unsigned arithmetic wraps to about 71 million minutes and lands the
  // device somewhere in the middle of the afternoon.
  int64_t minutes = static_cast<int64_t>(epoch_utc) / 60 + tz_offset_min;
  minutes %= SCREEN_MINUTES_PER_DAY;
  if (minutes < 0) {
    minutes += SCREEN_MINUTES_PER_DAY;
  }
  return static_cast<uint16_t>(minutes);
}

bool screen_window_contains(uint16_t start_min, uint16_t end_min, uint16_t now_min) {
  if (start_min == end_min) {
    return false;
  }
  if (start_min < end_min) {
    // Wholly within one day: 01:00 to 06:00.
    return now_min >= start_min && now_min < end_min;
  }
  // Wraps midnight: 22:30 to 07:00 is "after 22:30 or before 07:00".
  return now_min >= start_min || now_min < end_min;
}

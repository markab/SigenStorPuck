#include "format.h"

#include <math.h>
#include <stdio.h>

namespace {
constexpr const char* UNKNOWN = "--";
}

void puck_format_magnitude(const MaybeFloat& value, int decimals, char* out, size_t size) {
  if (!value.known) {
    snprintf(out, size, "%s", UNKNOWN);
    return;
  }
  snprintf(out, size, "%.*f", decimals, fabsf(value.value));
}

void puck_format_signed(const MaybeFloat& value, int decimals, char* out, size_t size) {
  if (!value.known) {
    snprintf(out, size, "%s", UNKNOWN);
    return;
  }
  snprintf(out, size, "%.*f", decimals, value.value);
}

void puck_format_duration(const MaybeInt& minutes, char* out, size_t size) {
  if (!minutes.known || minutes.value < 0) {
    snprintf(out, size, "%s", UNKNOWN);
    return;
  }
  const int32_t hours = minutes.value / 60;
  const int32_t rest = minutes.value % 60;
  if (hours == 0) {
    snprintf(out, size, "%dm", rest);
  } else {
    snprintf(out, size, "%dh %02dm", hours, rest);
  }
}

void puck_format_offset(int32_t seconds_ahead, char* out, size_t size) {
  // A slot that has already started is the one in force, not a future one.
  if (seconds_ahead <= 60) {
    snprintf(out, size, "now");
    return;
  }
  const int32_t minutes = seconds_ahead / 60;
  const int32_t hours = minutes / 60;
  const int32_t rest = minutes % 60;
  if (hours == 0) {
    snprintf(out, size, "in %dm", minutes);
  } else if (rest == 0) {
    snprintf(out, size, "in %dh", hours);
  } else {
    snprintf(out, size, "in %dh %02dm", hours, rest);
  }
}

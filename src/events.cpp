#include "events.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

// UTF-8, because the fonts carry these glyphs (tools/generate_fonts.sh).
constexpr const char* EN_DASH = "\xE2\x80\x93";
constexpr const char* MIDDLE_DOT = "\xC2\xB7";
constexpr const char* POUND = "\xC2\xA3";

// "Tonight" from this local hour on; earlier the same day is "Today".
constexpr int TONIGHT_FROM_HOUR = 17;

// Same collapse as snapshot.cpp: an explicit null and an absent key are both
// simply unknown.
MaybeFloat maybe_float(JsonVariantConst value) {
  MaybeFloat out;
  if (!value.isNull() && (value.is<float>() || value.is<int>())) {
    out.known = true;
    out.value = value.as<float>();
  }
  return out;
}

MaybeInt maybe_int(JsonVariantConst value) {
  MaybeInt out;
  if (!value.isNull() && (value.is<int32_t>() || value.is<float>())) {
    out.known = true;
    out.value = value.as<int32_t>();
  }
  return out;
}

EventKind kind_from(const char* text) {
  if (text == nullptr) {
    return EventKind::Unknown;
  }
  if (strcmp(text, "saving") == 0) return EventKind::Saving;
  if (strcmp(text, "happy_hour") == 0) return EventKind::HappyHour;
  if (strcmp(text, "free_import") == 0) return EventKind::FreeImport;
  if (strcmp(text, "axle") == 0) return EventKind::Axle;
  return EventKind::Unknown;
}

EventState state_from(const char* text) {
  if (text == nullptr) {
    return EventState::Unknown;
  }
  if (strcmp(text, "live") == 0) return EventState::Live;
  if (strcmp(text, "upcoming") == 0) return EventState::Upcoming;
  if (strcmp(text, "done") == 0) return EventState::Done;
  if (strcmp(text, "deferred") == 0) return EventState::Deferred;
  return EventState::Unknown;
}

EventDirection direction_from(const char* text) {
  if (text == nullptr) {
    return EventDirection::Unknown;
  }
  if (strcmp(text, "export") == 0) return EventDirection::Export;
  if (strcmp(text, "import") == 0) return EventDirection::Import;
  return EventDirection::Unknown;
}

// A time that is not a positive number is not a time. In particular a null end
// must not become 1970.
bool read_time(JsonVariantConst value, uint32_t* out) {
  if (value.isNull() || !(value.is<uint32_t>() || value.is<int64_t>() || value.is<float>())) {
    return false;
  }
  const int64_t seconds = value.as<int64_t>();
  if (seconds <= 0 || seconds > 0xFFFFFFFFLL) {
    return false;
  }
  *out = static_cast<uint32_t>(seconds);
  return true;
}

GridEvent parse_event(JsonObjectConst object) {
  GridEvent event;
  event.kind = kind_from(object["kind"].as<const char*>());
  event.state = state_from(object["state"].as<const char*>());
  event.direction = direction_from(object["direction"].as<const char*>());
  read_time(object["start"], &event.start);
  event.has_end = read_time(object["end"], &event.end);
  event.import_kwh = maybe_float(object["import_kwh"]);
  event.export_kwh = maybe_float(object["export_kwh"]);
  event.cut_kwh = maybe_float(object["cut_kwh"]);
  event.cap_kwh = maybe_float(object["cap_kwh"]);
  event.earned_gbp = maybe_float(object["earned_gbp"]);
  event.price_p = maybe_float(object["price_p"]);
  event.soc_start = maybe_float(object["soc_start"]);
  event.soc_end = maybe_float(object["soc_end"]);
  event.points = maybe_int(object["points"]);
  // Only meaningful beside a figure it qualifies.
  event.points_estimated = event.points.known && (object["points_estimated"] | false);
  return event;
}

// Local calendar parts of a UTC instant shifted by the site's offset. The device
// clock is UTC on purpose (net.cpp), so this is done by hand rather than through
// localtime(), which would only ever see UTC.
bool local_parts(uint32_t utc, int32_t tz_offset_min, struct tm* parts) {
  const time_t shifted = static_cast<time_t>(static_cast<int64_t>(utc) +
                                             static_cast<int64_t>(tz_offset_min) * 60);
  return gmtime_r(&shifted, parts) != nullptr;
}

int64_t local_day(uint32_t utc, int32_t tz_offset_min) {
  const int64_t local = static_cast<int64_t>(utc) + static_cast<int64_t>(tz_offset_min) * 60;
  // Floor, not truncation, so a negative local instant cannot land on day 0.
  return local >= 0 ? local / 86400 : (local - 86399) / 86400;
}

void format_kwh(const MaybeFloat& value, char* out, size_t size) {
  if (!value.known) {
    snprintf(out, size, "-- kWh");
  } else {
    snprintf(out, size, "%.1f kWh", value.value);
  }
}

// "1,520" — points run into the thousands and read better grouped.
void format_grouped(int32_t value, char* out, size_t size) {
  char digits[16];
  const bool negative = value < 0;
  snprintf(digits, sizeof(digits), "%ld",
           static_cast<long>(negative ? -static_cast<int64_t>(value) : value));
  char grouped[24];
  size_t length = strlen(digits);
  size_t at = 0;
  if (negative) {
    grouped[at++] = '-';
  }
  for (size_t i = 0; i < length && at + 1 < sizeof(grouped); ++i) {
    if (i > 0 && (length - i) % 3 == 0) {
      grouped[at++] = ',';
    }
    grouped[at++] = digits[i];
  }
  grouped[at] = '\0';
  snprintf(out, size, "%s", grouped);
}

void set_figure(EventFigure* figure, const char* value, const char* caption) {
  snprintf(figure->value, sizeof(figure->value), "%s", value);
  snprintf(figure->caption, sizeof(figure->caption), "%s", caption);
}

// "42 min", "1 h 05 min", "2 h".
void format_span(uint32_t seconds, char* out, size_t size) {
  const uint32_t minutes = (seconds + 59) / 60;  // never say "0 min" with time left
  const uint32_t hours = minutes / 60;
  const uint32_t rest = minutes % 60;
  if (hours == 0) {
    snprintf(out, size, "%u min", static_cast<unsigned>(minutes));
  } else if (rest == 0) {
    snprintf(out, size, "%u h", static_cast<unsigned>(hours));
  } else {
    snprintf(out, size, "%u h %02u min", static_cast<unsigned>(hours),
             static_cast<unsigned>(rest));
  }
}

}  // namespace

bool events_parse(const char* json, size_t length, EventList* out) {
  if (json == nullptr || out == nullptr) {
    return false;
  }
  // A full list is a couple of KB; the elastic document handles it on the heap
  // briefly and is gone before the caller does anything else.
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) {
    return false;
  }
  if (!doc.is<JsonObjectConst>()) {
    return false;
  }
  uint32_t now = 0;
  if (!read_time(doc["now"], &now)) {
    return false;
  }

  // Nothing below can fail, so `out` is only written once the payload is known
  // to be a list.
  *out = EventList();
  out->valid = true;
  out->now = now;

  JsonVariantConst current = doc["current"];
  if (current.is<JsonObjectConst>()) {
    out->has_current = true;
    out->current = parse_event(current.as<JsonObjectConst>());
  }

  // Absent, null or not an array: no past events, not a broken payload.
  for (JsonVariantConst entry : doc["past"].as<JsonArrayConst>()) {
    if (out->past_count >= EVENTS_MAX_PAST) {
      break;
    }
    if (!entry.is<JsonObjectConst>()) {
      continue;
    }
    out->past[out->past_count++] = parse_event(entry.as<JsonObjectConst>());
  }
  return true;
}

bool events_renderable(const GridEvent& event) {
  return event.kind != EventKind::Unknown && event.state != EventState::Unknown &&
         event.start != 0;
}

const GridEvent* events_featured(const EventList& list) {
  if (!list.valid) {
    return nullptr;
  }
  if (list.has_current && events_renderable(list.current)) {
    return &list.current;
  }
  for (uint8_t i = 0; i < list.past_count && i < EVENTS_MAX_PAST; ++i) {
    if (events_renderable(list.past[i])) {
      return &list.past[i];
    }
  }
  return nullptr;
}

EventState events_state_at(const GridEvent& event, uint32_t now) {
  const bool ended = event.has_end && now >= event.end;
  switch (event.state) {
    case EventState::Upcoming:
      if (now < event.start) {
        return EventState::Upcoming;
      }
      return ended ? EventState::Done : EventState::Live;
    case EventState::Live:
      return ended ? EventState::Done : EventState::Live;
    default:
      return event.state;
  }
}

const char* events_title(const GridEvent& event) {
  switch (event.kind) {
    case EventKind::Saving:
      return "SAVING SESSION";
    case EventKind::HappyHour:
      return "HAPPY HOUR";
    case EventKind::FreeImport:
      return "FREE IMPORT";
    case EventKind::Axle:
      switch (event.direction) {
        case EventDirection::Export:
          return "AXLE EXPORT";
        case EventDirection::Import:
          return "AXLE IMPORT";
        default:
          return "AXLE EVENT";
      }
    case EventKind::Unknown:
      break;
  }
  return "";
}

void events_format_when(const GridEvent& event, uint32_t now, int32_t tz_offset_min,
                        char* out, size_t size) {
  if (out == nullptr || size == 0) {
    return;
  }
  struct tm start = {};
  if (event.start == 0 || !local_parts(event.start, tz_offset_min, &start)) {
    snprintf(out, size, "--");
    return;
  }

  static const char* const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char day[20];
  const int64_t days_away = local_day(event.start, tz_offset_min) - local_day(now, tz_offset_min);
  if (days_away == 0) {
    snprintf(day, sizeof(day), "%s", start.tm_hour >= TONIGHT_FROM_HOUR ? "Tonight" : "Today");
  } else if (days_away == 1) {
    snprintf(day, sizeof(day), "Tomorrow");
  } else if (days_away == -1) {
    snprintf(day, sizeof(day), "Yesterday");
  } else {
    snprintf(day, sizeof(day), "%s %d %s", DAYS[start.tm_wday % 7], start.tm_mday,
             MONTHS[start.tm_mon % 12]);
  }

  struct tm end = {};
  if (event.has_end && local_parts(event.end, tz_offset_min, &end)) {
    snprintf(out, size, "%s %02d:%02d%s%02d:%02d", day, start.tm_hour, start.tm_min, EN_DASH,
             end.tm_hour, end.tm_min);
  } else {
    snprintf(out, size, "%s %02d:%02d", day, start.tm_hour, start.tm_min);
  }
}

void events_format_status(const GridEvent& event, uint32_t now, char* out, size_t size) {
  if (out == nullptr || size == 0) {
    return;
  }
  char span[24];
  switch (events_state_at(event, now)) {
    case EventState::Live:
      if (event.has_end && event.end > now) {
        format_span(event.end - now, span, sizeof(span));
        snprintf(out, size, "Live %s %s left", MIDDLE_DOT, span);
      } else {
        snprintf(out, size, "Live");
      }
      return;
    case EventState::Upcoming: {
      const uint32_t ahead = event.start > now ? event.start - now : 0;
      if (ahead < 60) {
        snprintf(out, size, "Starting");
      } else if (ahead < 48u * 3600u) {
        format_span(ahead, span, sizeof(span));
        snprintf(out, size, "In %s", span);
      } else {
        snprintf(out, size, "In %u days", static_cast<unsigned>(ahead / 86400u));
      }
      return;
    }
    case EventState::Done:
      snprintf(out, size, "Done");
      return;
    case EventState::Deferred:
      snprintf(out, size, "Deferred");
      return;
    case EventState::Unknown:
      break;
  }
  snprintf(out, size, "--");
}

size_t events_figures(const GridEvent& event, EventFigure* out, size_t max) {
  if (out == nullptr || max == 0) {
    return 0;
  }
  char value[24];
  size_t count = 0;

  // Before it starts every outcome is unknown, and a row of dashes would only
  // say so at length. A booked event shows what is known ahead instead, if
  // anything: the allowance, the price, points already estimated for a joined
  // session.
  if (event.state == EventState::Upcoming) {
    switch (event.kind) {
      case EventKind::HappyHour:
        if (event.cap_kwh.known) {
          snprintf(value, sizeof(value), "%g kWh", event.cap_kwh.value);
          set_figure(&out[count++], value, "FREE UP TO");
        }
        break;
      case EventKind::Axle:
        if (event.price_p.known) {
          snprintf(value, sizeof(value), "%.1fp/kWh", event.price_p.value);
          set_figure(&out[count++], value, "PRICE");
        }
        break;
      case EventKind::Saving:
        if (event.points.known) {
          char grouped[16];
          format_grouped(event.points.value, grouped, sizeof(grouped));
          snprintf(value, sizeof(value), "%s%s pts", event.points_estimated ? "~" : "",
                   grouped);
          set_figure(&out[count++], value, event.points_estimated ? "ESTIMATED" : "POINTS");
        }
        break;
      default:
        break;
    }
    return count;
  }

  switch (event.kind) {
    case EventKind::Saving: {
      format_kwh(event.cut_kwh, value, sizeof(value));
      set_figure(&out[count++], value, "CUT");
      // A deferred session earns nothing, so it has no points to show.
      if (event.state != EventState::Deferred && count < max) {
        if (!event.points.known) {
          set_figure(&out[count++], "-- pts", "POINTS");
        } else {
          char grouped[16];
          format_grouped(event.points.value, grouped, sizeof(grouped));
          snprintf(value, sizeof(value), "%s%s pts", event.points_estimated ? "~" : "",
                   grouped);
          set_figure(&out[count++], value, event.points_estimated ? "ESTIMATED" : "AWARDED");
        }
      }
      break;
    }
    case EventKind::HappyHour:
      if (event.cap_kwh.known) {
        char used[12];
        if (event.import_kwh.known) {
          snprintf(used, sizeof(used), "%.1f", event.import_kwh.value);
        } else {
          snprintf(used, sizeof(used), "--");
        }
        snprintf(value, sizeof(value), "%s of %g kWh", used, event.cap_kwh.value);
      } else {
        format_kwh(event.import_kwh, value, sizeof(value));
      }
      set_figure(&out[count++], value, "IMPORTED");
      break;
    case EventKind::FreeImport:
      format_kwh(event.import_kwh, value, sizeof(value));
      set_figure(&out[count++], value, "IMPORTED");
      break;
    case EventKind::Axle: {
      // Direction decides which energy figure is the point; without one, the
      // figure that is known wins, export first.
      const bool import = event.direction == EventDirection::Import ||
                          (event.direction == EventDirection::Unknown &&
                           !event.export_kwh.known && event.import_kwh.known);
      format_kwh(import ? event.import_kwh : event.export_kwh, value, sizeof(value));
      set_figure(&out[count++], value, import ? "IMPORTED" : "EXPORTED");
      if (count < max) {
        if (event.earned_gbp.known) {
          snprintf(value, sizeof(value), "%s%.2f", POUND, event.earned_gbp.value);
        } else {
          snprintf(value, sizeof(value), "%s--", POUND);
        }
        set_figure(&out[count++], value, "EARNED");
      }
      break;
    }
    case EventKind::Unknown:
      break;
  }
  return count;
}

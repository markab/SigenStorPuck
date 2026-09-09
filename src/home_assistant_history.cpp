#include "home_assistant_history.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <string.h>

namespace {

constexpr int16_t NO_EVENT = INT16_MIN;
constexpr int16_t UNKNOWN_EVENT = INT16_MIN + 1;

int16_t encode_event(float value) {
  const float scaled = value * static_cast<float>(HISTORY_SCALE);
  if (scaled >= static_cast<float>(INT16_MAX)) {
    return INT16_MAX;
  }
  if (scaled <= static_cast<float>(INT16_MIN + 2)) {
    return INT16_MIN + 2;
  }
  return static_cast<int16_t>(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

float decode_event(int16_t value) {
  return static_cast<float>(value) / static_cast<float>(HISTORY_SCALE);
}

bool parse_digits(const char* text, size_t offset, size_t count, int* value) {
  int parsed = 0;
  for (size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + c - '0';
  }
  *value = parsed;
  return true;
}

bool leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  if (month == 2 && leap_year(year)) {
    return 29;
  }
  return DAYS[month - 1];
}

// Days since 1970-01-01. This civil-calendar conversion does not consult the
// process timezone, so host tests and firmware produce identical epochs.
int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned shifted_month =
      static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
  const unsigned doy = (153 * shifted_month + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool relevant_entity(HaEntity entity) {
  return entity == HaEntity::PvPower || entity == HaEntity::GridPower ||
         entity == HaEntity::BatteryPower || entity == HaEntity::HomePower ||
         entity == HaEntity::EvPower || entity == HaEntity::BatterySoc;
}

}  // namespace

const char* ha_history_parse_error_name(HaHistoryParseError error) {
  switch (error) {
    case HaHistoryParseError::None:
      return "none";
    case HaHistoryParseError::InvalidConfiguration:
      return "invalid parser configuration";
    case HaHistoryParseError::NoMemory:
      return "history workspace allocation failed";
    case HaHistoryParseError::UnexpectedTopLevelToken:
      return "unexpected top-level token";
    case HaHistoryParseError::UnexpectedSeriesToken:
      return "unexpected inner-array token";
    case HaHistoryParseError::UnexpectedDelimiter:
      return "unexpected array delimiter";
    case HaHistoryParseError::ObjectTooLarge:
      return "state object exceeds size limit";
    case HaHistoryParseError::MalformedStateObject:
      return "malformed state object";
    case HaHistoryParseError::MissingEntityId:
      return "missing entity_id on first object";
    case HaHistoryParseError::InvalidEntityId:
      return "invalid entity_id";
    case HaHistoryParseError::InvalidTimestamp:
      return "invalid timestamp";
    case HaHistoryParseError::IncompletePayload:
      return "incomplete payload";
    case HaHistoryParseError::TrailingData:
      return "trailing data after top-level array";
  }
  return "unknown parser error";
}

size_t ha_history_fields_build(const char* const entity_ids[HA_ENTITY_COUNT],
                               const HaUnit units[HA_ENTITY_COUNT],
                               HaHistoryField out[HA_HISTORY_MAX_FIELDS]) {
  if (entity_ids == nullptr || units == nullptr || out == nullptr) {
    return 0;
  }
  size_t count = 0;
  const auto add = [&](HaEntity entity) {
    const size_t index = static_cast<size_t>(entity);
    if (entity_ids[index] == nullptr || entity_ids[index][0] == '\0' ||
        count >= HA_HISTORY_MAX_FIELDS) {
      return;
    }
    out[count++] = {entity, entity_ids[index], units[index]};
  };

  add(HaEntity::PvPower);
  add(HaEntity::BatterySoc);
  const size_t home_index = static_cast<size_t>(HaEntity::HomePower);
  if (entity_ids[home_index] != nullptr && entity_ids[home_index][0] != '\0') {
    add(HaEntity::HomePower);
    add(HaEntity::EvPower);
  } else {
    // All three are retained independently. If Recorder omits one of them, the
    // load curve simply has a gap rather than manufacturing a zero.
    add(HaEntity::GridPower);
    add(HaEntity::BatteryPower);
  }
  return count;
}

bool ha_history_timestamp_parse(const char* text, uint32_t* timestamp) {
  if (text == nullptr || timestamp == nullptr || strlen(text) < 20) {
    return false;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_digits(text, 0, 4, &year) || text[4] != '-' ||
      !parse_digits(text, 5, 2, &month) || text[7] != '-' ||
      !parse_digits(text, 8, 2, &day) || (text[10] != 'T' && text[10] != ' ') ||
      !parse_digits(text, 11, 2, &hour) || text[13] != ':' ||
      !parse_digits(text, 14, 2, &minute) || text[16] != ':' ||
      !parse_digits(text, 17, 2, &second) || month < 1 || month > 12 || day < 1 ||
      day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 60) {
    return false;
  }

  size_t offset = 19;
  if (text[offset] == '.') {
    ++offset;
    if (!isdigit(static_cast<unsigned char>(text[offset]))) {
      return false;
    }
    while (isdigit(static_cast<unsigned char>(text[offset]))) {
      ++offset;
    }
  }

  int offset_seconds = 0;
  if (text[offset] == 'Z' && text[offset + 1] == '\0') {
    // already UTC
  } else if ((text[offset] == '+' || text[offset] == '-') &&
             strlen(text + offset) == 6 && text[offset + 3] == ':') {
    int offset_hour = 0;
    int offset_minute = 0;
    if (!parse_digits(text, offset + 1, 2, &offset_hour) ||
        !parse_digits(text, offset + 4, 2, &offset_minute) || offset_hour > 23 ||
        offset_minute > 59) {
      return false;
    }
    offset_seconds = (offset_hour * 60 + offset_minute) * 60;
    if (text[offset] == '-') {
      offset_seconds = -offset_seconds;
    }
  } else {
    return false;
  }

  const int64_t epoch = days_from_civil(year, static_cast<unsigned>(month),
                                        static_cast<unsigned>(day)) * 86400 +
                        hour * 3600 + minute * 60 + (second == 60 ? 59 : second) -
                        offset_seconds;
  if (epoch <= 0 || epoch > UINT32_MAX) {
    return false;
  }
  *timestamp = static_cast<uint32_t>(epoch);
  return true;
}

HaHistoryParser::HaHistoryParser(const HaHistoryField* fields, size_t field_count,
                                 uint32_t local_midnight_ts,
                                 uint32_t next_local_midnight_ts,
                                 uint32_t cutoff_ts) {
  if (fields == nullptr || field_count == 0 || field_count > HA_HISTORY_MAX_FIELDS ||
      local_midnight_ts == 0 || cutoff_ts <= local_midnight_ts ||
      next_local_midnight_ts <= local_midnight_ts || cutoff_ts > next_local_midnight_ts) {
    fail(HaHistoryParseError::InvalidConfiguration);
    return;
  }
  field_count_ = field_count;
  for (size_t i = 0; i < field_count_; ++i) {
    if (fields[i].entity_id == nullptr || !ha_entity_id_valid(fields[i].entity_id) ||
        !relevant_entity(fields[i].entity)) {
      fail(HaHistoryParseError::InvalidConfiguration);
      return;
    }
    fields_[i] = fields[i];
    initial_[i] = UNKNOWN_EVENT;
  }

  day_from_minute_ = local_midnight_ts / 60;
  day_to_minute_ = (next_local_midnight_ts + 59) / 60;
  cutoff_minute_ = cutoff_ts / 60;
  first_minute_ = day_from_minute_;
  if (cutoff_minute_ - first_minute_ >= HISTORY_CAPACITY_MINUTES) {
    first_minute_ = cutoff_minute_ - (HISTORY_CAPACITY_MINUTES - 1);
  }
  const size_t sample_count = field_count_ * HISTORY_CAPACITY_MINUTES;
  samples_.reset(new (std::nothrow) int16_t[sample_count]);
  if (!samples_) {
    no_memory_ = true;
    fail(HaHistoryParseError::NoMemory);
    return;
  }
  for (size_t i = 0; i < sample_count; ++i) {
    samples_[i] = NO_EVENT;
  }
}

bool HaHistoryParser::ready() const {
  return !failed_ && samples_ != nullptr;
}

HaHistoryParseError HaHistoryParser::error() const {
  return error_;
}

size_t HaHistoryParser::error_series() const {
  return error_series_;
}

bool HaHistoryParser::fail(HaHistoryParseError error) {
  if (error_ == HaHistoryParseError::None) {
    error_ = error;
    error_series_ = series_index_;
  }
  failed_ = true;
  return false;
}

bool HaHistoryParser::feed(const uint8_t* data, size_t length) {
  if (!ready() || finished_ || (data == nullptr && length != 0)) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!consume(static_cast<char>(data[i]))) {
      return false;
    }
  }
  return true;
}

bool HaHistoryParser::consume(char c) {
  if (brace_depth_ != 0) {
    if (object_length_ + 1 >= sizeof(object_)) {
      return fail(HaHistoryParseError::ObjectTooLarge);
    }
    object_[object_length_++] = c;
    if (escaped_) {
      escaped_ = false;
      return true;
    }
    if (in_string_ && c == '\\') {
      escaped_ = true;
      return true;
    }
    if (c == '"') {
      in_string_ = !in_string_;
    } else if (!in_string_ && c == '{') {
      ++brace_depth_;
    } else if (!in_string_ && c == '}') {
      --brace_depth_;
      if (brace_depth_ == 0) {
        object_[object_length_] = '\0';
        const bool ok = finish_object();
        if (ok) {
          structure_ = StructureState::SeriesCommaOrEnd;
        }
        return ok;
      }
    }
    return true;
  }

  if (isspace(static_cast<unsigned char>(c))) {
    return true;
  }
  switch (structure_) {
    case StructureState::Start:
      if (c == '[') {
        structure_ = StructureState::RootValueOrEnd;
        return true;
      }
      return fail(HaHistoryParseError::UnexpectedTopLevelToken);
    case StructureState::RootValueOrEnd:
      if (c == '[') {
        ++series_index_;
        series_object_count_ = 0;
        structure_ = StructureState::SeriesValueOrEnd;
        return true;
      }
      if (c == ']') {
        structure_ = StructureState::End;
        return true;
      }
      return fail(HaHistoryParseError::UnexpectedTopLevelToken);
    case StructureState::RootCommaOrEnd:
      if (c == ',') {
        structure_ = StructureState::RootValue;
        return true;
      }
      if (c == ']') {
        structure_ = StructureState::End;
        return true;
      }
      return fail(HaHistoryParseError::UnexpectedDelimiter);
    case StructureState::RootValue:
      if (c == '[') {
        ++series_index_;
        series_object_count_ = 0;
        structure_ = StructureState::SeriesValueOrEnd;
        return true;
      }
      return fail(HaHistoryParseError::UnexpectedTopLevelToken);
    case StructureState::SeriesValueOrEnd:
      if (c == '{') {
        object_length_ = 0;
        object_[object_length_++] = c;
        brace_depth_ = 1;
        in_string_ = false;
        escaped_ = false;
        return true;
      }
      if (c == ']') {
        series_entity_[0] = '\0';
        structure_ = StructureState::RootCommaOrEnd;
        return true;
      }
      return fail(HaHistoryParseError::UnexpectedSeriesToken);
    case StructureState::SeriesCommaOrEnd:
      if (c == ',') {
        structure_ = StructureState::SeriesValue;
        return true;
      }
      if (c == ']') {
        series_entity_[0] = '\0';
        structure_ = StructureState::RootCommaOrEnd;
        return true;
      }
      return fail(HaHistoryParseError::UnexpectedDelimiter);
    case StructureState::SeriesValue:
      if (c == '{') {
        object_length_ = 0;
        object_[object_length_++] = c;
        brace_depth_ = 1;
        in_string_ = false;
        escaped_ = false;
        return true;
      }
      return fail(HaHistoryParseError::UnexpectedSeriesToken);
    case StructureState::End:
      return fail(HaHistoryParseError::TrailingData);
  }
  return fail(HaHistoryParseError::UnexpectedDelimiter);
}

bool HaHistoryParser::finish_object() {
  object_doc_.clear();
  if (deserializeJson(object_doc_, object_, object_length_) != DeserializationError::Ok ||
      !object_doc_.is<JsonObjectConst>()) {
    return fail(HaHistoryParseError::MalformedStateObject);
  }
  ++stats_.objects;
  JsonObjectConst root = object_doc_.as<JsonObjectConst>();
  const bool first_in_series = series_object_count_++ == 0;
  if (root["entity_id"].is<const char*>()) {
    const char* entity = root["entity_id"].as<const char*>();
    if (!ha_entity_id_valid(entity)) {
      return fail(HaHistoryParseError::InvalidEntityId);
    }
    strncpy(series_entity_, entity, sizeof(series_entity_) - 1);
    series_entity_[sizeof(series_entity_) - 1] = '\0';
  }
  if (first_in_series && series_entity_[0] == '\0') {
    // Under minimal_response, HA emits a full first object and compact later
    // objects containing only state and last_changed. The compact objects
    // inherit the identity established here; they must not establish a series.
    return fail(HaHistoryParseError::MissingEntityId);
  }

  const char* state = root["state"] | static_cast<const char*>(nullptr);
  const char* changed = root["last_changed"] | static_cast<const char*>(nullptr);
  if (changed == nullptr) {
    changed = root["last_updated"] | static_cast<const char*>(nullptr);
  }
  uint32_t timestamp = 0;
  if (state == nullptr) {
    return fail(HaHistoryParseError::MalformedStateObject);
  }
  if (!ha_history_timestamp_parse(changed, &timestamp)) {
    return fail(HaHistoryParseError::InvalidTimestamp);
  }
  if (timestamp / 60 >= cutoff_minute_) {
    return true;
  }

  for (size_t i = 0; i < field_count_; ++i) {
    if (strcmp(fields_[i].entity_id, series_entity_) != 0) {
      continue;
    }
    ++stats_.matching_states;
    MaybeFloat value;
    const HaValueStatus status = ha_normalise_state(
        state, fields_[i].unit, HA_ENTITIES[static_cast<size_t>(fields_[i].entity)].kind,
        &value);
    const int16_t event = status == HaValueStatus::Available
                              ? encode_event(value.value)
                              : UNKNOWN_EVENT;
    if (status == HaValueStatus::Available) {
      ++stats_.usable_states;
    }
    const uint32_t minute = timestamp / 60;
    if (minute < first_minute_) {
      initial_[i] = event;
    } else if (minute < cutoff_minute_) {
      samples_[i * HISTORY_CAPACITY_MINUTES + minute - first_minute_] = event;
    }
  }
  return true;
}

HaHistoryParseResult HaHistoryParser::finish(HaHistoryStats* stats) {
  if (finished_) {
    return HaHistoryParseResult::BadPayload;
  }
  finished_ = true;
  if (failed_ || structure_ != StructureState::End || brace_depth_ != 0 ||
      in_string_) {
    if (!failed_) {
      fail(HaHistoryParseError::IncompletePayload);
    }
    return no_memory_ ? HaHistoryParseResult::NoMemory : HaHistoryParseResult::BadPayload;
  }

  const uint32_t span = cutoff_minute_ - first_minute_;
  int16_t current[HA_HISTORY_MAX_FIELDS];
  int home = -1;
  int ev = -1;
  int pv = -1;
  int grid = -1;
  int battery = -1;
  for (size_t i = 0; i < field_count_; ++i) {
    current[i] = initial_[i];
    switch (fields_[i].entity) {
      case HaEntity::HomePower:
        home = static_cast<int>(i);
        break;
      case HaEntity::EvPower:
        ev = static_cast<int>(i);
        break;
      case HaEntity::PvPower:
        pv = static_cast<int>(i);
        break;
      case HaEntity::GridPower:
        grid = static_cast<int>(i);
        break;
      case HaEntity::BatteryPower:
        battery = static_cast<int>(i);
        break;
      default:
        break;
    }
  }
  for (uint32_t offset = 0; offset < span; ++offset) {
    for (size_t i = 0; i < field_count_; ++i) {
      const int16_t event = samples_[i * HISTORY_CAPACITY_MINUTES + offset];
      if (event != NO_EVENT) {
        current[i] = event;
      }
    }

    const uint32_t minute = first_minute_ + offset;
    for (size_t i = 0; i < field_count_; ++i) {
      if (fields_[i].entity == HaEntity::PvPower && current[i] > UNKNOWN_EVENT) {
        history_put(HistoryBank::Live, HistorySeries::Pv, minute, decode_event(current[i]));
        ++stats_.points_written;
      } else if (fields_[i].entity == HaEntity::BatterySoc &&
                 current[i] > UNKNOWN_EVENT) {
        history_put(HistoryBank::Live, HistorySeries::Soc, minute, decode_event(current[i]));
        ++stats_.points_written;
      }
    }

    MaybeFloat load;
    if (home >= 0 && current[home] > UNKNOWN_EVENT &&
        (ev < 0 || current[ev] > UNKNOWN_EVENT)) {
      load.known = true;
      load.value = decode_event(current[home]) +
                   (ev >= 0 ? decode_event(current[ev]) : 0.0f);
    } else if (home < 0 && pv >= 0 && grid >= 0 && battery >= 0 &&
               current[pv] > UNKNOWN_EVENT && current[grid] > UNKNOWN_EVENT &&
               current[battery] > UNKNOWN_EVENT) {
      load.known = true;
      load.value = decode_event(current[pv]) - decode_event(current[battery]) +
                   decode_event(current[grid]);
      if (load.value < 0.0f) {
        load.value = 0.0f;
      }
    }
    if (load.known) {
      history_put(HistoryBank::Live, HistorySeries::Load, minute, load.value);
      ++stats_.points_written;
    }
  }

  history_set_day_window(HistoryBank::Live, day_from_minute_, day_to_minute_);
  if (stats != nullptr) {
    *stats = stats_;
  }
  return stats_.points_written == 0 ? HaHistoryParseResult::NoData
                                    : HaHistoryParseResult::Applied;
}

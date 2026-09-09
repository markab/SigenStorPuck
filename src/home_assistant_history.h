#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include <ArduinoJson.h>

#include "history.h"
#include "home_assistant.h"

static constexpr size_t HA_HISTORY_MAX_FIELDS = 4;
// A no-attributes first/last state can still carry HA's small context object and
// three timestamps. This remains fixed while leaving room for a maximum entity
// ID and that standard envelope.
static constexpr size_t HA_HISTORY_OBJECT_MAX = 768;

struct HaHistoryField {
  HaEntity entity = HaEntity::PvPower;
  const char* entity_id = nullptr;
  HaUnit unit = HaUnit::Unknown;
};

struct HaHistoryStats {
  size_t objects = 0;
  size_t matching_states = 0;
  size_t usable_states = 0;
  size_t points_written = 0;
};

// Selects only entities needed by the three existing chart curves. With a
// direct home mapping, EV is included because HistorySeries::Load is house + EV.
// Without one, total load is derived from PV - battery + grid; EV cancels from
// that equation and is therefore not requested.
size_t ha_history_fields_build(const char* const entity_ids[HA_ENTITY_COUNT],
                               const HaUnit units[HA_ENTITY_COUNT],
                               HaHistoryField out[HA_HISTORY_MAX_FIELDS]);

enum class HaHistoryParseResult : uint8_t {
  Applied = 0,
  NoData,
  BadPayload,
  NoMemory,
};

enum class HaHistoryParseError : uint8_t {
  None = 0,
  InvalidConfiguration,
  NoMemory,
  UnexpectedTopLevelToken,
  UnexpectedSeriesToken,
  UnexpectedDelimiter,
  ObjectTooLarge,
  MalformedStateObject,
  MissingEntityId,
  InvalidEntityId,
  InvalidTimestamp,
  IncompletePayload,
  TrailingData,
};

const char* ha_history_parse_error_name(HaHistoryParseError error);

// Incrementally consumes Home Assistant's /api/history response. Only one small
// JSON object is buffered at a time; the fixed per-minute workspace is at most
// 4 * 1500 * int16_t (11.72 KiB) and is released after this one boot backfill.
// Nothing is written to HistoryBank::Live until finish() validates the complete
// response, so malformed payloads cannot partially corrupt a good live chart.
class HaHistoryParser {
 public:
  HaHistoryParser(const HaHistoryField* fields, size_t field_count,
                  uint32_t local_midnight_ts, uint32_t next_local_midnight_ts,
                  uint32_t cutoff_ts);

  bool ready() const;
  bool feed(const uint8_t* data, size_t length);
  HaHistoryParseResult finish(HaHistoryStats* stats = nullptr);
  HaHistoryParseError error() const;
  size_t error_series() const;

 private:
  bool consume(char c);
  bool finish_object();
  bool fail(HaHistoryParseError error);

  enum class StructureState : uint8_t {
    Start = 0,
    RootValueOrEnd,
    RootValue,
    RootCommaOrEnd,
    SeriesValueOrEnd,
    SeriesValue,
    SeriesCommaOrEnd,
    End,
  };

  HaHistoryField fields_[HA_HISTORY_MAX_FIELDS] = {};
  size_t field_count_ = 0;
  std::unique_ptr<int16_t[]> samples_;
  int16_t initial_[HA_HISTORY_MAX_FIELDS] = {};
  uint32_t day_from_minute_ = 0;
  uint32_t day_to_minute_ = 0;
  uint32_t first_minute_ = 0;
  uint32_t cutoff_minute_ = 0;

  char object_[HA_HISTORY_OBJECT_MAX] = {};
  size_t object_length_ = 0;
  JsonDocument object_doc_;
  char series_entity_[HA_ENTITY_ID_MAX + 1] = {};
  size_t series_index_ = 0;
  size_t series_object_count_ = 0;
  uint8_t brace_depth_ = 0;
  StructureState structure_ = StructureState::Start;
  HaHistoryParseError error_ = HaHistoryParseError::None;
  size_t error_series_ = 0;
  bool in_string_ = false;
  bool escaped_ = false;
  bool failed_ = false;
  bool no_memory_ = false;
  bool finished_ = false;
  HaHistoryStats stats_;
};

// ISO-8601 parser kept independent of the C library timezone. HA timestamps
// carry their own UTC offset, including on DST transition days.
bool ha_history_timestamp_parse(const char* text, uint32_t* timestamp);

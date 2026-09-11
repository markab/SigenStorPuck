#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device/fetch_result.h"

enum class HistoryBackfillOutcome : uint8_t {
  Success = 0,
  NoData,
  TransientFailure,
  PermanentFailure,
};

HistoryBackfillOutcome history_backfill_outcome(FetchResult result,
                                                 int http_status = 0);

static constexpr uint32_t HISTORY_BACKFILL_WINDOW_SECONDS = 2 * 60 * 60;

// A boot/source-activation backfill, not a recurring sync. One two-hour window
// is attempted between live polls. Empty windows advance normally; transport
// and server failures retry only the current window, at most three times.
class HistoryBackfillRetry {
 public:
  void activate(uint32_t day_start_ts, uint32_t next_day_start_ts,
                uint32_t cutoff_ts);
  bool due(uint32_t now_ms) const;
  void record(HistoryBackfillOutcome outcome, uint32_t now_ms,
              size_t points_written = 0);

  bool active() const;
  bool finished() const;
  bool succeeded() const;
  uint8_t attempts() const;
  uint16_t windows_completed() const;
  size_t points_written() const;
  uint32_t window_start_ts() const;
  uint32_t window_end_ts() const;

 private:
  static constexpr uint8_t MAX_ATTEMPTS = 3;
  static constexpr uint32_t RETRY_DELAY_MS = 30000;

  uint32_t window_start_ts_ = 0;
  uint32_t cutoff_ts_ = 0;
  uint32_t next_attempt_ms_ = 0;
  uint8_t attempts_ = 0;
  uint16_t windows_completed_ = 0;
  size_t points_written_ = 0;
  bool active_ = false;
  bool finished_ = false;
  bool succeeded_ = false;
};

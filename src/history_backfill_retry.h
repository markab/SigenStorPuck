#pragma once

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

// A boot/source-activation backfill, not a recurring sync. Empty Recorder data
// is final; transport and server failures get two delayed retries (three tries
// total) while live polling continues independently.
class HistoryBackfillRetry {
 public:
  void activate(uint32_t cutoff_ts);
  bool due(uint32_t now_ms) const;
  void record(HistoryBackfillOutcome outcome, uint32_t now_ms);

  bool active() const;
  bool finished() const;
  uint8_t attempts() const;
  uint32_t cutoff_ts() const;

 private:
  static constexpr uint8_t MAX_ATTEMPTS = 3;
  static constexpr uint32_t RETRY_DELAY_MS = 30000;

  uint32_t cutoff_ts_ = 0;
  uint32_t next_attempt_ms_ = 0;
  uint8_t attempts_ = 0;
  bool active_ = false;
  bool finished_ = false;
};

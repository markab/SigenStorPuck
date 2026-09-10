#include "history_backfill_retry.h"

HistoryBackfillOutcome history_backfill_outcome(FetchResult result, int http_status) {
  switch (result) {
    case FetchResult::Ok:
      return HistoryBackfillOutcome::Success;
    case FetchResult::EntityUnavailable:
      return HistoryBackfillOutcome::NoData;
    case FetchResult::NoNetwork:
    case FetchResult::ConnectFailed:
    case FetchResult::TlsFailed:
    case FetchResult::ClockUnset:
    case FetchResult::ReadTimeout:
      return HistoryBackfillOutcome::TransientFailure;
    case FetchResult::HttpError:
      return http_status == 429 || http_status >= 500
                 ? HistoryBackfillOutcome::TransientFailure
                 : HistoryBackfillOutcome::PermanentFailure;
    case FetchResult::NotConfigured:
    case FetchResult::Unauthorised:
    case FetchResult::BadPayload:
    case FetchResult::ProtocolError:
      return HistoryBackfillOutcome::PermanentFailure;
  }
  return HistoryBackfillOutcome::PermanentFailure;
}

void HistoryBackfillRetry::activate(uint32_t day_start_ts,
                                    uint32_t next_day_start_ts,
                                    uint32_t cutoff_ts) {
  if (active_ || finished_ || day_start_ts == 0 ||
      next_day_start_ts <= day_start_ts || cutoff_ts <= day_start_ts ||
      cutoff_ts > next_day_start_ts) {
    return;
  }
  window_start_ts_ = day_start_ts;
  cutoff_ts_ = cutoff_ts;
  active_ = true;
}

bool HistoryBackfillRetry::due(uint32_t now_ms) const {
  return active_ && !finished_ &&
         (attempts_ == 0 || static_cast<int32_t>(now_ms - next_attempt_ms_) >= 0);
}

void HistoryBackfillRetry::record(HistoryBackfillOutcome outcome, uint32_t now_ms,
                                  size_t points_written) {
  if (!active_ || finished_) {
    return;
  }
  ++attempts_;

  if (outcome == HistoryBackfillOutcome::Success ||
      outcome == HistoryBackfillOutcome::NoData) {
    points_written_ += points_written;
    ++windows_completed_;
    window_start_ts_ = window_end_ts();
    attempts_ = 0;
    next_attempt_ms_ = 0;
    if (window_start_ts_ >= cutoff_ts_) {
      active_ = false;
      finished_ = true;
      succeeded_ = true;
    }
    return;
  }

  if (outcome == HistoryBackfillOutcome::PermanentFailure ||
      attempts_ >= MAX_ATTEMPTS) {
    active_ = false;
    finished_ = true;
    return;
  }
  next_attempt_ms_ = now_ms + RETRY_DELAY_MS;
}

bool HistoryBackfillRetry::active() const {
  return active_;
}

bool HistoryBackfillRetry::finished() const {
  return finished_;
}

bool HistoryBackfillRetry::succeeded() const {
  return succeeded_;
}

uint8_t HistoryBackfillRetry::attempts() const {
  return attempts_;
}

uint16_t HistoryBackfillRetry::windows_completed() const {
  return windows_completed_;
}

size_t HistoryBackfillRetry::points_written() const {
  return points_written_;
}

uint32_t HistoryBackfillRetry::window_start_ts() const {
  return window_start_ts_;
}

uint32_t HistoryBackfillRetry::window_end_ts() const {
  if (!active_ || window_start_ts_ >= cutoff_ts_) {
    return cutoff_ts_;
  }
  const uint32_t remaining = cutoff_ts_ - window_start_ts_;
  return remaining > HISTORY_BACKFILL_WINDOW_SECONDS
             ? window_start_ts_ + HISTORY_BACKFILL_WINDOW_SECONDS
             : cutoff_ts_;
}

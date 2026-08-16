#include "poller.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "modbus_api.h"
#include "settings.h"
#include "sigen_api.h"
#include "solar_api.h"
#include "updater.h"
#include "ui/ui.h"

#include <time.h>

namespace {

// TLS lives on this stack: mbedTLS handshake buffers are large, and an
// under-sized stack here shows up as a crash inside the handshake rather than as
// anything that looks like a stack problem.
//
// One size for both sources, even though a Modbus fetch needs nothing like it.
// The update check runs on this task too (updater_service), and that is a TLS
// request whichever source the device polls with — so the 6 KB this used to drop
// to on the Modbus path would have overflowed inside the handshake the first
// time anyone opened the settings page.
constexpr uint32_t TASK_STACK_BYTES = 16384;

constexpr UBaseType_t TASK_PRIORITY = 3;
// Core 0. Arduino's loop(), and so LVGL, owns core 1.
constexpr BaseType_t TASK_CORE = 0;

// Backoff on repeated failure, so an unreachable server is retried at a sane rate
// instead of every few seconds forever.
constexpr uint32_t BACKOFF_CEILING_MS = 60000;

// How often to refetch today's curve from the server. A shape that has already
// happened does not change, and the newest few minutes are covered by the live
// poll writing into the ring meanwhile — so this only has to be often enough
// that a reboot's empty chart fills promptly.
constexpr uint32_t DAY_REFRESH_MS = 5 * 60 * 1000;

SemaphoreHandle_t s_lock = nullptr;
Snapshot s_snapshot;
PollStatus s_status;
int32_t s_tz_offset_min = 0;
volatile bool s_wake = false;

// Task-local in practice — only poll_task touches these — but kept here with the
// rest of the task's state rather than as statics buried in the loop.
uint32_t s_last_day_ms = 0;
bool s_day_complained = false;

// The past day currently loaded, and what the buttons are asking for. Kept apart
// so the fetch happens on the poll task rather than under the button handler:
// the button runs on the UI loop, and a TLS request there would stall drawing
// for the whole of a handshake.
int s_loaded_offset = 0;
uint32_t s_last_dated_ms = 0;
Snapshot s_day_snapshot;
bool s_day_valid = false;

// A past day does not change, so this is only a guard against the device sitting
// on one for hours with a stale copy — and against a fetch that failed.
constexpr uint32_t DATED_REFRESH_MS = 15 * 60 * 1000;

// "YYYY-MM-DD" for a whole number of days back from now, in local time. The
// server takes the date, not an offset, because a day is a local-calendar thing
// and only one of us knows the timezone for certain.
String date_for_offset(int days_back) {
  const time_t when = time(nullptr) + static_cast<time_t>(days_back) * 86400;
  struct tm parts = {};
  if (when <= 0 || localtime_r(&when, &parts) == nullptr) {
    return String();
  }
  char text[12];
  strftime(text, sizeof(text), "%Y-%m-%d", &parts);
  return String(text);
}

uint32_t backoff_for(uint32_t failures) {
  if (failures == 0) {
    return 0;
  }
  // 2s, 4s, 8s ... capped.
  uint32_t delay_ms = 2000;
  for (uint32_t i = 1; i < failures && delay_ms < BACKOFF_CEILING_MS; ++i) {
    delay_ms *= 2;
  }
  return delay_ms > BACKOFF_CEILING_MS ? BACKOFF_CEILING_MS : delay_ms;
}

void publish(const Snapshot& snapshot, const PollStatus& status) {
  if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    s_snapshot = snapshot;
    s_status = status;
    // Latched rather than left in the snapshot for readers to dig out: it is the
    // one field that is a property of the *site* rather than of this reading, and
    // the screen schedule needs it even while polls are failing. Keeping the last
    // known offset through a network outage is right — the timezone has not
    // changed just because the server stopped answering.
    if (snapshot.tz_offset_min.known) {
      s_tz_offset_min = snapshot.tz_offset_min.value;
    }
    xSemaphoreGive(s_lock);
  }
}

void publish_status(const PollStatus& status) {
  if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    s_status = status;
    xSemaphoreGive(s_lock);
  }
}

void poll_task(void* /*argument*/) {
  PollStatus status;

  // Read once, not per cycle: the source applies on the next boot (§D1), so it
  // cannot change under this loop, and re-reading it would only invite the two
  // paths to interleave.
  const bool modbus = settings_get().source == DataSource::Modbus;

  for (;;) {
    Snapshot fetched;
    int http_status = 0;
    const FetchResult result = modbus ? modbus_api_fetch(&fetched, &http_status)
                                      : sigen_api_fetch(&fetched, &http_status);

    status.last_result = result;
    status.last_http_status = http_status;

    if (result == FetchResult::Ok) {
      // The forecast the plant cannot supply, folded in before publishing so the
      // screens and history_record() see one complete snapshot rather than a
      // reading that grows a solar block a moment later. No I/O here — this is
      // the cached figures, refreshed further down between polls.
      if (modbus) {
        solar_api_apply(&fetched);
      }
      status.consecutive_failures = 0;
      status.last_ok_ms = millis();
      status.ever_succeeded = true;
      publish(fetched, status);
    } else {
      // Not configured is not a failure to back off from — there is simply
      // nothing to do until someone visits the settings page.
      if (result != FetchResult::NotConfigured) {
        ++status.consecutive_failures;
      }
      publish_status(status);
      // Logged sparsely: the first failure and then every tenth, so a server
      // that is down for an hour does not fill the log with one repeated line.
      // "Not configured" is skipped entirely — it is the normal state of a device
      // waiting to be set up, and it would otherwise print forever, because it
      // deliberately does not count as a failure and 0 % 10 is 0.
      const bool worth_logging = result != FetchResult::NotConfigured &&
                                 (status.consecutive_failures == 1 ||
                                  status.consecutive_failures % 10 == 0);
      if (worth_logging) {
        // Free heap alongside the reason, because the reason on its own is not
        // enough to act on. HTTPClient folds every transport failure into one
        // negative code, so sigen_api.cpp reports "TLS failed" for a genuine
        // certificate problem, for a server that is not answering, and for an
        // mbedTLS allocation that could not be satisfied — and those want three
        // different responses. The handshake needs tens of kilobytes at once, so
        // the largest free block separates the last case from the other two.
        Serial.printf("[poll] %s (http %d), failure %u, heap %u B free, largest block %u B\n",
                      fetch_result_name(result), http_status, status.consecutive_failures,
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
      }
    }

    // Today's curve from the server, on its own slow cadence. Same reasoning as
    // updater_service() below: between fetches, with this task's client closed,
    // so two TLS sessions never overlap.
    //
    // Deliberately not folded into the failure counting above — this is the
    // chart's backdrop, not the reading. A server too old to have the endpoint
    // 404s here forever, and that must not make a working device look broken.
    if (!modbus) {
      const uint32_t now = millis();
      const bool due = s_last_day_ms == 0 || now - s_last_day_ms >= DAY_REFRESH_MS;
      if (due && status.ever_succeeded) {
        s_last_day_ms = now;
        int day_status = 0;
        const FetchResult day = sigen_api_fetch_day(HistoryBank::Live, String(), &day_status);
        if (day != FetchResult::Ok && !s_day_complained) {
          // Once, not every time: an old server would otherwise print this every
          // five minutes for the life of the device.
          s_day_complained = true;
          Serial.printf("[poll] day series unavailable: %s (http %d) — charts will fill "
                        "from live polls only\n",
                        fetch_result_name(day), day_status);
        } else if (day == FetchResult::Ok) {
          s_day_complained = false;
        }
      }

      // The day the buttons have stepped back to. Fetched here rather than under
      // the button handler because that runs on the UI loop, where a TLS
      // handshake would stall drawing for its whole duration.
      const int wanted = ui_day_offset();
      const bool changed = wanted != s_loaded_offset;
      const bool stale = s_last_dated_ms == 0 || now - s_last_dated_ms >= DATED_REFRESH_MS;
      if (wanted == 0) {
        // Back to live. Nothing to fetch, and the day bank is left as it is —
        // stepping back to the same day again should not have to reload it.
        if (s_loaded_offset != 0 && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
          s_loaded_offset = 0;
          s_day_valid = false;
          xSemaphoreGive(s_lock);
        }
      } else if (status.ever_succeeded && (changed || stale)) {
        const String date = date_for_offset(wanted);
        if (!date.isEmpty()) {
          // The curve first: on a change the chart is the slowest thing to
          // arrive, and loading it before the figures avoids a frame showing the
          // new day's numbers over the old day's shape.
          int dated_status = 0;
          if (changed) {
            history_reset(HistoryBank::Day);
          }
          sigen_api_fetch_day(HistoryBank::Day, date, &dated_status);

          Snapshot dated;
          const FetchResult result = sigen_api_fetch_dated(date, &dated, &dated_status);
          if (result == FetchResult::Ok) {
            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
              s_day_snapshot = dated;
              s_day_valid = true;
              s_loaded_offset = wanted;
              xSemaphoreGive(s_lock);
            }
            s_last_dated_ms = now;
          } else {
            Serial.printf("[poll] %s for %s (http %d)\n", fetch_result_name(result),
                          date.c_str(), dated_status);
            // Retried on the next cycle rather than backed off: a day the user is
            // looking at is worth more than one that failed in the background.
            s_last_dated_ms = 0;
          }
        }
      }
    }

    // Between fetches, with this task's own client already closed, so an update
    // check never overlaps a poll's TLS session. That overlap is what exhausted
    // the heap and made the check report a refused connection.
    //
    // The forecast fetch keeps the same company for the same reason: it is
    // another TLS session on this stack, and it decides for itself whether one is
    // due — at most one an hour. Server source excluded because there the
    // forecast comes in the summary payload already.
    if (modbus) {
      solar_api_service();
    }
    updater_service();

    uint32_t wait_ms = settings_get().poll_interval_s * 1000;
    const uint32_t backoff_ms = backoff_for(status.consecutive_failures);
    if (backoff_ms > wait_ms) {
      wait_ms = backoff_ms;
    }
    if (result == FetchResult::NotConfigured) {
      wait_ms = 2000;
    }

    // Woken early when the settings change, so a corrected URL is tried at once
    // rather than after a full backoff.
    const uint32_t started = millis();
    while (millis() - started < wait_ms) {
      if (s_wake) {
        s_wake = false;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

}  // namespace

void poller_begin() {
  if (s_lock != nullptr) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  const bool modbus = settings_get().source == DataSource::Modbus;
  xTaskCreatePinnedToCore(poll_task, "puck_poll", TASK_STACK_BYTES, nullptr, TASK_PRIORITY,
                          nullptr, TASK_CORE);
  Serial.printf("[poll] task started on core %d, source %s, stack %u\n",
                static_cast<int>(TASK_CORE), modbus ? "modbus" : "server", TASK_STACK_BYTES);
}

// Both readers wait for the lock rather than giving up after a timeout.
//
// A timeout here cannot be handled honestly: there is nothing truthful to return
// when the answer is simply not available yet. The old 20 ms limit made
// poller_status() fall back to a default-constructed PollStatus, which reads as
// "not configured, 0 failures" — indistinguishable from a device nobody has set
// up, and observed on a working device. poller_snapshot() had the same flaw and
// would blank a good screen to "Waiting for data".
//
// Waiting is safe: the lock is only ever held for a struct copy, by code that
// does no I/O and cannot block, and publish() already takes it with
// portMAX_DELAY. There is no path that can hold it long enough to matter.
bool poller_snapshot(Snapshot* out) {
  if (s_lock == nullptr || out == nullptr) {
    return false;
  }
  bool have = false;
  if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    if (s_snapshot.valid) {
      *out = s_snapshot;
      have = true;
    }
    xSemaphoreGive(s_lock);
  }
  return have;
}

PollStatus poller_status() {
  PollStatus copy;
  if (s_lock != nullptr && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    copy = s_status;
    xSemaphoreGive(s_lock);
  }
  return copy;
}

bool poller_day_snapshot(Snapshot* out) {
  if (out == nullptr) {
    return false;
  }
  bool valid = false;
  if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    // Only when the loaded day is the one being asked for: between a button press
    // and its fetch landing, the previous day is still in there and showing it
    // under the new day's date would be worse than showing live.
    valid = s_day_valid && s_loaded_offset == ui_day_offset() && s_loaded_offset != 0;
    if (valid) {
      *out = s_day_snapshot;
    }
    xSemaphoreGive(s_lock);
  }
  return valid;
}

int32_t poller_tz_offset_min() {
  int32_t copy = 0;
  if (s_lock != nullptr && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    copy = s_tz_offset_min;
    xSemaphoreGive(s_lock);
  }
  return copy;
}

void poller_wake() {
  s_wake = true;
}

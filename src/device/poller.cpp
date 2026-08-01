#include "poller.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "modbus_api.h"
#include "settings.h"
#include "sigen_api.h"
#include "updater.h"

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
volatile bool s_wake = false;

// Task-local in practice — only poll_task touches these — but kept here with the
// rest of the task's state rather than as statics buried in the loop.
uint32_t s_last_day_ms = 0;
bool s_day_complained = false;

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
        const FetchResult day = sigen_api_fetch_day(&day_status);
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
    }

    // Between fetches, with this task's own client already closed, so an update
    // check never overlaps a poll's TLS session. That overlap is what exhausted
    // the heap and made the check report a refused connection.
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

void poller_wake() {
  s_wake = true;
}

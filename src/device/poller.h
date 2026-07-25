// The polling loop, on its own task.
//
// PLAN.md §B3: one network task pinned to core 0 while Arduino's loop() runs LVGL
// on core 1. A single mutex guards the shared snapshot and is held only for the
// swap — rendering happens outside the lock, so the UI never stalls on the
// network.

#pragma once

#include <Arduino.h>

#include "sigen_api.h"
#include "snapshot.h"

struct PollStatus {
  FetchResult last_result = FetchResult::NotConfigured;
  int last_http_status = 0;
  uint32_t consecutive_failures = 0;
  // millis() of the last successful fetch, or 0 if there has never been one.
  uint32_t last_ok_ms = 0;
  bool ever_succeeded = false;
};

// Starts the task. Safe to call before the device is provisioned: it will idle
// and pick the settings up as soon as they are saved.
void poller_begin();

// Copies the last good snapshot out under the lock. Returns false if there has
// never been a successful fetch, in which case `out` is untouched.
bool poller_snapshot(Snapshot* out);

PollStatus poller_status();

// Asks the loop to fetch immediately rather than waiting out its interval — used
// after the settings change, so a corrected URL takes effect at once.
void poller_wake();

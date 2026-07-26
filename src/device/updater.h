// Self-update from GitHub Releases (PLAN.md §C3).
//
// The web installer is for the first flash and for recovery. After that the device
// checks for a newer release itself, and the stock 16 MB partition table gives it
// two app slots, so a failed update rolls back to the previous one.

#pragma once

#include <Arduino.h>

enum class UpdateState {
  Idle,          // not checked yet, or checking is turned off
  Checking,
  UpToDate,
  Available,
  Downloading,
  Failed,
};

struct UpdateStatus {
  UpdateState state = UpdateState::Idle;
  String latest_version;  // as advertised by the release manifest
  String message;         // why it failed, when it did
};

void updater_begin();

// Starts a check in the background and returns at once.
//
// This is what the settings page calls when it is opened, and it must not block:
// that handler runs on the same thread as LVGL, and the check's HTTPS round trip
// would freeze the display for as long as it took — up to fifteen seconds
// against an unreachable host.
//
// De-duplicated and rate-limited, so repeatedly loading or saving the page does
// not become a burst of requests to GitHub. `force` skips the rate limit, for the
// explicit "Check now" button.
void updater_request_check(bool force);

// Fetches the release manifest and compares versions, synchronously. Blocks for
// as long as the network takes, so never call it from a render path — use
// updater_request_check() there.
void updater_check();

// Downloads firmware.bin into the spare OTA slot and reboots into it. Only does
// anything when a check has found a newer version.
void updater_apply();

UpdateStatus updater_status();

// Off by default, so the device makes no outbound call beyond your own server
// unless you ask it to (PLAN.md §C3).
bool updater_enabled();

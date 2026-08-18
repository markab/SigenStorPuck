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
//
// The check itself runs on the poll task, at its next turn — see updater_service().
void updater_request_check(bool force);

// Runs a requested check, if there is one. Called from the poll task between
// fetches, and from nowhere else.
//
// The check has to happen there rather than on a task of its own, because a
// second TLS session cannot be afforded. On the server source the poll task is
// already holding a WiFiClientSecure, and mbedTLS plus this certificate bundle
// needs tens of kilobytes of contiguous heap; opening another at the same time
// fails the handshake and reports itself as a refused connection. Observed on a
// real device with ~55 KB free — and once, the 16 KB task stack could not be
// allocated either.
//
// Running here means the two are never concurrent, costs no second stack, and
// still keeps the network off the LVGL thread. The price is latency: a check
// waits for the current poll to finish, so up to one poll interval.
void updater_service();

// Fetches the release manifest and compares versions, synchronously. Blocks for
// as long as the network takes, so never call it from a render path — use
// updater_request_check() there.
void updater_check();

// Asks for firmware.bin to be downloaded into the spare OTA slot and booted into.
// Only does anything when a check has found a newer version.
//
// Requested here, performed on the poll task, for the same reason as the check:
// the download is a TLS session, and a second one opened while the poll task
// holds its own exhausts the heap. Returns immediately, so the settings page can
// answer before the device starts rebooting under it.
void updater_request_apply();

UpdateStatus updater_status();

// Whether the settings page checks on open. Off by default; turning it on stops
// every outbound call beyond your own network (PLAN.md §C3).
bool updater_enabled();

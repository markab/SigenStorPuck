// WiFi provisioning, time and mDNS.
//
// PLAN.md §B2: a captive portal for WiFi only, then a proper settings page on the
// network. A 466 px round screen is a bad place to type a URL and a 43-character
// token, so the on-device UI never asks for them.

#pragma once

#include <Arduino.h>

enum class NetState {
  Starting,
  // Captive portal is up; the screen shows which AP to join.
  Portal,
  Connecting,
  Connected,
};

// Brings up WiFi. Non-blocking: if there are no stored credentials it raises the
// setup access point and returns immediately, so LVGL keeps animating rather than
// freezing behind a blocking portal.
void net_begin();

// Must be called from loop(): drives the captive portal and reconnection.
void net_loop();

NetState net_state();

// The access point to join during setup, e.g. "SigenStorPuck-Setup".
const char* net_setup_ap_name();

// Where to find the settings page once on the network: "sigenstorpuck.local" and
// the IP. Empty until connected.
String net_hostname();
String net_ip();

// True once the clock has been set from NTP. TLS certificate validation needs it,
// so it is surfaced rather than assumed.
bool net_time_synced();

// Clears stored WiFi credentials and reboots into the captive portal.
void net_forget_wifi();

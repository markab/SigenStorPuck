// Persistent settings, in NVS.
//
// The server URL and the kiosk token live here and nowhere else — never in the
// tree, never in a build flag (see CLAUDE.md).

#pragma once

#include <Arduino.h>

struct Settings {
  // "https://host" or "http://192.168.1.10:8000", no trailing slash.
  String base_url;
  // The server's read-only kiosk token. 365-day, revocable.
  String token;

  uint32_t poll_interval_s = 5;
  uint8_t brightness = 200;
  // 0 disables dimming.
  uint32_t dim_after_s = 0;
};

// Loads from NVS, falling back to defaults. Safe to call before WiFi is up.
void settings_begin();

const Settings& settings_get();

// Stores the server URL and token, both validated by the caller. Returns false
// if NVS rejected the write.
bool settings_set_server(const String& base_url, const String& token);

bool settings_set_display(uint8_t brightness, uint32_t dim_after_s);

bool settings_set_poll_interval(uint32_t seconds);

// True once there is both a base URL and a token — i.e. there is any point
// trying to poll.
bool settings_is_provisioned();

// The token with all but its last four characters replaced. Everything that
// displays or logs a token uses this: the settings page echoes it back so you
// can tell one device's enrolment from another, and that is not a good reason to
// put a working credential on a web page or in a serial log.
String settings_token_masked();

// Wipes the server URL and token. WiFi credentials are WiFiManager's and are not
// touched here.
void settings_forget_server();

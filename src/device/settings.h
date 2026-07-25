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
  // Dimmed rather than switched off: a status display you have to wake to read is
  // a worse status display. 0 disables dimming entirely.
  uint32_t dim_after_s = 30;
  uint8_t dim_brightness = 24;
  // Quarter turns clockwise, 0-3, for mounting the Puck whichever way suits.
  uint8_t orientation = 0;
  // Extra rotation in tenths of a degree, for a mount that is not square to a
  // quarter turn. Costs a resampling pass, so 0 is the fast path.
  int16_t fine_tenths = 0;
  // Auto-cycle through the screens; 0 = off. Spreads AMOLED wear across four
  // layouts instead of burning one in.
  uint32_t rotate_s = 0;
  // How often the sweep band runs, in minutes; 0 = off.
  uint32_t sweep_min = 240;
};

// Loads from NVS, falling back to defaults. Safe to call before WiFi is up.
void settings_begin();

const Settings& settings_get();

// Stores the server URL and token, both validated by the caller. Returns false
// if NVS rejected the write.
bool settings_set_server(const String& base_url, const String& token);

bool settings_set_display(uint8_t brightness, uint32_t dim_after_s,
                          uint8_t dim_brightness);

bool settings_set_poll_interval(uint32_t seconds);

// Takes effect on the next boot: the panel's rotation is set when it is brought up.
bool settings_set_orientation(uint8_t quarter_turns);

bool settings_set_fine_rotation(int16_t tenths_of_a_degree);

bool settings_set_screensaver(uint32_t rotate_s, uint32_t sweep_min);

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

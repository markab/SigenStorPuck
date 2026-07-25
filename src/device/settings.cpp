#include "settings.h"

#include <Preferences.h>

namespace {

// Named for this project, like everything else (see the provenance rule in
// CLAUDE.md).
constexpr const char* NAMESPACE = "sigenstorpuck";

constexpr const char* KEY_BASE_URL = "base_url";
constexpr const char* KEY_TOKEN = "token";
constexpr const char* KEY_POLL = "poll_s";
constexpr const char* KEY_BRIGHTNESS = "bright";
constexpr const char* KEY_DIM = "dim_s";
constexpr const char* KEY_DIM_BRIGHT = "dim_bright";

// The protocol enforces a 1 s floor and the server polls Modbus every 5 s, so
// anything faster only adds load without making data fresher (PLAN.md §A4).
constexpr uint32_t POLL_MIN_S = 2;
constexpr uint32_t POLL_MAX_S = 300;

Settings s_settings;

}  // namespace

void settings_begin() {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/true)) {
    // First boot: the namespace does not exist yet. Defaults stand.
    Serial.println("[settings] none stored, using defaults");
    return;
  }
  s_settings.base_url = prefs.getString(KEY_BASE_URL, "");
  s_settings.token = prefs.getString(KEY_TOKEN, "");
  s_settings.poll_interval_s = prefs.getUInt(KEY_POLL, s_settings.poll_interval_s);
  s_settings.brightness = prefs.getUChar(KEY_BRIGHTNESS, s_settings.brightness);
  s_settings.dim_after_s = prefs.getUInt(KEY_DIM, s_settings.dim_after_s);
  s_settings.dim_brightness = prefs.getUChar(KEY_DIM_BRIGHT, s_settings.dim_brightness);
  prefs.end();

  // The token is never logged, only its presence.
  Serial.printf("[settings] server=%s token=%s poll=%us\n",
                s_settings.base_url.isEmpty() ? "(unset)" : s_settings.base_url.c_str(),
                s_settings.token.isEmpty() ? "(unset)" : settings_token_masked().c_str(),
                s_settings.poll_interval_s);
}

const Settings& settings_get() {
  return s_settings;
}

bool settings_set_server(const String& base_url, const String& token) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  const bool ok = prefs.putString(KEY_BASE_URL, base_url) > 0 &&
                  (token.isEmpty() || prefs.putString(KEY_TOKEN, token) > 0);
  prefs.end();
  if (!ok) {
    return false;
  }
  s_settings.base_url = base_url;
  if (!token.isEmpty()) {
    s_settings.token = token;
  }
  Serial.printf("[settings] server set to %s (token %s)\n", base_url.c_str(),
                settings_token_masked().c_str());
  return true;
}

bool settings_set_display(uint8_t brightness, uint32_t dim_after_s, uint8_t dim_brightness) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  prefs.putUChar(KEY_BRIGHTNESS, brightness);
  prefs.putUInt(KEY_DIM, dim_after_s);
  prefs.putUChar(KEY_DIM_BRIGHT, dim_brightness);
  prefs.end();
  s_settings.brightness = brightness;
  s_settings.dim_after_s = dim_after_s;
  s_settings.dim_brightness = dim_brightness;
  return true;
}

bool settings_set_poll_interval(uint32_t seconds) {
  if (seconds < POLL_MIN_S) {
    seconds = POLL_MIN_S;
  } else if (seconds > POLL_MAX_S) {
    seconds = POLL_MAX_S;
  }
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  prefs.putUInt(KEY_POLL, seconds);
  prefs.end();
  s_settings.poll_interval_s = seconds;
  return true;
}

bool settings_is_provisioned() {
  return !s_settings.base_url.isEmpty() && !s_settings.token.isEmpty();
}

String settings_token_masked() {
  const size_t length = s_settings.token.length();
  if (length == 0) {
    return String("(unset)");
  }
  if (length <= 4) {
    return String("****");
  }
  return String("...") + s_settings.token.substring(length - 4);
}

void settings_forget_server() {
  Preferences prefs;
  if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    prefs.remove(KEY_BASE_URL);
    prefs.remove(KEY_TOKEN);
    prefs.end();
  }
  s_settings.base_url = "";
  s_settings.token = "";
  Serial.println("[settings] server details cleared");
}

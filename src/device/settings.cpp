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
constexpr const char* KEY_ORIENT = "orient";
constexpr const char* KEY_FINE = "fine_deg10";
constexpr const char* KEY_ROTATE = "rotate_s";
constexpr const char* KEY_SWEEP = "sweep_min";
constexpr const char* KEY_UPDATES = "updates";
constexpr const char* KEY_SOURCE = "source";
constexpr const char* KEY_HOSTNAME = "hostname";
constexpr const char* KEY_MB_HOST = "mb_host";
constexpr const char* KEY_MB_PORT = "mb_port";
constexpr const char* KEY_MB_PLANT = "mb_plant";
// The device list is a fixed-size blob rather than a key per field: four devices
// times three fields would be twelve NVS entries to keep in step, and the struct
// is plain data with no pointers in it.
constexpr const char* KEY_MB_DEVICES = "mb_devs";

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
  s_settings.orientation = prefs.getUChar(KEY_ORIENT, s_settings.orientation) & 0x03;
  s_settings.fine_tenths = prefs.getShort(KEY_FINE, s_settings.fine_tenths);
  s_settings.rotate_s = prefs.getUInt(KEY_ROTATE, s_settings.rotate_s);
  s_settings.sweep_min = prefs.getUInt(KEY_SWEEP, s_settings.sweep_min);
  s_settings.check_updates = prefs.getBool(KEY_UPDATES, s_settings.check_updates);

  s_settings.source = prefs.getUChar(KEY_SOURCE, 0) == 1 ? DataSource::Modbus : DataSource::Server;
  if (prefs.isKey(KEY_HOSTNAME)) {
    const String stored = prefs.getString(KEY_HOSTNAME, "");
    if (!stored.isEmpty()) {
      s_settings.hostname = stored;
    }
  }
  // Guarded by isKey rather than left to the default argument: Preferences logs a
  // missing key at ERROR level, and two red lines on the first boot of every
  // device that has never used Modbus look like a fault when they are not.
  if (prefs.isKey(KEY_MB_HOST)) {
    s_settings.modbus_host = prefs.getString(KEY_MB_HOST, "");
  }
  s_settings.modbus_port = prefs.getUShort(KEY_MB_PORT, s_settings.modbus_port);
  s_settings.modbus_plant_address = prefs.getUChar(KEY_MB_PLANT, s_settings.modbus_plant_address);
  if (prefs.isKey(KEY_MB_DEVICES)) {
    // A short blob leaves the defaults, which is an empty device list.
    prefs.getBytes(KEY_MB_DEVICES, s_settings.modbus_devices, sizeof(s_settings.modbus_devices));
  }
  prefs.end();

  // The token is never logged, only its presence.
  if (s_settings.source == DataSource::Modbus) {
    Serial.printf("[settings] source=modbus host=%s:%u plant=%u poll=%us\n",
                  s_settings.modbus_host.isEmpty() ? "(unset)" : s_settings.modbus_host.c_str(),
                  s_settings.modbus_port, s_settings.modbus_plant_address,
                  s_settings.poll_interval_s);
  } else {
    Serial.printf("[settings] source=server server=%s token=%s poll=%us\n",
                  s_settings.base_url.isEmpty() ? "(unset)" : s_settings.base_url.c_str(),
                  s_settings.token.isEmpty() ? "(unset)" : settings_token_masked().c_str(),
                  s_settings.poll_interval_s);
  }
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

bool settings_set_source(DataSource source) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  prefs.putUChar(KEY_SOURCE, static_cast<uint8_t>(source));
  prefs.end();
  s_settings.source = source;
  Serial.printf("[settings] source set to %s (applies on next boot)\n",
                source == DataSource::Modbus ? "modbus" : "server");
  return true;
}

String settings_clean_hostname(const String& raw) {
  String out;
  out.reserve(raw.length());
  for (size_t i = 0; i < raw.length() && out.length() < SETTINGS_MAX_HOSTNAME; ++i) {
    char c = raw[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (allowed) {
      out += c;
    } else if (!out.isEmpty() && out[out.length() - 1] != '-') {
      // Any other character becomes a single separator, so "Front Room  Puck"
      // does not come out as "front-room--puck".
      out += '-';
    }
  }
  while (!out.isEmpty() && out[out.length() - 1] == '-') {
    out.remove(out.length() - 1);
  }
  return out;
}

bool settings_set_hostname(const String& hostname) {
  if (hostname.isEmpty() || hostname.length() > SETTINGS_MAX_HOSTNAME) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  const bool ok = prefs.putString(KEY_HOSTNAME, hostname) > 0;
  prefs.end();
  if (!ok) {
    return false;
  }
  s_settings.hostname = hostname;
  Serial.printf("[settings] hostname set to %s (applies on next boot)\n", hostname.c_str());
  return true;
}

bool settings_set_modbus(const String& host, uint16_t port, uint8_t plant_address,
                         const ModbusDevice* devices, size_t count) {
  if (port == 0) {
    port = 502;
  }
  if (plant_address == 0) {
    plant_address = 247;
  }

  ModbusDevice staged[SETTINGS_MAX_MODBUS_DEVICES];
  if (devices != nullptr) {
    if (count > SETTINGS_MAX_MODBUS_DEVICES) {
      count = SETTINGS_MAX_MODBUS_DEVICES;
    }
    for (size_t i = 0; i < count; ++i) {
      // 247 is the plant and is addressed implicitly; a device claiming it would
      // make the plant read twice and the device never.
      if (devices[i].slave_id == 0 || devices[i].slave_id > 246) {
        continue;
      }
      staged[i] = devices[i];
    }
  }

  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  prefs.putString(KEY_MB_HOST, host);
  prefs.putUShort(KEY_MB_PORT, port);
  prefs.putUChar(KEY_MB_PLANT, plant_address);
  prefs.putBytes(KEY_MB_DEVICES, staged, sizeof(staged));
  prefs.end();

  s_settings.modbus_host = host;
  s_settings.modbus_port = port;
  s_settings.modbus_plant_address = plant_address;
  for (size_t i = 0; i < SETTINGS_MAX_MODBUS_DEVICES; ++i) {
    s_settings.modbus_devices[i] = staged[i];
  }
  Serial.printf("[settings] modbus set to %s:%u plant %u\n", host.c_str(), port, plant_address);
  return true;
}

bool settings_is_provisioned() {
  if (s_settings.source == DataSource::Modbus) {
    // No token and no device list needed: a plant with neither an inverter nor a
    // charger listed still fills the power and battery screens from the plant
    // address alone.
    return !s_settings.modbus_host.isEmpty();
  }
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

bool settings_set_orientation(uint8_t quarter_turns) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  prefs.putUChar(KEY_ORIENT, quarter_turns & 0x03);
  prefs.end();
  s_settings.orientation = quarter_turns & 0x03;
  return true;
}

bool settings_set_screensaver(uint32_t rotate_s, uint32_t sweep_min) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  prefs.putUInt(KEY_ROTATE, rotate_s);
  prefs.putUInt(KEY_SWEEP, sweep_min);
  prefs.end();
  s_settings.rotate_s = rotate_s;
  s_settings.sweep_min = sweep_min;
  return true;
}

bool settings_set_fine_rotation(int16_t tenths_of_a_degree) {
  // More than a few degrees is a quarter turn wanted instead, and the resampling
  // cost is not worth paying for it.
  if (tenths_of_a_degree < -300) {
    tenths_of_a_degree = -300;
  } else if (tenths_of_a_degree > 300) {
    tenths_of_a_degree = 300;
  }
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  prefs.putShort(KEY_FINE, tenths_of_a_degree);
  prefs.end();
  s_settings.fine_tenths = tenths_of_a_degree;
  return true;
}

bool settings_set_check_updates(bool enabled) {
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  prefs.putBool(KEY_UPDATES, enabled);
  prefs.end();
  s_settings.check_updates = enabled;
  return true;
}

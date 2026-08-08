// Persistent settings, in NVS.
//
// The server URL and the kiosk token live here and nowhere else — never in the
// tree, never in a build flag (see CLAUDE.md).

#pragma once

#include <Arduino.h>

// Where readings come from (docs/PLAN.md §D1). One firmware carries both paths;
// this picks which one the poll task uses, and it takes effect on the next boot
// like `orientation` does — changing it also changes how many screens exist.
enum class DataSource : uint8_t {
  Server = 0,  // SigenStor Display's /api/summary over HTTP(S)
  Modbus = 1,  // straight to the plant over Modbus TCP, LAN only
};

enum class ModbusDeviceType : uint8_t {
  Inverter = 0,
  AcCharger = 1,
};

// One device in the plant, as addressed in the Sigen app. The plant itself is
// implicit at address 247 and is not listed here.
struct ModbusDevice {
  uint8_t slave_id = 0;  // 1-246; 0 means the slot is unused
  ModbusDeviceType type = ModbusDeviceType::Inverter;
  // Only meaningful for an inverter: whether it has a DC (vehicle) charger, and
  // so whether its DC output counts towards EV power. Mirrors the `dc_charger`
  // flag in the server's own device config.
  bool dc_charger = false;
};

// Four covers any domestic plant with room to spare, and keeps the settings page
// a fixed shape rather than a list that grows.
static constexpr size_t SETTINGS_MAX_MODBUS_DEVICES = 4;

// Enough for a descriptive name without running past what mDNS is comfortable
// advertising, and short enough to read off a 466 px screen.
static constexpr size_t SETTINGS_MAX_HOSTNAME = 32;

struct Settings {
  DataSource source = DataSource::Server;

  // The device's name on the network: `<hostname>.local` and the WiFi DHCP name.
  //
  // Default unchanged from when it was hardcoded, so an existing device keeps
  // answering on sigenstorpuck.local and every bookmark and doc reference still
  // works. A second Puck on the same LAN is the reason this is editable at all —
  // two devices claiming one mDNS name is a coin toss over which you reach.
  String hostname = "sigenstorpuck";

  // "https://host" or "http://192.168.1.10:8000", no trailing slash.
  String base_url;
  // The server's read-only kiosk token. 365-day, revocable.
  String token;

  // The gateway or inverter exposing Modbus TCP. Enable access for this device's
  // IP in the Sigen app, which whitelists by address.
  String modbus_host;
  uint16_t modbus_port = 502;
  uint8_t modbus_plant_address = 247;
  ModbusDevice modbus_devices[SETTINGS_MAX_MODBUS_DEVICES];

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
  // Which screens are built, and which the auto-cycle steps through, one bit per
  // PuckScreen (see ui/ui.h). Two masks rather than one: a screen you want to be
  // able to swipe to is not necessarily one you want the device parking on for
  // minutes at a time.
  //
  // Both default to everything. The settings screen's visibility bit is ignored
  // — it is the only route back to this page from the device itself, so hiding
  // it would strand anyone who did not already know the address.
  uint8_t screens_visible = 0xFF;
  uint8_t screens_rotate = 0xFF;

  // Whether opening the settings page checks GitHub for a newer release.
  //
  // On by default. It was off, guarding a check that only ever happened when you
  // pressed a button — consent for something you were already asking for. Now the
  // page checks on its own, so the setting finally means something, and it is a
  // real switch you can turn back off rather than a one-way flag (PLAN.md §C3).
  //
  // Turning it off stops every outbound call beyond your own server. Installing
  // is a separate, manual act either way.
  bool check_updates = true;
};

// Loads from NVS, falling back to defaults. Safe to call before WiFi is up.
void settings_begin();

const Settings& settings_get();

// Stores the server URL and token, both validated by the caller. Returns false
// if NVS rejected the write.
bool settings_set_server(const String& base_url, const String& token);

// Which data source to use on the next boot.
bool settings_set_source(DataSource source);

// The device's network name. Takes effect on the next boot: mDNS and the DHCP
// hostname are both registered once, when the network comes up.
//
// Returns false if NVS rejected the write or the name was unusable. Use
// settings_clean_hostname() first if the text came from a human.
bool settings_set_hostname(const String& hostname);

// Reduces free text to something that can be a DNS label: lowercased, anything
// that is not a letter, digit or hyphen turned into a hyphen, runs collapsed,
// leading and trailing hyphens removed, truncated to SETTINGS_MAX_HOSTNAME.
//
// Returns an empty string when nothing usable is left, which the caller should
// treat as a rejection rather than silently storing.
String settings_clean_hostname(const String& raw);

// Stores the Modbus endpoint and device list. `count` slots are taken from
// `devices`; the rest are cleared, so removing a device is a matter of sending
// a shorter list.
bool settings_set_modbus(const String& host, uint16_t port, uint8_t plant_address,
                         const ModbusDevice* devices, size_t count);

bool settings_set_display(uint8_t brightness, uint32_t dim_after_s,
                          uint8_t dim_brightness);

bool settings_set_poll_interval(uint32_t seconds);

// Takes effect on the next boot: the panel's rotation is set when it is brought up.
bool settings_set_orientation(uint8_t quarter_turns);

bool settings_set_fine_rotation(int16_t tenths_of_a_degree);

bool settings_set_screensaver(uint32_t rotate_s, uint32_t sweep_min);

// Which screens exist and which auto-cycle, as PuckScreen bit masks. Takes
// effect on the next boot, like `source` and `orientation`: the tileview's tiles
// are built once.
bool settings_set_screens(uint8_t visible, uint8_t rotate);

bool settings_set_check_updates(bool enabled);

// True once there is enough stored to be worth polling: a base URL and a token
// on the server path, a host on the Modbus path.
bool settings_is_provisioned();

// The token with all but its last four characters replaced. Everything that
// displays or logs a token uses this: the settings page echoes it back so you
// can tell one device's enrolment from another, and that is not a good reason to
// put a working credential on a web page or in a serial log.
String settings_token_masked();

// Wipes the server URL and token. WiFi credentials are WiFiManager's and are not
// touched here.
void settings_forget_server();

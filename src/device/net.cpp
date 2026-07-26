#include "net.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_mac.h>
#include <time.h>

#include "settings.h"

namespace {

// Named for this project. Nothing here is carried over from any reference
// implementation (see the provenance rule in CLAUDE.md).
constexpr const char* SETUP_AP_PREFIX = "SigenStorPuck";

constexpr const char* NTP_PRIMARY = "pool.ntp.org";
constexpr const char* NTP_SECONDARY = "time.nist.gov";

WiFiManager s_manager;
NetState s_state = NetState::Starting;
bool s_time_requested = false;
bool s_mdns_up = false;

// "SigenStorPuck-A1B2C3". Held as a member rather than built on demand because
// WiFiManager keeps the pointer it is given.
String s_setup_ap;
// The name actually registered this boot. Kept separate from the setting so the
// screen never claims an address that is not live yet: changing the hostname
// takes effect on the next boot, and until then this is still the old one.
String s_active_hostname;

// A hostname field in the portal, so a second Puck can be named before it joins
// the network. Without it the two would both claim the same mDNS name at the
// same moment, and which one you reach is a coin toss.
WiFiManagerParameter s_hostname_param("hostname", "Device name", "", SETTINGS_MAX_HOSTNAME);

// The last three bytes of the MAC, which is what makes one Puck's setup network
// tellable from another's. The full MAC would be six pairs of hex to read off a
// screen and type into a phone; three is enough to disambiguate the handful of
// devices anyone will own.
//
// Read from eFuse rather than with WiFi.macAddress(). The latter needs the WiFi
// driver running and returns six zero bytes before then — setting WIFI_STA mode
// is not enough — which produced "SigenStorPuck-000000" on every device, the
// exact opposite of a unique name.
String device_suffix() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char text[7];
  snprintf(text, sizeof(text), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(text);
}

// Set when the portal changed the name, so the device can come back up under it
// rather than sitting on the old one until someone restarts it by hand. That
// wait is exactly the case this feature exists for: two Pucks being set up
// together would both answer to the default name in the meantime.
bool s_restart_for_hostname = false;

void save_params() {
  const String cleaned = settings_clean_hostname(String(s_hostname_param.getValue()));
  if (cleaned.isEmpty() || cleaned == s_active_hostname) {
    return;
  }
  if (settings_set_hostname(cleaned)) {
    s_restart_for_hostname = true;
  }
}

void on_connected() {
  Serial.printf("[net] connected, ip %s\n", WiFi.localIP().toString().c_str());

  if (!s_mdns_up && MDNS.begin(s_active_hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    s_mdns_up = true;
    Serial.printf("[net] settings page at http://%s.local/ or http://%s/\n",
                  s_active_hostname.c_str(), WiFi.localIP().toString().c_str());
  }

  if (!s_time_requested) {
    // UTC. The device shows relative times, not clock times, so no timezone is
    // needed — the clock exists to make TLS certificate validation possible.
    configTime(0, 0, NTP_PRIMARY, NTP_SECONDARY);
    s_time_requested = true;
    Serial.println("[net] NTP requested");
  }
}

}  // namespace

void net_begin() {
  WiFi.mode(WIFI_STA);

  // Read once and held: mDNS and the DHCP hostname are both registered a single
  // time, when the network comes up, so a change made later cannot apply until
  // the next boot however it is stored.
  s_active_hostname = settings_get().hostname;
  s_setup_ap = String(SETUP_AP_PREFIX) + "-" + device_suffix();

  WiFi.setHostname(s_active_hostname.c_str());
  // Keep the radio out of power save: at a 5 s poll the latency it adds is not
  // worth the milliamps on a mains-powered display.
  WiFi.setSleep(false);

  s_manager.setHostname(s_active_hostname.c_str());
  // The whole point: a blocking portal would freeze LVGL, and a frozen screen
  // during setup looks like a dead device.
  s_manager.setConfigPortalBlocking(false);
  // WiFi credentials and the device's own name. The server URL and token are
  // still entered later, on a real keyboard, via the settings page — they are
  // far too long to type into a captive portal (PLAN.md §B2).
  s_hostname_param.setValue(s_active_hostname.c_str(), SETTINGS_MAX_HOSTNAME);
  s_manager.addParameter(&s_hostname_param);
  s_manager.setSaveParamsCallback(save_params);
  s_manager.setTitle("SigenStorPuck");
  s_manager.setConfigPortalTimeout(0);

  // Logged on every boot, not only when the portal comes up. Otherwise the only
  // way to find out which setup network this particular device raises is to wipe
  // its WiFi credentials and watch it appear.
  Serial.printf("[net] hostname %s, setup AP \"%s\"\n", s_active_hostname.c_str(),
                s_setup_ap.c_str());

  if (s_manager.autoConnect(s_setup_ap.c_str())) {
    s_state = NetState::Connected;
    on_connected();
  } else {
    // Either the portal is up, or it is still trying stored credentials.
    s_state = s_manager.getConfigPortalActive() ? NetState::Portal : NetState::Connecting;
    Serial.printf("[net] no connection yet; join \"%s\" to set WiFi\n", s_setup_ap.c_str());
  }
}

void net_loop() {
  s_manager.process();

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && s_state != NetState::Connected) {
    // Only ever after the portal renamed us, never after an ordinary reconnect —
    // a device that reboots itself because the WiFi blipped would be a much worse
    // bug than a hostname that needed a manual restart.
    if (s_restart_for_hostname) {
      Serial.println("[net] renamed during setup, restarting to register it");
      delay(300);
      ESP.restart();
    }
    s_state = NetState::Connected;
    on_connected();
  } else if (!connected && s_state == NetState::Connected) {
    Serial.println("[net] connection lost");
    s_state = NetState::Connecting;
  } else if (!connected && s_state != NetState::Portal) {
    s_state = s_manager.getConfigPortalActive() ? NetState::Portal : NetState::Connecting;
  }
}

NetState net_state() {
  return s_state;
}

const char* net_setup_ap_name() {
  return s_setup_ap.c_str();
}

String net_hostname() {
  return s_mdns_up ? s_active_hostname + ".local" : String();
}

String net_ip() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
}

bool net_time_synced() {
  // 1 Jan 2024: anything earlier means NTP has not answered yet.
  return time(nullptr) > 1704067200;
}

void net_forget_wifi() {
  Serial.println("[net] forgetting WiFi, restarting");
  s_manager.resetSettings();
  delay(300);
  ESP.restart();
}

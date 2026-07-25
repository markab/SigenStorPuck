#include "net.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

namespace {

// Named for this project. Nothing here is carried over from any reference
// implementation (see the provenance rule in CLAUDE.md).
constexpr const char* SETUP_AP = "SigenStorPuck-Setup";
constexpr const char* MDNS_NAME = "sigenstorpuck";

constexpr const char* NTP_PRIMARY = "pool.ntp.org";
constexpr const char* NTP_SECONDARY = "time.nist.gov";

WiFiManager s_manager;
NetState s_state = NetState::Starting;
bool s_time_requested = false;
bool s_mdns_up = false;

void on_connected() {
  Serial.printf("[net] connected, ip %s\n", WiFi.localIP().toString().c_str());

  if (!s_mdns_up && MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    s_mdns_up = true;
    Serial.printf("[net] settings page at http://%s.local/\n", MDNS_NAME);
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
  WiFi.setHostname(MDNS_NAME);
  // Keep the radio out of power save: at a 5 s poll the latency it adds is not
  // worth the milliamps on a mains-powered display.
  WiFi.setSleep(false);

  s_manager.setHostname(MDNS_NAME);
  // The whole point: a blocking portal would freeze LVGL, and a frozen screen
  // during setup looks like a dead device.
  s_manager.setConfigPortalBlocking(false);
  // WiFi credentials only. The server URL and token are entered later, on a real
  // keyboard, via the settings page.
  s_manager.setTitle("SigenStorPuck");
  s_manager.setConfigPortalTimeout(0);

  if (s_manager.autoConnect(SETUP_AP)) {
    s_state = NetState::Connected;
    on_connected();
  } else {
    // Either the portal is up, or it is still trying stored credentials.
    s_state = s_manager.getConfigPortalActive() ? NetState::Portal : NetState::Connecting;
    Serial.printf("[net] no connection yet; join \"%s\" to set WiFi\n", SETUP_AP);
  }
}

void net_loop() {
  s_manager.process();

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && s_state != NetState::Connected) {
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
  return SETUP_AP;
}

String net_hostname() {
  return s_mdns_up ? String(MDNS_NAME) + ".local" : String();
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

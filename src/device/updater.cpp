#include "updater.h"

#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include "board_config.h"
#include "settings.h"

namespace {

// "latest" rather than a pinned tag, so a new release is picked up with no change
// here. GitHub redirects these to objects.githubusercontent.com, which is why
// redirects have to be followed.
constexpr const char* MANIFEST_URL =
    "https://github.com/markab/SigenStorPuck/releases/latest/download/manifest.json";
constexpr const char* FIRMWARE_URL =
    "https://github.com/markab/SigenStorPuck/releases/latest/download/firmware.bin";

UpdateStatus s_status;

extern "C" const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

void attach_bundle(WiFiClientSecure& client) {
  // The same validated-TLS rule as the API client: the cert bundle covers GitHub's
  // CDN as well as your own server, which is the reason for using a bundle rather
  // than pinning a root.
  client.setCACertBundle(rootca_crt_bundle_start,
                         static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
}

// Compares dotted versions numerically. A string compare would call "0.10.0" older
// than "0.9.0", which is exactly the point at which it would matter.
bool is_newer(const String& candidate, const String& current) {
  int ci = 0;
  int ni = 0;
  for (int part = 0; part < 3; ++part) {
    int c = 0;
    int n = 0;
    while (ci < static_cast<int>(candidate.length()) && isdigit(candidate[ci])) {
      c = c * 10 + (candidate[ci++] - '0');
    }
    while (ni < static_cast<int>(current.length()) && isdigit(current[ni])) {
      n = n * 10 + (current[ni++] - '0');
    }
    if (c != n) {
      return c > n;
    }
    if (ci < static_cast<int>(candidate.length())) ++ci;  // skip the dot
    if (ni < static_cast<int>(current.length())) ++ni;
  }
  return false;
}

}  // namespace

void updater_begin() {
  s_status.state = UpdateState::Idle;
}

bool updater_enabled() {
  return settings_get().check_updates;
}

void updater_check() {
  if (!updater_enabled()) {
    s_status.state = UpdateState::Idle;
    s_status.message = "update checks are turned off";
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    s_status.state = UpdateState::Failed;
    s_status.message = "no network";
    return;
  }

  s_status.state = UpdateState::Checking;

  WiFiClientSecure client;
  attach_bundle(client);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, MANIFEST_URL)) {
    s_status.state = UpdateState::Failed;
    s_status.message = "could not reach GitHub";
    return;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    s_status.state = UpdateState::Failed;
    s_status.message = String("manifest HTTP ") + code;
    return;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    s_status.state = UpdateState::Failed;
    s_status.message = "manifest did not parse";
    return;
  }

  s_status.latest_version = doc["version"] | "";
  if (s_status.latest_version.isEmpty()) {
    s_status.state = UpdateState::Failed;
    s_status.message = "manifest has no version";
    return;
  }

  const bool newer = is_newer(s_status.latest_version, String(PUCK_FW_VERSION));
  s_status.state = newer ? UpdateState::Available : UpdateState::UpToDate;
  s_status.message = newer ? "a newer release is available" : "running the latest release";
  Serial.printf("[update] latest %s, running %s: %s\n", s_status.latest_version.c_str(),
                PUCK_FW_VERSION, s_status.message.c_str());
}

void updater_apply() {
  if (s_status.state != UpdateState::Available) {
    return;
  }
  Serial.printf("[update] downloading %s\n", s_status.latest_version.c_str());
  s_status.state = UpdateState::Downloading;

  WiFiClientSecure client;
  attach_bundle(client);

  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // Writes into whichever OTA slot is not running. If the new image will not boot,
  // the bootloader falls back to this one — which is the reason for the stock
  // two-slot 16 MB partition table.
  const t_httpUpdate_return result = httpUpdate.update(client, FIRMWARE_URL);
  switch (result) {
    case HTTP_UPDATE_OK:
      // Not reached: rebootOnUpdate restarts us.
      s_status.state = UpdateState::UpToDate;
      break;
    case HTTP_UPDATE_NO_UPDATES:
      s_status.state = UpdateState::UpToDate;
      s_status.message = "server had no update to give";
      break;
    case HTTP_UPDATE_FAILED:
    default:
      s_status.state = UpdateState::Failed;
      s_status.message = httpUpdate.getLastErrorString();
      Serial.printf("[update] failed: %s\n", s_status.message.c_str());
      break;
  }
}

UpdateStatus updater_status() {
  return s_status;
}

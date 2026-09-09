#include "home_assistant_api.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <memory>
#include <new>
#include <time.h>

#include "board_config.h"
#include "settings.h"

namespace {

constexpr const char* TEMPLATE_PATH = "/api/template";
constexpr const char* USER_AGENT = "SigenStorPuck/" PUCK_FW_VERSION " (ESP32-S3)";
constexpr uint16_t CONNECT_TIMEOUT_MS = 4000;
constexpr uint16_t TOTAL_TIMEOUT_MS = 8000;

extern "C" const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

bool clock_is_plausible() {
  return time(nullptr) > 1704067200;
}

}  // namespace

FetchResult home_assistant_api_fetch(Snapshot* out, int* status_code,
                                     HaParseInfo* parse_info) {
  if (status_code != nullptr) {
    *status_code = 0;
  }
  if (parse_info != nullptr) {
    *parse_info = HaParseInfo{};
  }
  if (out == nullptr || !settings_home_assistant_is_configured()) {
    return FetchResult::NotConfigured;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return FetchResult::NoNetwork;
  }

  const Settings& settings = settings_get();
  const String base_url = settings.ha_base_url;
  const String token = settings.ha_token;
  String entities[HA_ENTITY_COUNT];
  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    entities[i] = settings.ha_entities[i];
  }

  const bool secure = base_url.startsWith("https://");
  if (secure && !clock_is_plausible()) {
    return FetchResult::ClockUnset;
  }

  String request_body;
  {
    const char* entity_ids[HA_ENTITY_COUNT];
    for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
      entity_ids[i] = entities[i].c_str();
    }
    std::unique_ptr<char[]> rendered_template(new (std::nothrow) char[HA_TEMPLATE_MAX]);
    if (!rendered_template ||
        !ha_template_build(entity_ids, rendered_template.get(), HA_TEMPLATE_MAX)) {
      return FetchResult::BadPayload;
    }

    JsonDocument request_doc;
    request_doc["template"] = rendered_template.get();
    request_body.reserve(strlen(rendered_template.get()) + 32);
    serializeJson(request_doc, request_body);
  }  // release the template and JSON tree before allocating TLS buffers

  WiFiClient plain;
  WiFiClientSecure tls;
  WiFiClient* client = nullptr;
  if (secure) {
    // Home Assistant bearer tokens deserve the same certificate validation as
    // server kiosk tokens. Never replace this with setInsecure().
    tls.setCACertBundle(rootca_crt_bundle_start,
                        static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
    client = &tls;
  } else {
    client = &plain;
  }

  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(TOTAL_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  http.setReuse(false);
  // Do not forward a bearer token across redirects. A moved endpoint is an HTTP
  // error until its final base URL is configured explicitly.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(*client, base_url + TEMPLATE_PATH)) {
    return secure ? FetchResult::TlsFailed : FetchResult::ConnectFailed;
  }

  // Neither this header nor the body is logged. The body contains no token, but
  // it does contain the configured entity IDs and is still private configuration.
  http.addHeader("Authorization", String("Bearer ") + token);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  const int status = http.POST(request_body);
  if (status_code != nullptr) {
    *status_code = status;
  }
  if (status <= 0) {
    http.end();
    return secure ? FetchResult::TlsFailed : FetchResult::ConnectFailed;
  }
  switch (ha_http_status(status)) {
    case HaHttpStatus::Unauthorised:
      http.end();
      return FetchResult::Unauthorised;
    case HaHttpStatus::HttpError:
      http.end();
      return FetchResult::HttpError;
    case HaHttpStatus::Ok:
      break;
  }

  const String body = http.getString();
  http.end();
  Snapshot parsed;
  HaParseInfo info;
  if (!ha_payload_parse(body.c_str(), body.length(), &parsed, &info)) {
    return FetchResult::BadPayload;
  }
  if (parse_info != nullptr) {
    *parse_info = info;
  }
  if (info.configured == 0) {
    return FetchResult::NotConfigured;
  }
  if (info.available == 0) {
    return FetchResult::EntityUnavailable;
  }
  *out = parsed;
  return FetchResult::Ok;
}

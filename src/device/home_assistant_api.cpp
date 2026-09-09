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
#include "home_assistant_history.h"
#include "settings.h"
#include "solar_source.h"

namespace {

constexpr const char* TEMPLATE_PATH = "/api/template";
constexpr const char* USER_AGENT = "SigenStorPuck/" PUCK_FW_VERSION " (ESP32-S3)";
constexpr uint16_t CONNECT_TIMEOUT_MS = 4000;
constexpr uint16_t TOTAL_TIMEOUT_MS = 8000;
constexpr uint16_t HISTORY_IDLE_TIMEOUT_MS = 12000;
constexpr size_t HISTORY_CHUNK_BYTES = 512;
// The parser itself has fixed memory. This cap also prevents a broken or hostile
// endpoint from holding the polling task while sending an endless response.
constexpr size_t HISTORY_RESPONSE_MAX_BYTES = 1024 * 1024;

HaParseInfo s_live_info;
bool s_live_info_valid = false;

extern "C" const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

bool clock_is_plausible() {
  return time(nullptr) > 1704067200;
}

FetchResult status_result(int status) {
  switch (ha_http_status(status)) {
    case HaHttpStatus::Ok:
      return FetchResult::Ok;
    case HaHttpStatus::Unauthorised:
      return FetchResult::Unauthorised;
    case HaHttpStatus::HttpError:
      return FetchResult::HttpError;
  }
  return FetchResult::HttpError;
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
  const bool include_forecast = solar_forecast_uses_home_assistant(
      DataSource::HomeAssistant, settings.ha_solar_forecast_source);
  String entities[HA_ENTITY_COUNT];
  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    // Forecast mappings share this one compact template response, but are not
    // queried when Disabled or the Puck cache is selected.
    if (i >= HA_FORECAST_ENTITY_FIRST && !include_forecast) {
      entities[i] = "";
    } else {
      entities[i] = settings.ha_entities[i];
    }
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
  const FetchResult http_result = status_result(status);
  if (http_result != FetchResult::Ok) {
    http.end();
    return http_result;
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
  s_live_info = info;
  s_live_info_valid = info.local_midnight_ts != 0 &&
                      info.next_local_midnight_ts > info.local_midnight_ts;
  *out = parsed;
  return FetchResult::Ok;
}

FetchResult home_assistant_api_fetch_history(uint32_t cutoff_ts, int* status_code,
                                             size_t* points_written) {
  if (status_code != nullptr) {
    *status_code = 0;
  }
  if (points_written != nullptr) {
    *points_written = 0;
  }
  if (!settings_home_assistant_is_configured() || !s_live_info_valid || cutoff_ts == 0) {
    return FetchResult::NotConfigured;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return FetchResult::NoNetwork;
  }

  const Settings& settings = settings_get();
  const bool secure = settings.ha_base_url.startsWith("https://");
  if (secure && !clock_is_plausible()) {
    return FetchResult::ClockUnset;
  }

  const char* entity_ids[HA_ENTITY_COUNT];
  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    entity_ids[i] = settings.ha_entities[i].c_str();
  }
  HaHistoryField fields[HA_HISTORY_MAX_FIELDS];
  const size_t field_count =
      ha_history_fields_build(entity_ids, s_live_info.units, fields);
  if (field_count == 0) {
    return FetchResult::EntityUnavailable;
  }

  time_t midnight = static_cast<time_t>(s_live_info.local_midnight_ts);
  struct tm utc = {};
  if (gmtime_r(&midnight, &utc) == nullptr) {
    return FetchResult::BadPayload;
  }
  char timestamp[24];
  if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
    return FetchResult::BadPayload;
  }
  time_t cutoff = static_cast<time_t>(cutoff_ts);
  if (gmtime_r(&cutoff, &utc) == nullptr) {
    return FetchResult::BadPayload;
  }
  char cutoff_timestamp[24];
  if (strftime(cutoff_timestamp, sizeof(cutoff_timestamp),
               "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
    return FetchResult::BadPayload;
  }

  String filter;
  for (size_t i = 0; i < field_count; ++i) {
    bool duplicate = false;
    for (size_t previous = 0; previous < i; ++previous) {
      duplicate = duplicate || strcmp(fields[previous].entity_id, fields[i].entity_id) == 0;
    }
    if (duplicate) {
      continue;
    }
    if (!filter.isEmpty()) {
      filter += ',';
    }
    filter += fields[i].entity_id;
  }
  const String url = settings.ha_base_url + "/api/history/period/" + timestamp +
                     "?end_time=" + cutoff_timestamp + "&filter_entity_id=" + filter +
                     "&minimal_response&no_attributes";

  WiFiClient plain;
  WiFiClientSecure tls;
  WiFiClient* client = nullptr;
  if (secure) {
    // Same validation as the live template call; Recorder reuses the bearer
    // token but never weakens transport security to obtain optional history.
    tls.setCACertBundle(rootca_crt_bundle_start,
                        static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
    client = &tls;
  } else {
    client = &plain;
  }

  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(HISTORY_IDLE_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  // Reading directly from the network stream avoids a second full JSON buffer.
  // HTTP/1.0 asks HA to delimit the body by length/connection rather than chunk
  // framing, leaving the incremental parser to see JSON bytes only.
  http.useHTTP10(true);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(*client, url)) {
    return secure ? FetchResult::TlsFailed : FetchResult::ConnectFailed;
  }
  // Never log this header, URL, or entity filter: the token and mappings are
  // both private device configuration.
  http.addHeader("Authorization", String("Bearer ") + settings.ha_token);
  http.addHeader("Accept", "application/json");
  const int status = http.GET();
  if (status_code != nullptr) {
    *status_code = status;
  }
  if (status <= 0) {
    http.end();
    return secure ? FetchResult::TlsFailed : FetchResult::ConnectFailed;
  }
  const FetchResult http_result = status_result(status);
  if (http_result != FetchResult::Ok) {
    http.end();
    return http_result;
  }

  const int content_length = http.getSize();
  if (content_length > static_cast<int>(HISTORY_RESPONSE_MAX_BYTES)) {
    http.end();
    return FetchResult::BadPayload;
  }
  // Allocate the 11.72 KiB minute workspace only after the TLS handshake and
  // response headers have succeeded. A failed connection should not surrender
  // heap to optional history before mbedTLS gets what it needs.
  HaHistoryParser parser(fields, field_count, s_live_info.local_midnight_ts,
                         s_live_info.next_local_midnight_ts, cutoff_ts);
  if (!parser.ready()) {
    http.end();
    return FetchResult::BadPayload;
  }
  NetworkClient* stream = http.getStreamPtr();
  uint8_t chunk[HISTORY_CHUNK_BYTES];
  size_t received = 0;
  int remaining = content_length;
  uint32_t last_progress = millis();
  bool stream_ok = true;
  bool parse_ok = true;
  while (http.connected() && remaining != 0) {
    const size_t available = stream->available();
    if (available == 0) {
      if (millis() - last_progress >= HISTORY_IDLE_TIMEOUT_MS) {
        stream_ok = false;
        break;
      }
      delay(1);
      continue;
    }
    size_t wanted = available < sizeof(chunk) ? available : sizeof(chunk);
    if (remaining > 0 && wanted > static_cast<size_t>(remaining)) {
      wanted = static_cast<size_t>(remaining);
    }
    const int got = stream->read(chunk, wanted);
    if (got <= 0) {
      stream_ok = false;
      break;
    }
    received += static_cast<size_t>(got);
    if (received > HISTORY_RESPONSE_MAX_BYTES ||
        !parser.feed(chunk, static_cast<size_t>(got))) {
      parse_ok = false;
      break;
    }
    if (remaining > 0) {
      remaining -= got;
    }
    last_progress = millis();
  }
  http.end();
  if (!parse_ok) {
    return FetchResult::BadPayload;
  }
  if (!stream_ok || remaining > 0) {
    return FetchResult::ReadTimeout;
  }

  HaHistoryStats stats;
  const HaHistoryParseResult parsed = parser.finish(&stats);
  if (points_written != nullptr) {
    *points_written = stats.points_written;
  }
  switch (parsed) {
    case HaHistoryParseResult::Applied:
      return FetchResult::Ok;
    case HaHistoryParseResult::NoData:
      return FetchResult::EntityUnavailable;
    case HaHistoryParseResult::BadPayload:
    case HaHistoryParseResult::NoMemory:
      return FetchResult::BadPayload;
  }
  return FetchResult::BadPayload;
}

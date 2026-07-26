#include "sigen_api.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "board_config.h"
#include "settings.h"

namespace {

constexpr const char* SUMMARY_PATH = "/api/summary";

// Identifies the device in the server's logs. Version included so an old device
// misbehaving in the field can be recognised from the access log alone.
constexpr const char* USER_AGENT = "SigenStorPuck/" PUCK_FW_VERSION " (ESP32-S3)";

// Separate connect and total budgets: a host that accepts the connection and then
// stalls should not hold the poll loop for the whole read timeout.
constexpr uint16_t CONNECT_TIMEOUT_MS = 4000;
constexpr uint16_t TOTAL_TIMEOUT_MS = 8000;

// The ESP-IDF root certificate bundle that Arduino-ESP32 embeds. One mechanism
// validates both the VPS and, later, GitHub's release CDN, and it survives a CA
// rotation — better than pinning an individual root such as ISRG Root X1.
// Arduino-ESP32 3.x wants the bundle and its length, so both ends are needed.
extern "C" const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

// TLS certificate validity cannot be checked without a real clock. A device that
// has never reached NTP sits somewhere in 1970, and every certificate looks
// not-yet-valid — which surfaces as a handshake failure and sends you hunting for
// a certificate problem that does not exist.
bool clock_is_plausible() {
  time_t now = time(nullptr);
  // 1 Jan 2024. Anything earlier means NTP has not landed.
  return now > 1704067200;
}

}  // namespace

const char* fetch_result_name(FetchResult result) {
  switch (result) {
    case FetchResult::Ok:
      return "ok";
    case FetchResult::NotConfigured:
      return "not configured";
    case FetchResult::NoNetwork:
      return "no network";
    case FetchResult::ConnectFailed:
      return "connect failed";
    case FetchResult::TlsFailed:
      return "TLS failed";
    case FetchResult::ClockUnset:
      return "clock unset";
    case FetchResult::Unauthorised:
      return "unauthorised";
    case FetchResult::HttpError:
      return "http error";
    case FetchResult::BadPayload:
      return "bad payload";
    case FetchResult::ProtocolError:
      return "modbus error";
    case FetchResult::ReadTimeout:
      return "read timeout";
  }
  return "unknown";
}

FetchResult sigen_api_fetch(Snapshot* out, int* status_code) {
  if (status_code != nullptr) {
    *status_code = 0;
  }
  if (!settings_is_provisioned()) {
    return FetchResult::NotConfigured;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return FetchResult::NoNetwork;
  }

  const Settings& settings = settings_get();
  const bool secure = settings.base_url.startsWith("https://");

  if (secure && !clock_is_plausible()) {
    return FetchResult::ClockUnset;
  }

  // WiFiClientSecure is large; both live on the stack of the calling task, which
  // is why the poll task is given a generous one.
  WiFiClient plain;
  WiFiClientSecure tls;
  WiFiClient* client = nullptr;
  if (secure) {
    // Validate, never setInsecure(): this request carries a 365-day kiosk token
    // over the public internet, and an unvalidated connection hands that token to
    // anyone who can spoof DNS or sit on the route.
    tls.setCACertBundle(rootca_crt_bundle_start,
                        static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
    // No setTimeout() here: HTTPClient::setTimeout below covers the read timeout
    // once the stream exists, so this would be redundant.
    //
    // Note on log noise you will see on every HTTPS poll:
    //   [E][NetworkClient.cpp:323] setSocketOption(): fail on 0, errno: 9
    // three times per fetch. That is upstream and harmless. NetworkClient::read
    // and ::write apply SO_RCVTIMEO/SO_SNDTIMEO using the base class's fd, but
    // NetworkClientSecure never sets it — TLS runs its own socket through
    // sslclient — so the option lands on fd 0 and fails. Polls succeed regardless.
    // Silencing it would mean giving up the timeouts below, which is a far worse
    // trade than a noisy log: they are what stops a stalled server holding the
    // poll loop.
    client = &tls;
  } else {
    // Plain HTTP is for the LAN only, and only because the user asked for an
    // http:// URL explicitly.
    client = &plain;
  }

  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(TOTAL_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  http.setReuse(false);
  // Release assets on GitHub redirect to another host; harmless here and needed
  // by the self-update path later.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  const String url = settings.base_url + SUMMARY_PATH;
  if (!http.begin(*client, url)) {
    return secure ? FetchResult::TlsFailed : FetchResult::ConnectFailed;
  }

  // Both cookie names, so one build works over plain HTTP on the LAN and HTTPS on
  // the VPS with no branching: __Host- prefixed cookies are refused by browsers
  // over HTTP, and the server accepts either (auth.py _read_cookie).
  String cookie = "sig_kiosk=" + settings.token + "; __Host-sig_kiosk=" + settings.token;
  http.addHeader("Cookie", cookie);
  http.addHeader("Accept", "application/json");

  const int status = http.GET();
  if (status_code != nullptr) {
    *status_code = status;
  }

  if (status <= 0) {
    http.end();
    // HTTPClient folds every transport failure into a negative code; on an https
    // URL the overwhelmingly likely cause is the handshake.
    return secure ? FetchResult::TlsFailed : FetchResult::ConnectFailed;
  }
  if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_FORBIDDEN) {
    http.end();
    return FetchResult::Unauthorised;
  }
  if (status != HTTP_CODE_OK) {
    http.end();
    return FetchResult::HttpError;
  }

  const String body = http.getString();
  http.end();

  Snapshot parsed;
  if (!snapshot_parse(body.c_str(), body.length(), &parsed)) {
    return FetchResult::BadPayload;
  }
  *out = parsed;
  return FetchResult::Ok;
}

#include "solar_api.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "board_config.h"
#include "settings.h"
#include "solar_forecast.h"

namespace {

constexpr const char* HOST = "https://api.open-meteo.com/v1/forecast";

// Open-Meteo publishes a new run roughly hourly, and a forecast for the next few
// hours is not the sort of thing that moves in minutes. Twenty-four requests a
// day against a free tier that allows ten thousand.
constexpr uint32_t REFRESH_MS = 60 * 60 * 1000;

// Failed fetches retry sooner than that, but not immediately: on this path the
// whole feature is one ring and four figures, and hammering a public endpoint
// because the WiFi dropped would be rude.
constexpr uint32_t RETRY_MS = 5 * 60 * 1000;

// forecast_days=1 gives today in local time, so 24 hourly records. A little
// slack in case an endpoint ever returns a partial extra hour.
constexpr size_t MAX_HOURS = 26;

constexpr uint16_t CONNECT_TIMEOUT_MS = 4000;
constexpr uint16_t TOTAL_TIMEOUT_MS = 8000;

constexpr const char* USER_AGENT = "SigenStorPuck/" PUCK_FW_VERSION " (ESP32-S3)";

// Same bundle sigen_api.cpp validates against, redeclared because that one is
// file-local. One mechanism for every TLS host the device speaks to.
extern "C" const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

// The weather, as fetched. Kept separately from the generation figures below
// because the two go stale for different reasons: this expires with the day,
// whereas the figures expire the moment somebody corrects a tilt on the settings
// page. Editing the roof should not cost a round trip to Germany.
SolarHour s_hours[MAX_HOURS];
size_t s_hour_count = 0;
uint32_t s_day_start = 0;  // UTC epoch of the local midnight the hours start at
int32_t s_utc_offset_min = 0;
bool s_have_weather = false;

// This site's generation, integrated from those hours.
float s_slot_kwh[SOLAR_SLOTS_PER_DAY] = {};
bool s_have_forecast = false;
SolarSite s_computed_for;

uint32_t s_last_attempt_ms = 0;
FetchResult s_last_result = FetchResult::NotConfigured;

// Assembles the configured site, or reports that there is not one.
//
// Deliberately the same test the server makes in _solar_block(): coordinates and
// at least one array with a rating. An array row left at its defaults is not a
// half-configured array, it is an empty slot.
bool site_from_settings(SolarSite* out) {
  const Settings& settings = settings_get();
  if (!settings.solar_location_set) {
    return false;
  }
  out->latitude = settings.latitude;
  out->longitude = settings.longitude;
  out->system_loss = settings.solar_system_loss;
  out->inverter_cap_kw = settings.solar_inverter_cap_kw;
  out->array_count = 0;
  for (size_t i = 0; i < SOLAR_MAX_ARRAYS; ++i) {
    if (settings.solar_arrays[i].kwp > 0.0f) {
      out->array[out->array_count++] = settings.solar_arrays[i];
    }
  }
  return out->array_count > 0;
}

bool same_site(const SolarSite& a, const SolarSite& b) {
  if (a.latitude != b.latitude || a.longitude != b.longitude || a.system_loss != b.system_loss ||
      a.inverter_cap_kw != b.inverter_cap_kw || a.array_count != b.array_count) {
    return false;
  }
  for (size_t i = 0; i < a.array_count && i < SOLAR_MAX_ARRAYS; ++i) {
    if (a.array[i].kwp != b.array[i].kwp || a.array[i].tilt_deg != b.array[i].tilt_deg ||
        a.array[i].azimuth_deg != b.array[i].azimuth_deg) {
      return false;
    }
  }
  return true;
}

// Only the coordinates decide what the weather request asks for. Everything else
// — tilt, rating, losses, the cap — is applied to hours already in hand.
bool same_place(const SolarSite& a, const SolarSite& b) {
  return a.latitude == b.latitude && a.longitude == b.longitude;
}

// TLS cannot judge a certificate's validity without a clock, and a device that
// has never reached NTP sits in 1970 where everything looks not-yet-valid.
bool clock_is_plausible() {
  return time(nullptr) > 1704067200;  // 1 Jan 2024
}

void recompute(const SolarSite& site) {
  solar_forecast_day(site, s_day_start, s_hours, s_hour_count, s_day_start, s_slot_kwh);
  s_computed_for = site;
  s_have_forecast = true;

  const SolarSummary summary =
      solar_summarise(s_slot_kwh, s_day_start, static_cast<uint32_t>(time(nullptr)));
  Serial.printf("[solar] forecast %.1f kWh today, %.1f kWh to come, peak %.2f kW, tz %+d min\n",
                static_cast<double>(summary.forecast_kwh),
                static_cast<double>(summary.remaining_kwh),
                static_cast<double>(summary.peak_kw), static_cast<int>(s_utc_offset_min));
}

FetchResult fetch_weather(const SolarSite& site) {
  if (WiFi.status() != WL_CONNECTED) {
    return FetchResult::NoNetwork;
  }
  if (!clock_is_plausible()) {
    return FetchResult::ClockUnset;
  }

  // timezone=auto is doing two jobs. It makes the day a *local* day, so the
  // twenty-four hours returned are the ones the ring is a fraction of; and it
  // makes the response carry utc_offset_seconds, which is the only thing on this
  // path that knows what local means — the Puck sets no timezone of its own
  // (net.cpp) and plant_system_timezone reads 0 on this plant even in summer.
  char url[256];
  snprintf(url, sizeof(url),
           "%s?latitude=%.5f&longitude=%.5f"
           "&hourly=direct_normal_irradiance,diffuse_radiation"
           "&timezone=auto&forecast_days=1&timeformat=unixtime",
           HOST, static_cast<double>(site.latitude), static_cast<double>(site.longitude));

  WiFiClientSecure tls;
  tls.setCACertBundle(rootca_crt_bundle_start,
                      static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));

  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(TOTAL_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(tls, url)) {
    return FetchResult::TlsFailed;
  }
  http.addHeader("Accept", "application/json");

  const int status = http.GET();
  if (status <= 0) {
    http.end();
    return FetchResult::TlsFailed;
  }
  if (status != HTTP_CODE_OK) {
    http.end();
    return FetchResult::HttpError;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    return FetchResult::BadPayload;
  }

  JsonArrayConst times = doc["hourly"]["time"];
  JsonArrayConst beam = doc["hourly"]["direct_normal_irradiance"];
  JsonArrayConst diffuse = doc["hourly"]["diffuse_radiation"];
  if (times.isNull() || beam.isNull() || diffuse.isNull() || times.size() == 0) {
    return FetchResult::BadPayload;
  }

  // With timeformat=unixtime the hourly stamps are true UTC seconds whatever
  // timezone was asked for, so the first one *is* the UTC instant this local day
  // begins at — no date arithmetic, and right in the zones offset by three
  // quarters of an hour that a whole-hour assumption would put 45 minutes out.
  const uint32_t first_hour = times[0].as<uint32_t>();
  if (first_hour == 0) {
    return FetchResult::BadPayload;
  }

  size_t count = 0;
  for (size_t i = 0; i < times.size() && count < MAX_HOURS; ++i) {
    // Nulls do occur at the edge of a model's coverage. Read as a dark hour
    // rather than skipped, so index and hour stay in step.
    s_hours[count].direct_normal =
        i < beam.size() && !beam[i].isNull() ? beam[i].as<float>() : 0.0f;
    s_hours[count].diffuse =
        i < diffuse.size() && !diffuse[i].isNull() ? diffuse[i].as<float>() : 0.0f;
    ++count;
  }

  s_hour_count = count;
  s_day_start = first_hour;
  s_utc_offset_min = doc["utc_offset_seconds"].as<int32_t>() / 60;
  s_have_weather = true;
  return FetchResult::Ok;
}

}  // namespace

FetchResult solar_api_service() {
  SolarSite site;
  if (!site_from_settings(&site)) {
    // Not a failure to retry — there is nothing to fetch until someone fills the
    // form in. The figures are dropped too, or switching the location off would
    // leave the old ones on screen looking live.
    s_have_weather = false;
    s_have_forecast = false;
    s_last_result = FetchResult::NotConfigured;
    // Clearing the attempt clock too, so filling the form back in fetches at
    // once. Left as it was, somebody who blanked the location and typed it again
    // would sit through the retry interval below for no reason.
    s_last_attempt_ms = 0;
    return s_last_result;
  }

  const uint32_t now_ms = millis();
  const uint32_t now_s = static_cast<uint32_t>(time(nullptr));

  // A forecast covers one local day, so it expires at that day's end however
  // recently it was fetched. Without this, a device left running would show
  // yesterday's curve well into the morning.
  const bool rolled = s_have_weather && now_s >= s_day_start + 86400;
  const bool moved = s_have_forecast && !same_place(site, s_computed_for);
  const bool stale = s_last_attempt_ms == 0 || now_ms - s_last_attempt_ms >= REFRESH_MS;
  // A fetch that failed waits its full retry regardless of what prompted the next
  // one. Otherwise a mistyped latitude would retry on every poll — the failure
  // triggers above are all still true while it is still wrong.
  const bool retrying_too_soon = s_last_result != FetchResult::Ok && s_last_attempt_ms != 0 &&
                                 now_ms - s_last_attempt_ms < RETRY_MS;

  if ((!s_have_weather || rolled || moved || stale) && !retrying_too_soon) {
    s_last_attempt_ms = now_ms;
    s_last_result = fetch_weather(site);
    if (s_last_result != FetchResult::Ok) {
      Serial.printf("[solar] forecast fetch failed: %s\n", fetch_result_name(s_last_result));
      return s_last_result;
    }
    recompute(site);
    return s_last_result;
  }

  // The weather still stands but the roof was edited. Correcting a tilt and
  // waiting an hour to see whether it helped would make the settings form
  // untestable, and none of those fields change what Open-Meteo was asked.
  if (!s_have_forecast || !same_site(site, s_computed_for)) {
    recompute(site);
  }
  return s_last_result;
}

void solar_api_apply(Snapshot* snapshot) {
  if (snapshot == nullptr || !snapshot->valid) {
    return;
  }
  if (!s_have_forecast) {
    // Configured but nothing fetched yet reports `configured` with nulls, the
    // same shape the server sends when Open-Meteo is unreachable: the screen then
    // says "no forecast yet" rather than "no PV system".
    snapshot->solar.configured = s_last_result != FetchResult::NotConfigured;
    return;
  }

  // Ahead of the forecast block: a chart anchored to the wrong midnight is the
  // more visible of the two errors this fetch prevents.
  snapshot->tz_offset_min.known = true;
  snapshot->tz_offset_min.value = s_utc_offset_min;

  const SolarSummary summary =
      solar_summarise(s_slot_kwh, s_day_start, static_cast<uint32_t>(time(nullptr)));

  snapshot->solar.configured = true;
  snapshot->solar.forecast_kwh = {true, summary.forecast_kwh};
  snapshot->solar.remaining_kwh = {true, summary.remaining_kwh};
  snapshot->solar.peak_kw = {true, summary.peak_kw};

  // Actual against what the forecast said should have arrived by now. The floor
  // is the server's own: before dawn the denominator is a rounding error and the
  // ratio swings between nothing and thousands of per cent.
  const float so_far = summary.forecast_kwh - summary.remaining_kwh;
  if (snapshot->today.present && snapshot->today.solar.known && so_far > 0.05f) {
    snapshot->solar.vs_forecast_pct = {true, snapshot->today.solar.value / so_far * 100.0f};
  } else {
    snapshot->solar.vs_forecast_pct = {};
  }
}

bool solar_api_ready() {
  return s_have_forecast;
}

FetchResult solar_api_last_result() {
  return s_last_result;
}

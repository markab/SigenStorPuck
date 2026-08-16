#include "solar_forecast.h"

#include <math.h>
#include <time.h>

namespace {

constexpr float PI = 3.14159265358979f;

float radians(float degrees) {
  return degrees * PI / 180.0f;
}

float degrees_of(float radians_value) {
  return radians_value * 180.0f / PI;
}

}  // namespace

void solar_position(uint32_t epoch_utc, float latitude, float longitude, float* elevation_deg,
                    float* azimuth_deg) {
  const time_t when = static_cast<time_t>(epoch_utc);
  struct tm utc = {};
  if (gmtime_r(&when, &utc) == nullptr) {
    *elevation_deg = -90.0f;
    *azimuth_deg = 0.0f;
    return;
  }

  const float day = static_cast<float>(utc.tm_yday + 1);
  const float hour = static_cast<float>(utc.tm_hour) + static_cast<float>(utc.tm_min) / 60.0f +
                     static_cast<float>(utc.tm_sec) / 3600.0f;

  const float declination = radians(23.45f) * sinf(radians(360.0f * (284.0f + day) / 365.0f));

  // The equation of time: the sun does not keep clock time, and by early
  // November it is a quarter of an hour ahead of it. Dropping this would shift
  // the whole curve rather than change its shape, which is worse — the peak
  // would land in the wrong half hour.
  const float b = radians(360.0f * (day - 81.0f) / 364.0f);
  const float eot = 9.87f * sinf(2.0f * b) - 7.53f * cosf(b) - 1.5f * sinf(b);

  const float solar_time = hour + (longitude * 4.0f + eot) / 60.0f;
  const float hour_angle = radians(15.0f * (solar_time - 12.0f));
  const float lat = radians(latitude);

  const float elevation = asinf(sinf(declination) * sinf(lat) +
                                cosf(declination) * cosf(lat) * cosf(hour_angle));
  float azimuth = atan2f(sinf(hour_angle),
                         cosf(hour_angle) * sinf(lat) - tanf(declination) * cosf(lat));
  azimuth = fmodf(degrees_of(azimuth) + 180.0f, 360.0f);
  if (azimuth < 0.0f) {
    azimuth += 360.0f;
  }

  *elevation_deg = degrees_of(elevation);
  *azimuth_deg = azimuth;
}

float solar_poa_factor(float elevation_deg, float azimuth_deg, float tilt_deg,
                       float panel_azimuth_deg) {
  if (elevation_deg <= 0.0f) {
    return 0.0f;
  }
  const float elevation = radians(elevation_deg);
  const float azimuth = radians(azimuth_deg);
  const float tilt = radians(tilt_deg);
  const float panel = radians(panel_azimuth_deg);

  const float cos_incidence = sinf(elevation) * cosf(tilt) +
                              cosf(elevation) * sinf(tilt) * cosf(azimuth - panel);
  return cos_incidence > 0.0f ? cos_incidence : 0.0f;
}

void solar_forecast_day(const SolarSite& site, uint32_t day_start_epoch, const SolarHour* hours,
                        size_t hour_count, uint32_t first_hour_epoch, float* out_slot_kwh) {
  if (out_slot_kwh == nullptr) {
    return;
  }
  for (size_t i = 0; i < SOLAR_SLOTS_PER_DAY; ++i) {
    out_slot_kwh[i] = 0.0f;
  }
  if (hours == nullptr || hour_count == 0 || site.array_count == 0) {
    return;
  }

  constexpr uint32_t SLOT_SECONDS = SOLAR_SLOT_MINUTES * 60;
  const float hours_per_slot = static_cast<float>(SOLAR_SLOT_MINUTES) / 60.0f;

  for (size_t i = 0; i < SOLAR_SLOTS_PER_DAY; ++i) {
    // Sampled at the middle of the slot rather than its edge: a half hour either
    // side of sunrise is the difference between nothing and something, and the
    // midpoint is the honest average of the two.
    const uint32_t middle = day_start_epoch + static_cast<uint32_t>(i) * SLOT_SECONDS +
                            SLOT_SECONDS / 2;
    if (middle < first_hour_epoch) {
      continue;
    }
    const size_t hour_index = (middle - first_hour_epoch) / 3600;
    if (hour_index >= hour_count) {
      continue;
    }
    const SolarHour& irradiance = hours[hour_index];

    float elevation = 0.0f;
    float azimuth = 0.0f;
    solar_position(middle, site.latitude, site.longitude, &elevation, &azimuth);

    float total = 0.0f;
    for (size_t a = 0; a < site.array_count && a < SOLAR_MAX_ARRAYS; ++a) {
      const PvArray& array = site.array[a];
      if (array.kwp <= 0.0f) {
        continue;
      }
      const float beam =
          irradiance.direct_normal *
          solar_poa_factor(elevation, azimuth, array.tilt_deg, array.azimuth_deg);
      // The sky's own contribution, which a tilted panel sees less of the more it
      // leans over. An isotropic sky — cruder than the models a design tool would
      // use, and the same one the server settles for.
      const float sky = irradiance.diffuse * (1.0f + cosf(radians(array.tilt_deg))) / 2.0f;
      const float plane_of_array = beam + sky;
      total += array.kwp * (plane_of_array / 1000.0f) * site.system_loss * hours_per_slot;
    }

    if (site.inverter_cap_kw > 0.0f) {
      const float cap = site.inverter_cap_kw * hours_per_slot;
      if (total > cap) {
        total = cap;
      }
    }
    out_slot_kwh[i] = total;
  }
}

SolarSummary solar_summarise(const float* slot_kwh, uint32_t day_start_epoch, uint32_t now_epoch) {
  SolarSummary summary;
  if (slot_kwh == nullptr) {
    return summary;
  }
  constexpr uint32_t SLOT_SECONDS = SOLAR_SLOT_MINUTES * 60;
  const float hours_per_slot = static_cast<float>(SOLAR_SLOT_MINUTES) / 60.0f;

  for (size_t i = 0; i < SOLAR_SLOTS_PER_DAY; ++i) {
    const float slot = slot_kwh[i];
    summary.forecast_kwh += slot;
    // Slots that have not started yet, matching how the server counts what is
    // left rather than prorating the slot now in progress. The two never differ
    // by a sliver that way.
    if (day_start_epoch + static_cast<uint32_t>(i) * SLOT_SECONDS >= now_epoch) {
      summary.remaining_kwh += slot;
    }
    const float kw = slot / hours_per_slot;
    if (kw > summary.peak_kw) {
      summary.peak_kw = kw;
    }
  }
  return summary;
}

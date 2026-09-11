#pragma once

#include <stdint.h>

// The Solar screen's three optional forecast figures, as bits. Which of them a
// source can supply is decided once, from configuration — never from whether a
// value happens to be known this minute. Deciding it per reading made the figures
// shuffle every morning: vs-forecast is withheld before dawn by both the server
// and the Puck's own model, so PEAK slid into its place and back again once
// enough of the day had been forecast to compare against.
enum SolarFigure : uint8_t {
  SOLAR_FIGURE_REMAINING = 1u << 0,
  SOLAR_FIGURE_VS_FORECAST = 1u << 1,
  SOLAR_FIGURE_PEAK = 1u << 2,
};
static constexpr uint8_t SOLAR_FIGURES_ALL =
    SOLAR_FIGURE_REMAINING | SOLAR_FIGURE_VS_FORECAST | SOLAR_FIGURE_PEAK;

// Slots are packed left-to-right, top-to-bottom after today's total forecast, so a
// figure the source can never supply leaves no hole. A source that supplies all
// three gets the original fixed layout: total and remaining, then vs and peak.
struct SolarOptionalMetricSlots {
  static constexpr int8_t Hidden = -1;

  int8_t remaining = Hidden;
  int8_t vs_forecast = Hidden;
  int8_t peak = Hidden;
};

SolarOptionalMetricSlots solar_optional_metric_slots(uint8_t supplied);

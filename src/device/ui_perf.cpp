#include "ui/ui_perf.h"

#if PUCK_UI_PERF && defined(ARDUINO)

#include <Arduino.h>

namespace {

struct TransitionStats {
  bool active = false;
  bool ready = false;
  UiPerfTransitionKind kind = UiPerfTransitionKind::Programmatic;
  int from_screen = 0;
  int to_screen = 0;
  bool animated = false;
  bool from_chart = false;
  bool to_chart = false;
  uint32_t started_us = 0;
  uint8_t rotation = 0;
  uint32_t flushes = 0;
  uint32_t frames = 0;
  uint32_t pixels = 0;
  uint32_t rotation_us = 0;
  uint32_t panel_us = 0;
  uint32_t chart_refresh_us = 0;
  uint32_t chart_refreshes = 0;
  uint32_t chart_draw_us = 0;
  uint32_t chart_draws = 0;
  uint32_t history_reduce_us = 0;
  uint32_t smooth_columns_us = 0;
};

TransitionStats s_stats;
uint32_t s_pending_history_reduce_us = 0;
uint32_t s_pending_smooth_columns_us = 0;

const char* kind_name(UiPerfTransitionKind kind) {
  switch (kind) {
    case UiPerfTransitionKind::AutoCycle:
      return "auto";
    case UiPerfTransitionKind::Swipe:
      return "swipe";
    default:
      return "programmatic";
  }
}

const char* screen_type(bool chart) {
  return chart ? "chart" : "simple";
}

void add_section_time(UiPerfSection section, uint32_t elapsed_us) {
  if (!s_stats.active) {
    if (section == UiPerfSection::HistoryReduce) {
      s_pending_history_reduce_us += elapsed_us;
    } else if (section == UiPerfSection::SmoothColumns) {
      s_pending_smooth_columns_us += elapsed_us;
    } else if (section == UiPerfSection::ChartRefresh) {
      Serial.printf("[ui-perf] chart-refresh=%uus history-reduce=%uus "
                    "smooth-columns=%uus\n",
                    elapsed_us, s_pending_history_reduce_us,
                    s_pending_smooth_columns_us);
      s_pending_history_reduce_us = 0;
      s_pending_smooth_columns_us = 0;
    }
    return;
  }
  switch (section) {
    case UiPerfSection::ChartRefresh:
      s_stats.chart_refresh_us += elapsed_us;
      ++s_stats.chart_refreshes;
      break;
    case UiPerfSection::ChartDraw:
      s_stats.chart_draw_us += elapsed_us;
      ++s_stats.chart_draws;
      break;
    case UiPerfSection::HistoryReduce:
      s_stats.history_reduce_us += elapsed_us;
      break;
    case UiPerfSection::SmoothColumns:
      s_stats.smooth_columns_us += elapsed_us;
      break;
  }
}

}  // namespace

uint32_t ui_perf_now_us() {
  return micros();
}

void ui_perf_transition_begin(UiPerfTransitionKind kind, int from_screen,
                              int to_screen, bool animated, bool from_chart,
                              bool to_chart) {
  s_stats = TransitionStats{};
  s_stats.active = true;
  s_stats.kind = kind;
  s_stats.from_screen = from_screen;
  s_stats.to_screen = to_screen;
  s_stats.animated = animated;
  s_stats.from_chart = from_chart;
  s_stats.to_chart = to_chart;
  s_stats.started_us = micros();
}

void ui_perf_swipe_begin(int from_screen, bool from_chart) {
  // Programmatic scrolling emits the same LVGL event. Its caller already gave
  // us the intended destination and animation mode, so do not replace it with a
  // less informative "swipe" record.
  if (!s_stats.active) {
    ui_perf_transition_begin(UiPerfTransitionKind::Swipe, from_screen, -1, true,
                             from_chart, false);
  }
}

void ui_perf_transition_ready(int to_screen, bool to_chart) {
  if (!s_stats.active) {
    return;
  }
  s_stats.to_screen = to_screen;
  s_stats.to_chart = to_chart;
  s_stats.ready = true;
}

void ui_perf_flush(uint8_t rotation, uint32_t pixels, uint32_t rotation_us,
                   uint32_t panel_us, bool frame_end) {
  if (!s_stats.active) {
    return;
  }
  ++s_stats.flushes;
  s_stats.rotation = rotation;
  s_stats.pixels += pixels;
  s_stats.rotation_us += rotation_us;
  s_stats.panel_us += panel_us;
  if (!frame_end) {
    return;
  }
  ++s_stats.frames;
  if (!s_stats.ready) {
    return;
  }

  const uint32_t total_us = micros() - s_stats.started_us;
  const uint32_t frame_interval_us =
      s_stats.frames > 1 ? total_us / (s_stats.frames - 1) : 0;
  Serial.printf(
      "[ui-perf] rotation=%u %s %s %d->%d %s->%s total=%uus frames=%u "
      "interval~=%uus "
      "flushes=%u pixels=%u rotate=%uus panel=%uus chart-refresh=%uus/%u "
      "history-reduce=%uus smooth-columns=%uus chart-draw=%uus/%u\n",
      static_cast<unsigned>(s_stats.rotation), kind_name(s_stats.kind),
      s_stats.animated ? "animated" : "instant", s_stats.from_screen,
      s_stats.to_screen, screen_type(s_stats.from_chart),
      screen_type(s_stats.to_chart), total_us, s_stats.frames, frame_interval_us,
      s_stats.flushes, s_stats.pixels, s_stats.rotation_us, s_stats.panel_us,
      s_stats.chart_refresh_us, s_stats.chart_refreshes,
      s_stats.history_reduce_us, s_stats.smooth_columns_us,
      s_stats.chart_draw_us, s_stats.chart_draws);
  s_stats.active = false;
}

UiPerfTimer::UiPerfTimer(UiPerfSection section)
    : section_(section), started_us_(micros()) {}

UiPerfTimer::~UiPerfTimer() {
  add_section_time(section_, micros() - started_us_);
}

#endif

// Opt-in timing for the physical display hot path.
//
// Enable with PLATFORMIO_BUILD_FLAGS="-D PUCK_UI_PERF=1". The normal build has
// zero logging and the calls below compile away. Timings deliberately aggregate
// per transition: per-flush or per-column logging would perturb the work being
// measured and swamp the serial port.

#pragma once

#include <stdint.h>

#ifndef PUCK_UI_PERF
#define PUCK_UI_PERF 0
#endif

enum class UiPerfTransitionKind : uint8_t {
  Programmatic,
  AutoCycle,
  Swipe,
};

enum class UiPerfSection : uint8_t {
  ChartRefresh,
  ChartDraw,
  HistoryReduce,
  SmoothColumns,
};

#if PUCK_UI_PERF && defined(ARDUINO)

void ui_perf_transition_begin(UiPerfTransitionKind kind, int from_screen,
                              int to_screen, bool animated, bool from_chart,
                              bool to_chart);
void ui_perf_swipe_begin(int from_screen, bool from_chart);
void ui_perf_transition_ready(int to_screen, bool to_chart);
void ui_perf_flush(uint8_t rotation, uint32_t pixels, uint32_t rotation_us,
                   uint32_t panel_us, bool frame_end);

class UiPerfTimer {
 public:
  explicit UiPerfTimer(UiPerfSection section);
  ~UiPerfTimer();

 private:
  UiPerfSection section_;
  uint32_t started_us_;
};

uint32_t ui_perf_now_us();

#else

inline void ui_perf_transition_begin(UiPerfTransitionKind, int, int, bool, bool,
                                     bool) {}
inline void ui_perf_swipe_begin(int, bool) {}
inline void ui_perf_transition_ready(int, bool) {}
inline void ui_perf_flush(uint8_t, uint32_t, uint32_t, uint32_t, bool) {}

class UiPerfTimer {
 public:
  explicit UiPerfTimer(UiPerfSection) {}
};

inline uint32_t ui_perf_now_us() {
  return 0;
}

#endif

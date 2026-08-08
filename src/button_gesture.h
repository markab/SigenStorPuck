// Turning a button's held/not-held level into a press, a double press or a hold.
//
// Lives outside src/device/ on purpose, same as enrol_url.cpp and history.cpp:
// there is nothing Arduino about it, and it is fiddly enough to have had two
// bugs in it that only showed up under a finger. The simulator's selftest drives
// it with synthetic timelines.
//
// A level rather than events, so the GPIO button and the PMIC button — which are
// wired nothing alike — run through the same recogniser and behave identically.

#pragma once

#include <stdint.h>

enum class ButtonGesture : uint8_t { None, Press, Double, Hold };

struct ButtonGestureConfig {
  // Minimum press length. Contact bounce on a bare GPIO; zero for a button that
  // arrives already debounced, where a minimum would throw away the shortest
  // real taps rather than noise.
  uint32_t debounce_ms = 0;
  // How long after letting go a second press has to *start* to count as a
  // double. Also the delay a single press carries, since the two are the same
  // wait: telling one press from the first half of two means waiting to find out.
  uint32_t double_ms = 400;
  // A hold, reported while the button is still down rather than on release, so
  // it is obvious it has worked without having to let go and find out.
  uint32_t hold_ms = 3000;
};

struct ButtonGestureState {
  ButtonGestureConfig config;

  bool was_down = false;
  uint32_t down_at = 0;
  bool hold_fired = false;
  uint32_t last_release = 0;
  bool press_pending = false;
  // The second tap of a double, already acted on at its press. Its release must
  // not then be taken for the first tap of another one.
  bool double_armed = false;

  // Call with the button's current level and the current millisecond count.
  // Returns at most one gesture per call.
  ButtonGesture update(bool down, uint32_t now);
};

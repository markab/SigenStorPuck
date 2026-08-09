// Turning a button's held/not-held level into a press or one of two holds.
//
// Lives outside src/device/ on purpose, same as enrol_url.cpp and history.cpp:
// there is nothing Arduino about it, and it is fiddly enough to have had bugs in
// it that only showed up under a finger. The simulator's selftest drives it with
// synthetic timelines.
//
// A level rather than events, so the GPIO button and the PMIC button — which are
// wired nothing alike — run through the same recogniser and behave identically.
//
// There is deliberately no double press. One was tried and was unreliable in the
// hand, and it cost every single press a wait: telling one press from the first
// half of two means waiting out the whole double window before acting. Two hold
// lengths need no such wait, so a press now fires the moment the button comes up.

#pragma once

#include <stdint.h>

enum class ButtonGesture : uint8_t {
  None,
  Press,     // pressed and released without reaching the first hold
  Hold,      // held past hold_ms, reported while still down
  LongHold,  // held past long_hold_ms, likewise
};

struct ButtonGestureConfig {
  // Minimum press length. Contact bounce on a bare GPIO; zero for a button that
  // arrives already debounced, where a minimum would throw away the shortest
  // real taps rather than noise.
  uint32_t debounce_ms = 0;
  // Both holds are reported while the button is still down rather than on
  // release, so it is obvious they have worked without having to let go and find
  // out. The cost is that holding through to the long one fires the short one on
  // the way — acceptable here, because both long actions end with the device
  // restarting or powering off and nothing the short one did will outlive it.
  uint32_t hold_ms = 2000;
  uint32_t long_hold_ms = 5000;
};

struct ButtonGestureState {
  ButtonGestureConfig config;

  bool was_down = false;
  uint32_t down_at = 0;
  bool hold_fired = false;
  bool long_hold_fired = false;

  // Call with the button's current level and the current millisecond count.
  // Returns at most one gesture per call.
  ButtonGesture update(bool down, uint32_t now);
};

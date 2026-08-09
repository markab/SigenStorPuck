#include "button_gesture.h"

ButtonGesture ButtonGestureState::update(bool down, uint32_t now) {
  ButtonGesture gesture = ButtonGesture::None;

  if (down && !was_down) {
    down_at = now;
    hold_fired = false;
    long_hold_fired = false;
  } else if (down) {
    const uint32_t held = now - down_at;
    // The long hold is tested first so that a poll gap wide enough to skip both
    // thresholds reports the one the finger actually earned, rather than the
    // short hold followed by nothing.
    if (!long_hold_fired && held >= config.long_hold_ms) {
      long_hold_fired = true;
      gesture = ButtonGesture::LongHold;
    } else if (!hold_fired && held >= config.hold_ms) {
      hold_fired = true;
      gesture = ButtonGesture::Hold;
    }
  } else if (was_down) {
    // Fired on release, with nothing to wait for: a press is simply a press that
    // did not become a hold. This is what dropping the double press bought.
    const uint32_t held = now - down_at;
    if (!hold_fired && !long_hold_fired && held >= config.debounce_ms) {
      gesture = ButtonGesture::Press;
    }
  }

  was_down = down;
  return gesture;
}

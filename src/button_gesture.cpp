#include "button_gesture.h"

ButtonGesture ButtonGestureState::update(bool down, uint32_t now) {
  ButtonGesture gesture = ButtonGesture::None;

  if (down && !was_down) {
    // The double is decided here, at the second press, not at its release: the
    // gap that matters is the one the finger makes between letting go and
    // pressing again. Waiting for the release folds however long the second
    // press was held into the window, so a pair of unhurried taps came out as
    // two singles — which is exactly what it did.
    if (press_pending && now - last_release <= config.double_ms) {
      press_pending = false;
      double_armed = true;
      gesture = ButtonGesture::Double;
    }
    down_at = now;
    hold_fired = false;
  } else if (down && !hold_fired && now - down_at >= config.hold_ms) {
    // A hold cancels whatever the press was going to become: holding the button
    // after a tap must not also step a day on the way to a restart.
    hold_fired = true;
    press_pending = false;
    double_armed = false;
    gesture = ButtonGesture::Hold;
  } else if (!down && was_down) {
    const uint32_t held = now - down_at;
    if (hold_fired || double_armed || held < config.debounce_ms) {
      // The release that ends a hold or completes a double, both already acted
      // on — or contact bounce.
      double_armed = false;
    } else {
      press_pending = true;
      last_release = now;
    }
  } else if (press_pending && now - last_release > config.double_ms) {
    // The window closed with no second press, so it was a single after all.
    press_pending = false;
    gesture = ButtonGesture::Press;
  }

  was_down = down;
  return gesture;
}

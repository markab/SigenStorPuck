#include "buttons.h"

#include "board_config.h"

namespace {

// Long enough to ignore contact bounce, short enough not to feel sticky.
constexpr uint32_t DEBOUNCE_MS = 40;
// Beyond this it is a hold, not a press, and nothing happens — so a leaning
// finger or something resting on the button cannot reboot the device.
constexpr uint32_t MAX_PRESS_MS = 1500;

bool s_was_down = false;
uint32_t s_down_at = 0;

}  // namespace

void buttons_begin() {
  // Active low, with the internal pull-up: the button pulls GPIO0 to ground.
  pinMode(PUCK_BUTTON_BOOT, INPUT_PULLUP);
  Serial.println("[buttons] BOOT reboots on a short press");
}

void buttons_loop() {
  const bool down = digitalRead(PUCK_BUTTON_BOOT) == LOW;
  const uint32_t now = millis();

  if (down && !s_was_down) {
    s_down_at = now;
  } else if (!down && s_was_down) {
    const uint32_t held = now - s_down_at;
    if (held >= DEBOUNCE_MS && held <= MAX_PRESS_MS) {
      Serial.println("[buttons] BOOT pressed, restarting");
      delay(50);
      ESP.restart();
    }
  }
  s_was_down = down;
}

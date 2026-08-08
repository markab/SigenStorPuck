#include "buttons.h"

#include "board_config.h"
#include "power.h"
#include "ui/ui.h"

namespace {

// Long enough to ignore contact bounce, short enough not to feel sticky.
constexpr uint32_t DEBOUNCE_MS = 40;

// How long a second press has to arrive within to count as a double. Also the
// delay a single press carries, since the two are the same wait.
constexpr uint32_t DOUBLE_MS = 320;

// A hold. Fires while the button is still down rather than on release, so it is
// obvious it has worked without having to let go and find out.
constexpr uint32_t HOLD_MS = 3000;

enum class Gesture : uint8_t { None, Press, Double, Hold };

// One recogniser, driven by a held/not-held level, so the GPIO button and the
// PMIC button behave identically. Both are polled from loop() every few
// milliseconds, which is far finer than any of the thresholds above.
struct Recogniser {
  bool was_down = false;
  uint32_t down_at = 0;
  bool hold_fired = false;
  uint32_t last_release = 0;
  bool press_pending = false;

  Gesture update(bool down, uint32_t now) {
    Gesture gesture = Gesture::None;

    if (down && !was_down) {
      down_at = now;
      hold_fired = false;
    } else if (down && !hold_fired && now - down_at >= HOLD_MS) {
      // A hold cancels anything the press was going to become: holding the
      // button after a tap must not also step a day on the way to a restart.
      hold_fired = true;
      press_pending = false;
      gesture = Gesture::Hold;
    } else if (!down && was_down) {
      const uint32_t held = now - down_at;
      if (hold_fired || held < DEBOUNCE_MS) {
        // Bounce, or the release that ends a hold already acted on.
      } else if (press_pending && now - last_release <= DOUBLE_MS) {
        press_pending = false;
        gesture = Gesture::Double;
      } else {
        press_pending = true;
        last_release = now;
      }
    } else if (press_pending && now - last_release > DOUBLE_MS) {
      // The window closed with no second press, so it was a single after all.
      press_pending = false;
      gesture = Gesture::Press;
    }

    was_down = down;
    return gesture;
  }
};

Recogniser s_boot;
Recogniser s_pwr;

// Both day steps report what they did, because on a device with no keyboard the
// only confirmation you get is the screen.
void step_day(int delta) {
  if (!ui_day_stepping()) {
    // The Modbus source has daily counters and no dated API behind them, so
    // there is no past to step into. Better to say so than to look broken.
    ui_toast("LIVE ONLY");
    return;
  }
  const int before = ui_day_offset();
  ui_set_day_offset(before + delta);
  const int after = ui_day_offset();
  if (after == before) {
    // Already at today, or already as far back as the data goes. Say so rather
    // than leaving a press looking like it was missed.
    ui_toast(delta > 0 ? "TODAY" : "OLDEST DAY");
  } else if (after == 0) {
    ui_toast("TODAY");
  }
  Serial.printf("[buttons] day offset %d\n", after);
}

void handle_pwr(Gesture gesture) {
  switch (gesture) {
    case Gesture::Press:
      step_day(-1);
      break;
    case Gesture::Double: {
      const bool on = !ui_rotate_enabled();
      ui_set_rotate_enabled(on);
      ui_toast(on ? "AUTO-CYCLE ON" : "AUTO-CYCLE OFF");
      Serial.printf("[buttons] auto-cycle %s\n", on ? "on" : "off");
      break;
    }
    case Gesture::Hold:
      Serial.println("[buttons] PWR held, shutting down");
      ui_toast("POWERING OFF");
      // Long enough for the message to land on the glass before the rails go.
      delay(400);
      power_shutdown();
      break;
    default:
      break;
  }
}

void handle_boot(Gesture gesture) {
  switch (gesture) {
    case Gesture::Press:
      step_day(1);
      break;
    case Gesture::Double:
      ui_next_screen();
      break;
    case Gesture::Hold:
      Serial.println("[buttons] BOOT held, restarting");
      ui_toast("RESTARTING");
      delay(400);
      ESP.restart();
      break;
    default:
      break;
  }
}

}  // namespace

void buttons_begin() {
  // Active low, with the internal pull-up: the button pulls GPIO0 to ground.
  pinMode(PUCK_BUTTON_BOOT, INPUT_PULLUP);
  Serial.println("[buttons] PWR: day back / double auto-cycle / hold 3s off");
  Serial.println("[buttons] BOOT: day forward / double next screen / hold 3s restart");
}

void buttons_loop() {
  const uint32_t now = millis();

  const Gesture boot = s_boot.update(digitalRead(PUCK_BUTTON_BOOT) == LOW, now);
  if (boot != Gesture::None) {
    // Any button counts as activity, so a press wakes a dimmed screen and holds
    // the auto-cycle off — the same treatment a touch gets.
    lv_disp_trig_activity(nullptr);
    handle_boot(boot);
  }

  const Gesture pwr = s_pwr.update(power_key_down(), now);
  if (pwr != Gesture::None) {
    lv_disp_trig_activity(nullptr);
    handle_pwr(pwr);
  }
}

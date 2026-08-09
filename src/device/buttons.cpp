#include "buttons.h"

#include "board_config.h"
#include "button_gesture.h"
#include "poller.h"
#include "settings.h"
#include "power.h"
#include "ui/ui.h"

namespace {

// Long enough to ignore contact bounce on the raw GPIO, short enough not to feel
// sticky. Only BOOT needs it — see ButtonGestureConfig::debounce_ms.
constexpr uint32_t DEBOUNCE_MS = 40;

// Two seconds for the middle action, five for the one you should have to mean.
constexpr uint32_t HOLD_MS = 2000;
constexpr uint32_t LONG_HOLD_MS = 5000;

// BOOT is a bare GPIO and bounces, so a press shorter than a few tens of
// milliseconds is contact noise. PWR is debounced by the PMIC before it ever
// becomes an interrupt, and a minimum there would throw away exactly the taps
// power.cpp works hardest to preserve — the ones seen whole between two polls,
// which it can only report as lasting a single loop iteration.
ButtonGestureState s_boot{{DEBOUNCE_MS, HOLD_MS, LONG_HOLD_MS}};
ButtonGestureState s_pwr{{/*debounce_ms=*/0, HOLD_MS, LONG_HOLD_MS}};

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
    // Already at today, or as far back as the data goes. No toast: it shares its
    // slot with the day indicator, so saying "OLDEST DAY" would blank the date
    // you are looking at to tell you it has not changed. The date staying put is
    // the answer, and at today the indicator disappearing is.
    return;
  }
  // Kick the poll task rather than letting it notice on its own schedule. It
  // sleeps for the whole poll interval — five seconds by default — and the day
  // would sit on LOADING for all of it.
  poller_wake();
  Serial.printf("[buttons] day offset %d\n", after);
}

void handle_pwr(ButtonGesture gesture) {
  switch (gesture) {
    case ButtonGesture::Press:
      step_day(-1);
      break;
    case ButtonGesture::Hold: {
      const bool on = !ui_rotate_enabled();
      ui_set_rotate_enabled(on);
      // Persisted, so this is the same switch as the one on the settings page
      // rather than a runtime shadow of it that a reboot would silently undo.
      // A human-rate toggle is nothing to NVS.
      settings_set_rotate_enabled(on);
      ui_toast(on ? "AUTO-CYCLE ON" : "AUTO-CYCLE OFF");
      Serial.printf("[buttons] auto-cycle %s\n", on ? "on" : "off");
      break;
    }
    case ButtonGesture::LongHold:
      Serial.println("[buttons] PWR held long, shutting down");
      ui_toast("POWERING OFF");
      // Long enough for the message to land on the glass before the rails go.
      delay(400);
      power_shutdown();
      break;
    default:
      break;
  }
}

void handle_boot(ButtonGesture gesture) {
  switch (gesture) {
    case ButtonGesture::Press:
      step_day(1);
      break;
    case ButtonGesture::Hold:
      ui_next_screen();
      break;
    case ButtonGesture::LongHold:
      Serial.println("[buttons] BOOT held long, restarting");
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
  Serial.println("[buttons] PWR: day back / hold 2s auto-cycle / hold 5s off");
  Serial.println("[buttons] BOOT: day forward / hold 2s next screen / hold 5s restart");
}

void buttons_loop() {
  const uint32_t now = millis();

  const ButtonGesture boot = s_boot.update(digitalRead(PUCK_BUTTON_BOOT) == LOW, now);
  if (boot != ButtonGesture::None) {
    // Any button counts as activity, so a press wakes a dimmed screen and holds
    // the auto-cycle off — the same treatment a touch gets.
    lv_disp_trig_activity(nullptr);
    handle_boot(boot);
  }

  const ButtonGesture pwr = s_pwr.update(power_key_down(), now);
  if (pwr != ButtonGesture::None) {
    lv_disp_trig_activity(nullptr);
    handle_pwr(pwr);
  }
}

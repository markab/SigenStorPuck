// The two physical buttons, and what they do.
//
// They are wired quite differently, which used to limit what each could do:
//
//   BOOT is GPIO0, a normal readable pin, so firmware can do what it likes with
//   it — except that holding it while power is applied still forces USB download
//   mode. That is silicon strapping, not something firmware can override.
//
//   PWR is not a GPIO at all. It goes to the AXP2101's PWRON pin (Waveshare's own
//   BSP declares BSP_CAPS_BUTTONS 0), so it is reachable only through the PMIC.
//
// The PMIC turns out to report press and release edges, not just its own idea of
// a short or long press — so power.cpp can hand over a plain held/not-held level
// and both buttons run through one gesture recogniser here. That matters: the
// chip can only call a hold "long" at 1 s, 1.5 s or 2 s, and there is no way to
// get two thresholds out of it at all. The PMIC's own hardware cut-off is pushed
// out to 10 s so it cannot fire before the five-second one below.
//
//   PWR    press        a day back through the data
//          hold 2 s     auto-cycle on or off
//          hold 5 s     power off
//
//   BOOT   press        a day forward, no further than today
//          hold 2 s     next screen
//          hold 5 s     restart
//
// Two hold lengths rather than a double press. A double was tried and was
// unreliable in the hand, and it cost every single press a wait — telling one
// press from the first half of two means waiting out the whole double window
// before acting. A press now fires the moment the button comes up.
//
// Both holds report while the button is still down, so holding through to five
// seconds fires the two-second action on the way. That is harmless: both
// five-second actions end with the device restarting or powering off.

#pragma once

#include <Arduino.h>

void buttons_begin();

// Call from loop(). Reads both buttons and runs whatever they ask for.
void buttons_loop();

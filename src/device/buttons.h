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
// chip can only call a hold "long" at 1 s, 1.5 s or 2 s, and none of those is the
// three seconds these gestures want.
//
//   PWR    press        a day back through the data
//          double       auto-cycle on or off
//          hold 3 s     power off
//
//   BOOT   press        a day forward, no further than today
//          double       next screen
//          hold 3 s     restart
//
// A single press cannot fire until the double-press window has passed, so
// stepping a day carries that much delay. Unavoidable: telling one press from
// the first half of two means waiting to find out.

#pragma once

#include <Arduino.h>

void buttons_begin();

// Call from loop(). Reads both buttons and runs whatever they ask for.
void buttons_loop();

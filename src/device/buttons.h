// The two physical buttons.
//
// They are wired quite differently, which limits what each can do:
//
//   BOOT is GPIO0, a normal readable pin, so firmware can do what it likes with
//   it — except that holding it while power is applied still forces USB download
//   mode. That is silicon strapping, not something firmware can override.
//
//   PWR is not a GPIO at all. It goes to the AXP2101's PWRON pin (Waveshare's own
//   BSP declares BSP_CAPS_BUTTONS 0), so it is reachable only through the PMIC,
//   and its long-press power-off is handled by the chip in hardware.

#pragma once

#include <Arduino.h>

void buttons_begin();

// Call from loop(). A short press of BOOT reboots.
void buttons_loop();

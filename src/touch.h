// CST9217 capacitive touch over the shared I2C bus, wired to LVGL.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// Walks the shared I2C bus and logs every address that answers, naming the ones
// this board is expected to carry. Wire.begin() must already have been called.
// Worth doing on every boot: when touch is dead this one log line says whether
// the controller is absent or merely at an unexpected address.
uint8_t touch_scan_i2c();

// Brings up the controller and registers it as LVGL's pointer input device.
// Probes both candidate addresses rather than trusting either datasheet.
// Returns false if neither answered; the display still works in that case.
bool touch_begin();

// Quarter turns clockwise, 0-3, matching what was passed to display_begin().
// Applied to the reported coordinates, because rotating the image without
// rotating the touch mapping puts every tap in the wrong place.
void touch_set_orientation(uint8_t rotation);

// The address the controller actually answered on, or 0 if it never did.
uint8_t touch_address();

// Most recent touch position, and whether a finger is down right now. The
// bring-up screen uses this to prove the mirroring is correct; the real UI uses
// ordinary LVGL events.
bool touch_pressed(lv_point_t* point);

// CO5300 AMOLED panel over QSPI, wired to LVGL.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// Brings up the panel and registers it as LVGL's display. lv_init() must have
// been called first. Returns false if the draw buffer could not be allocated,
// in which case nothing has been registered with LVGL.
// `rotation` is quarter turns clockwise, 0-3, for mounting the Puck whichever way
// suits. The panel is square, so no resolution swap is needed — but the touch
// mapping must be rotated to match, or taps land somewhere else entirely.
bool display_begin(uint8_t rotation);

// 0..255. There is no backlight pin on this board — this is a panel command,
// which is why the panel is typed as Arduino_CO5300 rather than Arduino_GFX.
void display_set_brightness(uint8_t level);

// Where the draw buffer ended up, for the boot log: true if it landed in
// DMA-capable internal RAM, false if it fell back to PSRAM.
bool display_buffer_is_internal();

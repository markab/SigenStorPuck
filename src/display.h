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

// Puts the panel to sleep, or wakes it. Genuinely off — DISPOFF followed by
// SLPIN — not brightness 0.
//
// That is a different thing from the idle dim, and deliberately so. Dimming is
// for a display someone might glance at, where blanking would defeat the point
// of having it on the wall. This is for the hours nobody is in the room, where
// the only thing a lit panel is doing is spending its life: emission is what
// wears an AMOLED, and zero emission is the one mitigation that beats every
// sweep band and auto-cycle put together.
//
// Costs ~240 ms in the panel's own settling delays each way, which is why it is
// a scheduled transition and not something to toggle per frame. Waking repaints
// the whole screen: controller RAM is undefined across SLPIN/SLPOUT, though the
// register configuration — rotation, colour mode, the window offsets — survives,
// so no re-init is needed.
void display_set_sleep(bool asleep);

bool display_asleep();

// Where the draw buffer ended up, for the boot log: true if it landed in
// DMA-capable internal RAM, false if it fell back to PSRAM.
bool display_buffer_is_internal();

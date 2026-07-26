// A scannable QR code, used twice: on the settings screen and on the
// "Not configured" overlay.
//
// Both show the same thing — where this device's own settings page lives — so
// the card is built once here rather than twice with slightly different
// margins.

#pragma once

#include <lvgl.h>

// A QR card `modules_px` across plus its quiet zone, so the object returned is
// wider than `modules_px`. Starts hidden: there is nothing to encode until the
// device knows its own address.
lv_obj_t* puck_qr_block_create(lv_obj_t* parent, lv_coord_t modules_px);

// Re-encodes the card. Hides it if `url` is empty or will not fit at this size,
// because a QR that cannot be scanned is worse than an honest gap.
void puck_qr_block_set_url(lv_obj_t* card, const char* url);

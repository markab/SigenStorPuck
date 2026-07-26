// The last screen: how to reach this device's own settings page.
//
// The same QR code and addresses the "Not configured" overlay shows, kept
// available once the device *is* configured — otherwise the only way back to
// the settings page is to already know where it is.

#pragma once

#include <lvgl.h>

lv_obj_t* screen_settings_create(lv_obj_t* parent);

// `host` is the mDNS name and may be empty; `ip` is the dotted address. Both,
// because mDNS is not universally resolvable — see the note in main.cpp. Safe
// to call every refresh: unchanged addresses do not re-encode the QR.
void screen_settings_set_address(const char* host, const char* ip);

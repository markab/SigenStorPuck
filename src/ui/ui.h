// The whole user interface: the data screens in a horizontally-swiped tileview
// followed by the settings-address screen, with a page-dot indicator
// (docs/PLAN.md §B4).
//
// One entry point in and one snapshot in, so neither the device's main loop nor
// the simulator needs to know how many screens there are.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

// Builds the tileview and every screen under `parent`, returning the tileview.
//
// `with_server_screens` is false on the Modbus data source, which has neither
// the tariff behind the cost screen (PLAN.md §D1) nor the flow decomposition
// behind the flows screen. Everything downstream asks ui_screen_count() rather
// than assuming a number.
lv_obj_t* ui_create(lv_obj_t* parent, bool with_server_screens);

// Pushes a snapshot to every screen that exists. This is the single "here is a
// new reading" call PLAN.md §B3 asks for: the WebSocket path of §A4 can replace
// the poll loop later without any screen changing, and §D2's Modbus path
// already does.
void ui_update(const Snapshot& snapshot);

// A full-screen message covering the tileview: WiFi setup, "not configured",
// "re-enrol needed", "waiting for data". Pass nullptr as the title to hide it.
//
// An overlay rather than a screen of its own, because these states are not
// something to swipe to — they are the only thing worth showing while they last.
//
// `with_qr` adds the settings-page QR code from ui_set_address(), for the states
// whose instruction is "go to the settings page".
void ui_set_overlay(const char* title, const char* detail, bool with_qr = false);

// Where this device's own settings page lives, for the QR code and the address
// lines on the last screen. `host` is the mDNS name and may be empty; `ip` is
// the dotted address, empty until WiFi is up. Cheap to call every refresh.
void ui_set_address(const char* host, const char* ip);

// The Puck's own battery, as opposed to the house battery. Hidden unless the
// device is genuinely running on battery.
void ui_set_device_battery(bool show, int percent, bool charging);

// Fine rotation, in tenths of a degree, for a panel mounted a few degrees off a
// quarter turn. Applied on top of whatever whole quarter turn is in force.
//
// This is not free: LVGL renders the rotated content through an intermediate layer
// and resamples it, so text goes slightly soft and redraws cost more. 0 disables it
// completely and is the untouched path.
void ui_set_fine_rotation(int16_t tenths_of_a_degree);

// Auto-cycling through the screens, in seconds; 0 turns it off. Pauses while the
// screen is being touched so it never changes under your finger.
void ui_set_rotate_interval(uint32_t seconds);

// How often to run the sweep band, in minutes; 0 turns it off.
void ui_set_sweep_interval(uint32_t minutes);

// Screen navigation, for the simulator's keyboard and its screenshot pass. Touch
// swiping needs none of this — the tileview handles that itself.
int ui_screen_count();
int ui_current_screen();
void ui_show_screen(int index);

// The whole user interface: four screens in a horizontally-swiped tileview,
// with a page-dot indicator (docs/PLAN.md §B4).
//
// One entry point in and one snapshot in, so neither the device's main loop nor
// the simulator needs to know how many screens there are.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

// Builds the tileview and every screen under `parent`, returning the tileview.
lv_obj_t* ui_create(lv_obj_t* parent);

// Pushes a snapshot to all four screens. This is the single "here is a new
// reading" call PLAN.md §B3 asks for: the WebSocket path of §A4 can replace the
// poll loop later without any screen changing.
void ui_update(const Snapshot& snapshot);

// A full-screen message covering the tileview: WiFi setup, "not configured",
// "re-enrol needed", "waiting for data". Pass nullptr as the title to hide it.
//
// An overlay rather than a fifth screen, because these states are not something
// to swipe to — they are the only thing worth showing while they last.
void ui_set_overlay(const char* title, const char* detail);

// The Puck's own battery, as opposed to the house battery. Hidden unless the
// device is genuinely running on battery.
void ui_set_device_battery(bool show, int percent, bool charging);

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

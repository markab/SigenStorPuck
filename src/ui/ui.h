// The whole user interface: the data screens in a horizontally-swiped tileview
// followed by the settings-address screen, with a page-dot indicator
// (docs/PLAN.md §B4).
//
// One entry point in and one snapshot in, so neither the device's main loop nor
// the simulator needs to know how many screens there are.

#pragma once

#include <lvgl.h>

#include "snapshot.h"

// A screen's identity, fixed and independent of where it lands in the tileview.
//
// The settings masks are stored against these, so turning one screen off does
// not silently re-point another screen's bit at its neighbour. Never renumber
// them: a device's stored mask outlives any given firmware.
enum PuckScreen : uint8_t {
  PUCK_SCREEN_POWER = 0,
  PUCK_SCREEN_BATTERY,
  PUCK_SCREEN_SOLAR,
  PUCK_SCREEN_FLOWS,
  PUCK_SCREEN_COST,
  PUCK_SCREEN_SETTINGS,
  // Appended, not inserted. Load belongs next to solar when you swipe, but the
  // ids are what a device's stored mask is written against — giving it id 3 would
  // silently turn this into the flows screen on every Puck already in the field.
  // Swipe order is a separate list; see PUCK_SCREEN_ORDER.
  PUCK_SCREEN_LOAD,
  PUCK_SCREEN_COUNT,
};

// The order they are swiped through, which is not the order they are numbered.
// Settings stays last: it is the way back to the settings page, and a route out
// belongs at the end.
static constexpr PuckScreen PUCK_SCREEN_ORDER[PUCK_SCREEN_COUNT] = {
    PUCK_SCREEN_POWER, PUCK_SCREEN_BATTERY, PUCK_SCREEN_SOLAR, PUCK_SCREEN_LOAD,
    PUCK_SCREEN_FLOWS, PUCK_SCREEN_COST,    PUCK_SCREEN_SETTINGS,
};

// Screens the Modbus source cannot fill: the cost screen needs tariff tables
// (PLAN.md §D1) and the flows screen needs a decomposition the plant's daily
// counters cannot give.
static constexpr uint8_t PUCK_SERVER_ONLY_SCREENS =
    (1u << PUCK_SCREEN_FLOWS) | (1u << PUCK_SCREEN_COST);

struct UiConfig {
  // False on the Modbus data source, which removes the server-only screens
  // whatever the masks say.
  bool with_server_screens = true;
  // One bit per PuckScreen. The settings screen is built regardless of its bit:
  // it is the only route back to the settings page from the device itself.
  uint8_t visible = 0xFF;
  // Which screens the auto-cycle steps through. A screen worth swiping to is not
  // necessarily one worth parking on for minutes at a time.
  uint8_t rotate = 0xFF;
};

// Builds the tileview and the enabled screens under `parent`, returning the
// tileview. Everything downstream asks ui_screen_count() rather than assuming a
// number.
lv_obj_t* ui_create(lv_obj_t* parent, const UiConfig& config);

// Pushes a snapshot to every screen that exists. This is the single "here is a
// new reading" call PLAN.md §B3 asks for: the WebSocket path of §A4 can replace
// the poll loop later without any screen changing, and §D2's Modbus path
// already does.
void ui_update(const Snapshot& snapshot);

// The past day being viewed, when one is loaded. Screen 1 ignores it entirely —
// it is the live screen and stays live — and the day-oriented screens use it in
// place of the live reading. Call ui_clear_day() when there is no day loaded,
// which is also what an offset of zero means.
//
// What the server can and cannot date is worth knowing here: `today`, `cost` and
// `solar` follow the date, but `power`, `battery` and `alarms` are built from
// live registers and are always now. So the state of charge, the kW pills and
// the current tariff rate stay live even on a past day. The date indicator is
// what tells you which day the totals belong to.
void ui_update_day(const Snapshot& snapshot);
void ui_clear_day();

// A day has been asked for but has not arrived yet.
//
// The day-oriented screens blank to dashes and the indicator says LOADING,
// rather than falling back to the live reading. Falling back was the first
// attempt and it flashed *today's* figures under a past date every time you
// stepped between days — briefly, confidently and wrongly.
void ui_set_day_loading();

// A full-screen message covering the tileview: WiFi setup, "not configured",
// "re-enrol needed", "waiting for data". Pass nullptr as the title to hide it.
//
// An overlay rather than a screen of its own, because these states are not
// something to swipe to — they are the only thing worth showing while they last.
//
// `with_qr` adds the settings-page QR code from ui_set_address(), for the states
// whose instruction is "go to the settings page".
//
// `highlight` is the thing the reader has to act on — the setup network's name,
// the address to open — drawn large and bright, with `detail` demoted to the
// muted line that explains what to do with it. They used to be one string, which
// meant the name of the network you had to find was set in the same small grey as
// the words "Join the WiFi network" around it, on the one screen whose entire job
// is to hand over a value. Pass nullptr where there is no such value.
void ui_set_overlay(const char* title, const char* highlight, const char* detail,
                    bool with_qr = false);

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

// Screen navigation, for the simulator's keyboard, its screenshot pass and the
// BOOT button. Touch swiping needs none of this — the tileview handles that.
int ui_screen_count();
int ui_current_screen();
void ui_show_screen(int index);

// Advances to the next screen, wrapping. What BOOT's double press does.
void ui_next_screen();

// Which screen a tile index holds, for a caller that needs to know what it is
// looking at rather than where it is.
PuckScreen ui_screen_at(int index);

// Auto-cycling, toggled from the PWR button as well as configured in settings.
// The interval is what settings stores; this is the runtime on/off on top of it,
// so switching it back on resumes at the configured rate rather than needing one.
void ui_set_rotate_enabled(bool enabled);
bool ui_rotate_enabled();

// Which day the day-oriented screens are showing, in whole days back from today:
// 0 is today, -1 yesterday, down to -PUCK_MAX_DAYS_BACK. Screen 1 is live
// whatever this says.
//
// Refused, and left where it was, when day stepping is not available — see
// ui_set_day_stepping().
void ui_set_day_offset(int days_back);
int ui_day_offset();

// Whether stepping back a day is possible at all. False on the Modbus source,
// which has daily counters and no dated API behind them, so there is no past to
// step into. The buttons say so rather than moving an indicator over data that
// will never change.
void ui_set_day_stepping(bool available);
bool ui_day_stepping();

// Whether the screen currently shown is one whose figures belong to a day.
//
// False on screen 1, which is live, and on the settings screen, which shows an
// address. Those two carry no date, so stepping the day from them would move an
// indicator you cannot see — the buttons do nothing there instead.
bool ui_day_screen();

// A brief message across the top, for an action whose effect is not otherwise
// visible — toggling the auto-cycle, mostly. Clears itself.
void ui_toast(const char* text);

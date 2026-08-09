// Desktop simulator entry point (docs/PLAN.md §B5).
//
// Loads the canned /api/summary fixtures from test/fixtures and lets you step
// through them with the keyboard, so screen work happens in a one-second
// edit-run loop instead of a flash cycle.
//
// What it renders right now is a plain dump of the parsed Snapshot. That is
// deliberately not a design — it exists to prove the fixture -> parse -> LVGL
// path end to end, and to make the awkward states (null `today`, null
// registers, unconfigured cost) visible before any screen depends on them.

#include <dirent.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include "board_config.h"
#include "history.h"
#include "sim_backend.h"
#include "snapshot.h"
#include "ui/theme.h"
#include "ui/ui.h"

namespace {

constexpr uint32_t COLOUR_BACKGROUND = PUCK_COLOUR_BG;
constexpr uint32_t COLOUR_TEXT = PUCK_COLOUR_TEXT;
constexpr uint32_t COLOUR_MUTED = PUCK_COLOUR_MUTED;
constexpr uint32_t COLOUR_ACCENT = PUCK_COLOUR_BATTERY;
constexpr uint32_t COLOUR_WARN = PUCK_COLOUR_WARN;

std::vector<std::string> s_fixture_paths;
size_t s_current = 0;
int16_t s_startup_tilt = 0;

// --modbus previews the reduced arrangement that data source gets: no cost
// screen and no flows screen (§D1), which is otherwise only visible on a device
// configured for it.
bool s_with_server_screens = true;

// The real screen, plus the raw field dump kept behind the 'd' key. Keeping the
// dump around is worth its few lines: when a screen shows something surprising,
// it answers "is the parse wrong or the layout wrong?" immediately.
lv_obj_t* s_debug_root = nullptr;
bool s_show_debug = false;

lv_obj_t* s_heading = nullptr;
lv_obj_t* s_body = nullptr;

std::vector<std::string> discover_fixtures(const std::string& directory) {
  std::vector<std::string> found;
  DIR* dir = opendir(directory.c_str());
  if (dir == nullptr) {
    return found;
  }
  while (const dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
      found.push_back(directory + "/" + name);
    }
  }
  closedir(dir);
  // Numeric filename prefixes give a stable, meaningful order.
  std::sort(found.begin(), found.end());
  return found;
}

bool read_file(const std::string& path, std::string* out) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  out->clear();
  char buffer[512];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, read);
  }
  fclose(file);
  return true;
}

std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "--" for an unknown value, never "0.00". The whole reason MaybeFloat exists.
std::string number(const MaybeFloat& value, const char* unit, int decimals = 2) {
  if (!value.known) {
    return "--";
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.*f%s", decimals, value.value, unit);
  return buffer;
}

std::string number(const MaybeInt& value, const char* unit) {
  if (!value.known) {
    return "--";
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%d%s", value.value, unit);
  return buffer;
}

// Signed legs read much more clearly with the direction spelled out than with a
// minus sign the viewer has to interpret against the register conventions.
std::string signed_leg(const MaybeFloat& value, const char* positive, const char* negative) {
  std::string text = number(value, " kW");
  if (!value.known) {
    return text;
  }
  if (value.value > 0.0f) {
    text += "  ";
    text += positive;
  } else if (value.value < 0.0f) {
    text += "  ";
    text += negative;
  }
  return text;
}

std::string describe(const Snapshot& snapshot) {
  std::string text;
  char line[192];

  snprintf(line, sizeof(line), "v%d   %s   age %us   alarms %d\n\n", snapshot.version,
           snapshot.ok ? "ok" : "NOT ok", snapshot.age_s, snapshot.alarms);
  text += line;

  snprintf(line, sizeof(line), "PV     %s\n", number(snapshot.power.pv, " kW").c_str());
  text += line;
  if (snapshot.power.off_grid) {
    text += "GRID   OFF-GRID (leg suppressed)\n";
  } else {
    snprintf(line, sizeof(line), "GRID   %s\n",
             signed_leg(snapshot.power.grid, "import", "export").c_str());
    text += line;
  }
  snprintf(line, sizeof(line), "BATT   %s\n",
           signed_leg(snapshot.power.batt, "charge", "discharge").c_str());
  text += line;
  snprintf(line, sizeof(line), "HOME   %s\n", number(snapshot.power.home, " kW").c_str());
  text += line;
  snprintf(line, sizeof(line), "EV     %s\n\n", number(snapshot.power.ev, " kW").c_str());
  text += line;

  snprintf(line, sizeof(line), "SOC    %s      SOH %s\n",
           number(snapshot.battery.soc_pct, " %", 1).c_str(),
           number(snapshot.battery.soh_pct, " %", 1).c_str());
  text += line;
  snprintf(line, sizeof(line), "TEMP   %s     CAP %s\n",
           number(snapshot.battery.temp_c, " C", 1).c_str(),
           number(snapshot.battery.capacity_kwh, " kWh", 1).c_str());
  text += line;
  snprintf(line, sizeof(line), "ETA    %s\n\n", number(snapshot.battery.eta_min, " min").c_str());
  text += line;

  if (!snapshot.today.present) {
    text += "TODAY  null - day unreadable\n\n";
  } else {
    snprintf(line, sizeof(line), "TODAY  solar %s  imp %s  exp %s\n",
             number(snapshot.today.solar, "", 1).c_str(),
             number(snapshot.today.imported, "", 1).c_str(),
             number(snapshot.today.exported, "", 1).c_str());
    text += line;
    snprintf(line, sizeof(line), "       load %s  chg %s  dis %s\n\n",
             number(snapshot.today.load, "", 1).c_str(),
             number(snapshot.today.charge, "", 1).c_str(),
             number(snapshot.today.discharge, "", 1).c_str());
    text += line;
  }

  if (!snapshot.solar.configured) {
    text += "SOLAR  no forecast configured\n\n";
  } else {
    snprintf(line, sizeof(line), "SOLAR  fc %s  left %s  vs %s  peak %s\n\n",
             number(snapshot.solar.forecast_kwh, "", 1).c_str(),
             number(snapshot.solar.remaining_kwh, "", 1).c_str(),
             number(snapshot.solar.vs_forecast_pct, "%", 0).c_str(),
             number(snapshot.solar.peak_kw, "", 1).c_str());
    text += line;
  }

  if (!snapshot.cost.configured) {
    text += "COST   no tariff configured\n";
  } else {
    snprintf(line, sizeof(line), "COST   save %s GBP    now %s\n",
             number(snapshot.cost.saving_gbp, "").c_str(),
             number(snapshot.cost.rate_p, "p", 1).c_str());
    text += line;
    text += "NEXT   ";
    for (size_t i = 0; i < snapshot.cost.next_count; ++i) {
      snprintf(line, sizeof(line), "%.1fp  ", snapshot.cost.next[i].pence);
      text += line;
    }
    text += "\n";
  }

  return text;
}

// --------------------------------------------------- synthetic day history ---
//
// The fixtures are single snapshots, so there is nothing for the charts of §D3
// to draw. Rather than grow the fixture format, each one is expanded into a
// plausible day that *ends* at its own instantaneous values.
//
// The point of this is the awkward case, not the pretty one: a solar curve
// broken up by cloud is what the min/max envelope exists to render, and a smooth
// bell would let a bug in the reduction pass unnoticed.

float hash01(uint32_t value) {
  uint32_t x = value * 2654435761u;
  x ^= x >> 13;
  x *= 1274126177u;
  x ^= x >> 16;
  return static_cast<float>(x & 0xFFFFu) / 65535.0f;
}

// Normalised generation, 0 at night and peaking near the middle of the day. The
// exponent narrows the bell slightly, which is closer to a real array than a
// plain sine.
float solar_shape(int local_minute) {
  constexpr int SUNRISE = 330;   // 05:30
  constexpr int SUNSET = 1230;   // 20:30
  if (local_minute <= SUNRISE || local_minute >= SUNSET) {
    return 0.0f;
  }
  const float t = static_cast<float>(local_minute - SUNRISE) /
                  static_cast<float>(SUNSET - SUNRISE);
  return powf(sinf(t * 3.14159265f), 1.6f);
}

// Cloud, in bands rather than uniformly, so part of the day stays clean and the
// two cases can be compared in one screenshot.
float cloud_factor(uint32_t minute, int local_minute) {
  const float season = sinf(static_cast<float>(local_minute) * 0.011f);
  if (season < 0.25f) {
    return 1.0f;  // clear spell
  }
  const float roll = hash01(minute);
  if (roll > 0.45f) {
    return 1.0f;
  }
  // A dip to somewhere between a fifth and most of clear-sky output.
  return 0.2f + 0.7f * hash01(minute + 7777u);
}

// A day's worth of battery: drained overnight, charged through the middle of the
// day, drawn down again in the evening.
float soc_shape(int local_minute) {
  const float t = static_cast<float>(local_minute) / 1440.0f * 2.0f * 3.14159265f;
  return -cosf(t - 1.2f);  // -1 around 06:00, +1 around 18:00
}

void synthesise_history(const Snapshot& snapshot) {
  history_reset(HistoryBank::Live);
  if (!snapshot.valid || snapshot.ts == 0) {
    return;
  }

  // UTC, so the fixtures' own 13:00 timestamp is the time of day. Setting it at
  // all is what puts the charts on their anchored-to-midnight path rather than
  // the rolling fallback, which is the arrangement worth looking at.
  history_set_timezone(HistoryBank::Live, 0);

  const uint32_t end = snapshot.ts / 60;
  const int end_local = static_cast<int>(end % 1440);

  // Scale the bell so it passes exactly through the live reading. A fixture
  // reporting nothing at midday gets an overcast day instead of a bright one it
  // would contradict.
  const float shape_now = solar_shape(end_local);
  const bool generating = snapshot.power.pv.known && snapshot.power.pv.value > 0.05f;
  const bool overcast = snapshot.power.pv.known && !generating;
  float peak = 4.2f;
  if (generating && shape_now > 0.05f) {
    peak = snapshot.power.pv.value / shape_now;
  } else if (overcast) {
    peak = 0.9f;
  }

  // State of charge: build the shape, then shift the whole series so its last
  // sample is the reading actually on screen.
  constexpr float SOC_AMPLITUDE = 28.0f;
  const float soc_offset = snapshot.battery.soc_pct.known
                               ? snapshot.battery.soc_pct.value -
                                     SOC_AMPLITUDE * soc_shape(end_local)
                               : 0.0f;

  // From local midnight to now, not a full 24 h back: the anchored window covers
  // the whole day, so filling only the part that has happened is what makes the
  // curve grow into an empty right-hand side the way it will on a real device.
  for (int local = 0; local <= end_local; ++local) {
    const uint32_t minute = end - static_cast<uint32_t>(end_local - local);

    // A null-register fixture must leave the band genuinely empty — that is the
    // case the "nothing recorded yet" path exists for.
    if (snapshot.power.pv.known) {
      const float cloud = overcast ? 0.35f + 0.5f * hash01(minute)
                                   : cloud_factor(minute, local);
      history_put(HistoryBank::Live, HistorySeries::Pv, minute, peak * solar_shape(local) * cloud);
    }

    // House load: a base draw with a morning and an evening hump, plus the odd
    // kettle. Spiky on purpose — a flat line would not exercise the smoothing the
    // load chart needs, which is the whole reason its window is wider.
    if (snapshot.power.home.known) {
      const float morning = expf(-powf((local - 450) / 90.0f, 2.0f));
      const float evening = expf(-powf((local - 1140) / 120.0f, 2.0f));
      float load = 0.35f + 1.4f * morning + 2.2f * evening;
      if (hash01(minute + 31337u) > 0.94f) {
        load += 2.5f;  // kettle
      }
      history_put(HistoryBank::Live, HistorySeries::Load, minute, load);
    }

    if (snapshot.battery.soc_pct.known) {
      float soc = soc_offset + SOC_AMPLITUDE * soc_shape(local);
      if (soc < 2.0f) {
        soc = 2.0f;
      }
      if (soc > 100.0f) {
        soc = 100.0f;
      }
      history_put(HistoryBank::Live, HistorySeries::Soc, minute, soc);
    }
  }
}

void show_current_fixture() {
  const std::string& path = s_fixture_paths[s_current];
  const std::string name = base_name(path);

  char heading[128];
  snprintf(heading, sizeof(heading), "%s   [%zu/%zu]", name.c_str(), s_current + 1,
           s_fixture_paths.size());
  lv_label_set_text(s_heading, heading);

  std::string json;
  if (!read_file(path, &json)) {
    lv_label_set_text(s_body, "could not read the fixture");
    return;
  }

  Snapshot snapshot;
  if (!snapshot_parse(json.c_str(), json.size(), &snapshot)) {
    lv_label_set_text(s_body, "fixture did not parse");
    lv_obj_set_style_text_color(s_body, lv_color_hex(COLOUR_WARN), LV_PART_MAIN);
    ui_update(Snapshot{});
    return;
  }

  lv_obj_set_style_text_color(s_body, lv_color_hex(COLOUR_TEXT), LV_PART_MAIN);
  lv_label_set_text(s_body, describe(snapshot).c_str());
  // Rebuilt per fixture, so stepping between them does not leave the previous
  // fixture's day behind the new one's numbers.
  synthesise_history(snapshot);
  ui_update(snapshot);
  if (ui_day_offset() != 0) {
    ui_update_day(snapshot);
  }

  const std::string title = "SigenStorPuck sim - " + name;
  sim_backend_set_title(title.c_str());
  printf("[sim] %s (%zu bytes)\n", name.c_str(), json.size());
}

void build_debug_view() {
  lv_obj_t* screen = s_debug_root;

  // Same bezel guide as the device's bring-up screen, so it stays obvious that
  // the usable area is a circle and not a 466 px square.
  lv_obj_t* bezel = lv_obj_create(screen);
  lv_obj_set_size(bezel, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_center(bezel);
  lv_obj_set_style_radius(bezel, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bezel, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(bezel, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(bezel, lv_color_hex(COLOUR_MUTED), LV_PART_MAIN);
  lv_obj_clear_flag(bezel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bezel, LV_OBJ_FLAG_CLICKABLE);

  s_heading = lv_label_create(screen);
  lv_obj_set_style_text_font(s_heading, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_heading, lv_color_hex(COLOUR_ACCENT), LV_PART_MAIN);
  lv_obj_align(s_heading, LV_ALIGN_TOP_MID, 0, 44);

  s_body = lv_label_create(screen);
  lv_obj_set_style_text_font(s_body, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_body, lv_color_hex(COLOUR_TEXT), LV_PART_MAIN);
  lv_obj_set_width(s_body, PUCK_SAFE_SQUARE);
  lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 70);

  lv_obj_t* hint = lv_label_create(screen);
  lv_obj_set_style_text_font(hint, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(COLOUR_MUTED), LV_PART_MAIN);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(hint,
                    "n/p fixture   [ ] screen   , . tilt   - = day\n"
                    "r auto-cycle   o overlay   d dump   esc quit");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -34);
}

// Builds both views once and swaps visibility, rather than tearing down and
// rebuilding a widget tree on every keypress.
void build_views() {
  lv_obj_t* screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(COLOUR_BACKGROUND), LV_PART_MAIN);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  UiConfig config;
  config.with_server_screens = s_with_server_screens;
  ui_create(screen, config);

  // A stand-in for what net_hostname()/net_ip() return on the device, so the
  // settings screen and its QR code render here rather than only on hardware.
  ui_set_address("sigenstorpuck.local", "192.168.1.100");

  s_debug_root = lv_obj_create(screen);
  lv_obj_remove_style_all(s_debug_root);
  lv_obj_set_size(s_debug_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_center(s_debug_root);
  lv_obj_set_style_bg_color(s_debug_root, lv_color_hex(COLOUR_BACKGROUND), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_debug_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_debug_root, LV_OBJ_FLAG_SCROLLABLE);

  build_debug_view();
  lv_obj_add_flag(s_debug_root, LV_OBJ_FLAG_HIDDEN);
}

void apply_view_visibility() {
  if (s_show_debug) {
    lv_obj_clear_flag(s_debug_root, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_debug_root, LV_OBJ_FLAG_HIDDEN);
  }
}

// The overlays are driven by device state — WiFi, provisioning, a revoked token
// — none of which a fixture can express. Cycling them from the keyboard is the
// only way to lay them out without flashing a deliberately broken device.
void cycle_overlay() {
  static int which = 0;
  which = (which + 1) % 3;
  switch (which) {
    case 1:
      ui_set_overlay("Not configured",
                     "Scan, or open http://sigenstorpuck.local/\n"
                     "or http://192.168.1.100/\n"
                     "and paste the enrolment URL",
                     true);
      break;
    case 2:
      ui_set_overlay("WiFi setup", "Join the WiFi network\nSigenStorPuck setup");
      break;
    default:
      ui_set_overlay(nullptr, nullptr);
      break;
  }
}

void handle_key(int key) {
  const size_t count = s_fixture_paths.size();
  if (key == 'o') {
    cycle_overlay();
    return;
  }
  if (key == 'd') {
    s_show_debug = !s_show_debug;
    apply_view_visibility();
    return;
  }
  // Fine rotation, for judging the resampling cost before it reaches hardware.
  if (key == ',' || key == '.') {
    static int16_t tenths = 0;
    tenths += (key == '.') ? 5 : -5;
    if (tenths > 300) tenths = 300;
    if (tenths < -300) tenths = -300;
    ui_set_fine_rotation(tenths);
    printf("[sim] fine rotation %.1f degrees\n", tenths / 10.0);
    return;
  }
  // The day indicator and the toast, which the buttons drive on hardware and no
  // fixture can express.
  if (key == '-' || key == '=') {
    ui_set_day_offset(ui_day_offset() + (key == '=' ? 1 : -1));
    // The device fetches a dated payload for a past day; here the current fixture
    // stands in for it, so the loaded state is what gets drawn. Shift-stepping
    // shows the state before it arrives instead.
    if (ui_day_offset() == 0) {
      ui_clear_day();
    } else {
      show_current_fixture();
    }
    return;
  }
  if (key == '_' || key == '+') {
    ui_set_day_offset(ui_day_offset() + (key == '+' ? 1 : -1));
    if (ui_day_offset() == 0) {
      ui_clear_day();
    } else {
      ui_set_day_loading();
    }
    return;
  }
  if (key == 'r') {
    const bool on = !ui_rotate_enabled();
    ui_set_rotate_enabled(on);
    ui_toast(on ? "CYCLE ON" : "CYCLE OFF");
    return;
  }
  if (key == ']' || key == '[') {
    const int count = ui_screen_count();
    const int step = key == ']' ? 1 : count - 1;
    ui_show_screen((ui_current_screen() + step) % count);
    return;
  }
  if (key == 'n' || key == ' ') {
    s_current = (s_current + 1) % count;
  } else if (key == 'p') {
    s_current = (s_current + count - 1) % count;
  } else if (key >= '1' && key <= '9') {
    const size_t requested = static_cast<size_t>(key - '1');
    if (requested >= count) {
      return;
    }
    s_current = requested;
  } else {
    return;
  }
  show_current_fixture();
}

}  // namespace

// Renders every fixture once and writes each to a BMP, then exits. Non
// interactive, so a screen can be checked against all the awkward states in one
// command with nobody watching the window.
int run_screenshot_pass(const std::string& output_directory) {
  int failures = 0;
  for (int screen = 0; screen < ui_screen_count(); ++screen) {
    ui_show_screen(screen);
    for (s_current = 0; s_current < s_fixture_paths.size(); ++s_current) {
      show_current_fixture();

      // Let the flow animation run briefly before capturing, so the shot shows
      // dots mid-travel rather than all bunched at the start of their path.
      for (int frame = 0; frame < 12; ++frame) {
        sim_backend_delay(20);
        lv_timer_handler();
      }
      lv_refr_now(nullptr);

      const std::string name = base_name(s_fixture_paths[s_current]);
      const std::string stem = name.substr(0, name.find_last_of('.'));
      char prefix[16];
      snprintf(prefix, sizeof(prefix), "s%d_", screen + 1);
      const std::string path = output_directory + "/" + prefix + stem + ".bmp";
      if (sim_backend_save_bmp(path.c_str())) {
        printf("[sim] wrote %s\n", path.c_str());
      } else {
        ++failures;
      }
    }
  }

  // The overlays are not screens, but they are full-screen layouts with a QR
  // code and three lines of text to fit on a circle — exactly the thing this
  // pass exists to check. Once each, over whichever fixture is loaded.
  for (int i = 1; i <= 2; ++i) {
    cycle_overlay();
    lv_refr_now(nullptr);
    char path[512];
    snprintf(path, sizeof(path), "%s/overlay_%d.bmp", output_directory.c_str(), i);
    if (sim_backend_save_bmp(path)) {
      printf("[sim] wrote %s\n", path);
    } else {
      ++failures;
    }
  }
  cycle_overlay();

  // The day indicator and the toast, which the buttons drive on hardware and no
  // fixture can express — same reasoning as the overlays above.
  ui_show_screen(1);
  // Back to a real fixture first: the loop above leaves s_current one past the
  // end, and show_current_fixture() would index out of bounds, fail to read and
  // return before feeding the UI anything — leaving these captures showing
  // whatever was last drawn.
  s_current = 0;
  // Offset before the fixture, so show_current_fixture() feeds the day as well
  // as the live reading and the screens draw the loaded state, not the pending
  // one.
  ui_set_day_offset(-2);
  show_current_fixture();
  lv_refr_now(nullptr);
  {
    const std::string path = output_directory + "/day_back.bmp";
    if (sim_backend_save_bmp(path.c_str())) {
      printf("[sim] wrote %s\n", path.c_str());
    } else {
      ++failures;
    }
  }
  ui_set_day_loading();
  lv_refr_now(nullptr);
  {
    const std::string path = output_directory + "/day_loading.bmp";
    if (sim_backend_save_bmp(path.c_str())) {
      printf("[sim] wrote %s\n", path.c_str());
    } else {
      ++failures;
    }
  }

  // Back to a loaded day, then a message over it: a transient toast owns the top
  // slot while it lasts, so this has to come after the loading capture or it
  // would be the thing on screen for it.
  show_current_fixture();
  ui_toast("CYCLE OFF");
  lv_refr_now(nullptr);
  {
    const std::string path = output_directory + "/toast.bmp";
    if (sim_backend_save_bmp(path.c_str())) {
      printf("[sim] wrote %s\n", path.c_str());
    } else {
      ++failures;
    }
  }

  ui_set_day_offset(0);
  ui_clear_day();

  // How close LV_MEM_SIZE is to its limit, after a pass that has built every
  // screen and drawn every fixture through them — which is as hard as the pool
  // is ever worked. Reported here because the failure mode is not a clear "pool
  // full": it surfaces as whatever allocates next, and the sixth screen turned
  // it into an out-of-memory assert inside the anti-aliased corner mask.
  lv_mem_monitor_t mem;
  lv_mem_monitor(&mem);
  printf("[sim] LVGL pool: %u%% of %u KB used, %u%% fragmented, largest free block %u B\n",
         mem.used_pct, static_cast<unsigned>(mem.total_size / 1024), mem.frag_pct,
         static_cast<unsigned>(mem.free_biggest_size));

  return failures;
}

// src/sim/selftest.cpp — checks for the shared Modbus and history code, which
// needs no display and so runs before any of the LVGL setup below.
int run_selftest();

int main(int argc, char** argv) {
  // sim [fixture-dir] [--shot output-dir] [--selftest] [--modbus]
  std::string directory = "test/fixtures";
  std::string shot_directory;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--selftest") {
      return run_selftest();
    } else if (argument == "--modbus") {
      s_with_server_screens = false;
    } else if (argument == "--shot" && i + 1 < argc) {
      shot_directory = argv[++i];
    } else if (argument == "--tilt" && i + 1 < argc) {
      s_startup_tilt = static_cast<int16_t>(atoi(argv[++i]));
    } else {
      directory = argument;
    }
  }

  s_fixture_paths = discover_fixtures(directory);
  if (s_fixture_paths.empty()) {
    fprintf(stderr,
            "[sim] no .json fixtures in '%s'\n"
            "      run from the repo root, or pass a directory as the first argument\n",
            directory.c_str());
    return 1;
  }
  printf("[sim] %zu fixtures in %s\n", s_fixture_paths.size(), directory.c_str());

  lv_init();
  if (!sim_backend_begin()) {
    return 1;
  }

  build_views();
  if (s_startup_tilt != 0) {
    ui_set_fine_rotation(s_startup_tilt);
  }

  if (!shot_directory.empty()) {
    const int failures = run_screenshot_pass(shot_directory);
    sim_backend_end();
    return failures == 0 ? 0 : 1;
  }

  show_current_fixture();

  while (sim_backend_pump()) {
    while (const int key = sim_backend_take_key()) {
      handle_key(key);
    }
    lv_timer_handler();
    // Roughly LV_DISP_DEF_REFR_PERIOD; the renderer's vsync does the real pacing.
    sim_backend_delay(5);
  }

  sim_backend_end();
  return 0;
}

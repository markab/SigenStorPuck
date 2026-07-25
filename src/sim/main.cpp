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
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include "board_config.h"
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
  ui_update(snapshot);

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
  lv_label_set_text(hint, "n/p fixture  [ ] screen  , . tilt  d dump  esc quit");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);
}

// Builds both views once and swaps visibility, rather than tearing down and
// rebuilding a widget tree on every keypress.
void build_views() {
  lv_obj_t* screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(COLOUR_BACKGROUND), LV_PART_MAIN);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  ui_create(screen);

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

void handle_key(int key) {
  const size_t count = s_fixture_paths.size();
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
  return failures;
}

int main(int argc, char** argv) {
  // sim [fixture-dir] [--shot output-dir]
  std::string directory = "test/fixtures";
  std::string shot_directory;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--shot" && i + 1 < argc) {
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

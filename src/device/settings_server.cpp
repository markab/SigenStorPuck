#include "settings_server.h"

#include <WebServer.h>

#include "board_config.h"
#include "enrol_url.h"
#include "modbus_api.h"
#include "net.h"
#include "poller.h"
#include "power.h"
#include "settings.h"
#include "sigen_api.h"
#include "updater.h"
#include "touch.h"
#include "ui/ui.h"

#include <math.h>

namespace {

WebServer s_server(80);
bool s_running = false;

// Minimal inline CSS: the page has to work from a phone on the sofa with no
// network access beyond the LAN, so nothing may be fetched from a CDN.
const char* PAGE_STYLE =
    "body{font-family:system-ui,sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem;"
    "background:#111;color:#eee}"
    "h1{font-size:1.3rem}"
    // A hairline above every heading. Sections are runs of sibling elements rather
    // than wrapped blocks, so the heading is the only place a divider can go without
    // adding a div around each of the seven sections. #333 is the secondary-button
    // grey already in use, so it reads as part of the palette rather than a new
    // colour. Uniform, which also separates the header block from the first section.
    "h2{font-size:1rem;margin-top:2.2rem;padding-top:1.2rem;"
    "border-top:1px solid #333;color:#9ab}"
    // `select` belongs here with the text fields. Left out, it falls back to
    // inline flow and lands on the same line as its own label, which is what put
    // the Type dropdown on top of the labels either side of it.
    "input,select,button{font-size:1rem;padding:.5rem;width:100%;box-sizing:border-box;"
    "margin:.25rem 0 1rem;border-radius:.4rem;border:1px solid #444;background:#1c1c1e;color:#eee}"
    "button{background:#0a84ff;border:0;font-weight:600;cursor:pointer}"
    "button.secondary{background:#333}"
    // Tick boxes are not text fields, and the rule above would stretch one to the
    // full width of its column — a 528 px radio button with its text stranded off
    // to the right.
    "input[type=checkbox],input[type=radio]{width:auto;flex:0 0 auto;margin:0;padding:0}"
    "table.screens{width:100%;border-collapse:collapse;margin:.4rem 0}"
    "table.screens th{font-size:.8rem;font-weight:500;opacity:.7;text-align:center}"
    "table.screens th:first-child{text-align:left}"
    "table.screens td{padding:.25rem 0}"
    "table.screens td:not(:first-child){text-align:center;width:5.5rem}"
    "label{font-size:.85rem;color:#9ab}"
    // A whole row you can click, for the controls whose label sits beside them
    // rather than above them.
    "label.opt{display:flex;align-items:center;gap:.5rem;font-size:1rem;color:#eee;margin:.4rem 0}"
    // Matches a text field's height so a checkbox lines up with the inputs either
    // side of it instead of floating at the top of the cell.
    ".check{display:flex;align-items:center;gap:.5rem;padding:.5rem 0;margin:.25rem 0 1rem}"
    // align-items:end keeps the controls on one line when a label wraps and its
    // neighbours do not — "Plant address" takes two lines in a third of a 375 px
    // phone, and without this its input alone sits 19 px lower than the rest.
    // Bottom-aligning fixes that for any label at any width, rather than tuning
    // one string until it happens to fit.
    ".row{display:flex;gap:.75rem;align-items:end}.row>*{flex:1}.row>.wide{flex:2}"
    "code{background:#000;padding:.15rem .35rem;border-radius:.25rem}"
    ".ok{color:#30d158}.bad{color:#ff9f0a}"
    "p.hint{font-size:.8rem;color:#888;margin-top:-.6rem}"
    // The negative margin above is what makes a hint hug the field it describes.
    // A hint that follows a group of controls rather than a single one needs the
    // opposite, or it clings to the last label and looks like part of it.
    "p.hint.gap{margin-top:.6rem}";

String escape_html(const String& text) {
  String out;
  out.reserve(text.length() + 8);
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String page(const String& message, bool message_is_error) {
  const Settings& settings = settings_get();
  const PollStatus status = poller_status();

  // Kicked off before anything is rendered, so the answer is usually waiting by
  // the time the page refreshes itself a couple of seconds later. Returns
  // immediately — the check runs on its own task, because this handler shares a
  // thread with LVGL and a blocking HTTPS round trip here would freeze the
  // display.
  updater_request_check(/*force=*/false);
  const UpdateStatus update = updater_status();

  String html;
  html.reserve(4096);
  html += "<!doctype html><html><head><meta charset=utf-8>";
  html += "<meta name=viewport content='width=device-width,initial-scale=1'>";
  // Come back for the answer, but only while there is one coming. Any other state
  // is final, so the page settles instead of reloading under you while you type.
  if (update.state == UpdateState::Checking || update.state == UpdateState::Downloading) {
    html += "<meta http-equiv=refresh content=3>";
  }
  html += "<title>SigenStorPuck</title><style>";
  html += PAGE_STYLE;
  html += "</style></head><body>";
  html += "<h1>SigenStorPuck</h1>";
  html += "<p class=hint>firmware " PUCK_FW_VERSION " &middot; ";
  html += escape_html(net_ip());
  html += "</p>";

  // The device's own condition, as header context rather than a section of its own.
  // Neither line is about reaching the server — the clock exists so TLS can be
  // validated, and this is the Puck's battery, not the house's — so they belong
  // beside the version and the address rather than next to the poll result.
  html += "<p class=hint>Clock: ";
  html += net_time_synced() ? "set from NTP" : "not set (HTTPS will fail)";
  const PowerStatus power = power_status();
  html += " &middot; power: ";
  if (!power.pmic_ok) {
    html += "PMIC not detected";
  } else if (!power.battery_present) {
    html += power.usb_present ? "USB, no battery fitted" : "no battery fitted";
  } else {
    if (power.percent >= 0) {
      html += String(power.percent) + "% (" + power.millivolts + " mV)";
    } else {
      html += String(power.millivolts) + " mV (gauge not reporting)";
    }
    html += power.charging ? ", charging" : (power.usb_present ? ", on USB" : ", on battery");
  }
  html += "</p>";

  if (message.length() > 0) {
    html += "<p class=";
    html += message_is_error ? "bad" : "ok";
    html += ">";
    html += escape_html(message);
    html += "</p>";
  }

  // First, because it is what tells you which Puck you are looking at — worth
  // settling before anything else when two of them are being set up together.
  html += "<h2>Network</h2><form method=post action=/hostname>";
  html += "<label for=host>Device name</label>";
  html += "<input id=host name=host maxlength=32 value='";
  html += escape_html(settings.hostname);
  html += "' placeholder='sigenstorpuck'>";
  html += "<p class=hint>Reachable at <code>";
  html += escape_html(settings.hostname);
  html += ".local</code> and at <code>";
  html += escape_html(net_ip());
  // Worth stating plainly: the name on this page is the stored one, which is not
  // necessarily the one answering right now.
  //
  // The old hint also spelled out "Letters, digits and hyphens", which read as a
  // riddle to anyone who had not just written settings_clean_hostname(). The
  // constraint still holds — that function strips anything else — and the reader
  // still finds out, because saving reports the name it actually stored.
  html += "</code>.<br>Takes effect after a restart.</p>";
  html += "<button type=submit>Save</button></form>";

  // Both sources are configurable whichever one is running, so the other can be
  // set up before switching to it — a device that had to be switched first and
  // configured second would spend the gap unable to reach anything.
  const bool modbus = settings.source == DataSource::Modbus;
  html += "<h2>Data source</h2><form method=post action=/source>";
  html += "<label class=opt><input type=radio name=src value=server";
  html += modbus ? "" : " checked";
  html += "> SigenStor Display server</label>";
  html += "<label class=opt><input type=radio name=src value=modbus";
  html += modbus ? " checked" : "";
  html += "> Plant over Modbus (LAN only)</label>";
  html += "<p class='hint gap'>Takes effect after a restart.</p>";
  html += "<button type=submit>Save</button></form>";
  html += "<h2>Server</h2>";
  if (modbus) {
    html += "<p class=hint>Not in use: the data source is set to Modbus.</p>";
  }
  html += "<form method=post action=/save>";
  html += "<label for=enrol>Paste the enrolment URL from Admin &rarr; Kiosk devices</label>";
  html += "<input id=enrol name=enrol placeholder='https://host/kiosk-enroll?token=...'>";
  html += "<p class=hint>Or fill the two fields below instead.</p>";
  html += "<label for=base>Server URL</label>";
  html += "<input id=base name=base value='";
  html += escape_html(settings.base_url);
  html += "' placeholder='http://192.168.1.10:8000'>";
  html += "<label for=token>Kiosk token</label>";
  // Never the real token: the page shows only its last four characters so you can
  // tell one device's enrolment from another. Echoing a working credential back
  // into a page — or a browser history, or a screenshot — is not worth the
  // convenience of an editable field.
  html += "<input id=token name=token placeholder='currently ";
  html += escape_html(settings_token_masked());
  html += "'>";
  html += "<p class=hint>Leave blank to keep the stored token.</p>";
  html += "<button type=submit>Save</button></form>";

  // Between the two buttons, which is where it is wanted: pressing Test connection
  // and then scrolling three sections down to find out what happened was the whole
  // problem with keeping this in a Status section of its own.
  html += "<p class='hint gap'>Last poll: <code>";
  html += fetch_result_name(status.last_result);
  html += "</code>";
  if (status.last_http_status != 0) {
    html += " (HTTP ";
    html += status.last_http_status;
    html += ")";
  }
  if (status.consecutive_failures > 0) {
    // Only when there are any. A permanent "0" is noise directly under a button.
    html += ", ";
    html += status.consecutive_failures;
    html += " consecutive failures";
  }
  html += "</p>";

  html += "<form method=post action=/test><button class=secondary type=submit>";
  html += "Test connection</button></form>";

  html += "<h2>Plant (Modbus)</h2>";
  if (!modbus) {
    html += "<p class=hint>Not in use: the data source is set to the server.</p>";
  }
  html += "<form method=post action=/modbus><div class=row>";
  // An IP address needs about twice the room a port or an address does.
  html += "<div class=wide><label for=mbhost>Gateway or inverter IP</label>";
  html += "<input id=mbhost name=mbhost value='";
  html += escape_html(settings.modbus_host);
  html += "' placeholder='192.168.1.50'></div>";
  html += "<div><label for=mbport>Port</label>";
  html += "<input id=mbport name=mbport type=number min=1 max=65535 value=";
  html += settings.modbus_port;
  html += "></div>";
  html += "<div><label for=mbplant>Plant address</label>";
  html += "<input id=mbplant name=mbplant type=number min=1 max=247 value=";
  html += settings.modbus_plant_address;
  html += "></div></div>";
  // Enable Modbus TCP for this device's IP in the Sigen app: access is
  // whitelisted per client address, and a Puck that has not been added simply
  // gets no answer.
  html += "<p class=hint>Enable Modbus TCP for this device's IP in the Sigen app, "
          "and give it a reserved DHCP lease.</p>";
  html += "<p class=hint>Devices are optional — the plant address alone fills the "
          "power and battery screens. Add an inverter for battery temperature and "
          "daily totals, a charger for EV power. Slave ID 0 means the slot is unused.</p>";
  for (size_t i = 0; i < SETTINGS_MAX_MODBUS_DEVICES; ++i) {
    const ModbusDevice& device = settings.modbus_devices[i];
    const String suffix(static_cast<unsigned>(i));
    html += "<div class=row><div><label for=mbid" + suffix + ">Slave ID</label>";
    html += "<input id=mbid" + suffix + " name=mbid" + suffix +
            " type=number min=0 max=246 value=";
    html += device.slave_id;
    html += "></div><div><label for=mbtype" + suffix + ">Type</label>";
    html += "<select id=mbtype" + suffix + " name=mbtype" + suffix + ">";
    // One word each. Three columns on a 375 px phone leave about 66 px of text
    // room in the dropdown, and "Hybrid inverter" needs 104 — the browser would
    // silently clip it to something unreadable.
    html += "<option value=inv";
    html += device.type == ModbusDeviceType::Inverter ? " selected" : "";
    html += ">Inverter</option><option value=acc";
    html += device.type == ModbusDeviceType::AcCharger ? " selected" : "";
    html += ">Charger</option></select></div>";
    html += "<div><label for=mbdc" + suffix + ">DC charger</label>";
    html += "<div class=check><input id=mbdc" + suffix + " name=mbdc" + suffix +
            " type=checkbox value=1";
    html += device.dc_charger ? " checked" : "";
    html += "> fitted</div></div></div>";
  }
  html += "<button type=submit>Save</button></form>";

  html += "<h2>Display</h2><form method=post action=/display><div class=row>";
  html += "<div><label for=bright>Brightness (0-255)</label><input id=bright name=bright type=number min=10 max=255 value=";
  html += settings.brightness;
  html += "></div>";
  html += "<div><label for=poll>Poll interval (s)</label><input id=poll name=poll type=number min=2 max=300 value=";
  html += settings.poll_interval_s;
  html += "></div></div><div class=row>";
  html += "<div><label for=dim>Dim after (s, 0 = never)</label><input id=dim name=dim type=number min=0 max=3600 value=";
  html += settings.dim_after_s;
  html += "></div>";
  html += "<div><label for=dimb>Dimmed brightness</label><input id=dimb name=dimb type=number min=1 max=255 value=";
  html += settings.dim_brightness;
  html += "></div></div>";
  // No fine-rotation field yet: the drawing mechanism for it is blocked (see
  // ui_set_fine_rotation). Offering a control that stores a value and changes
  // nothing on screen would be worse than not offering it.
  html += "<label for=orient>Orientation (quarter turns, applies on restart)</label>";
  html += "<input id=orient name=orient type=number min=0 max=3 value=";
  html += settings.orientation;
  html += ">";
  // The switch and the interval are separate controls on purpose: turning the
  // auto-cycle off by zeroing its interval loses the rate it was set to, and
  // finding it again is a job nobody wants after switching it back on. The PWR
  // button's double press writes the same setting, so the two agree.
  html += "<label class=opt><input type=checkbox name=roten value=1";
  if (settings.rotate_enabled) {
    html += " checked";
  }
  html += "> Auto-cycle screens</label>";
  html += "<div class=row>";
  html += "<div><label for=rot>Auto-cycle every (s, 0 = off)</label><input id=rot name=rot type=number min=0 max=3600 value=";
  html += settings.rotate_s;
  html += "></div>";
  html += "<div><label for=sweep>Sweep every (min, 0 = off)</label><input id=sweep name=sweep type=number min=0 max=1440 value=";
  html += settings.sweep_min;
  html += "></div></div>";

  // Two columns of checkboxes rather than one list with a modifier: "show it" and
  // "park on it" are different questions, and a screen worth being able to swipe
  // to is not necessarily one worth leaving up for minutes at a time.
  //
  // Applies on restart because the tileview builds its tiles once — same as
  // orientation and the data source.
  // Indexed by PuckScreen id, but listed below in PUCK_SCREEN_ORDER so the page
  // reads the way the device swipes.
  static const char* const SCREEN_NAME[PUCK_SCREEN_COUNT] = {
      "Power", "Battery", "Solar", "Flows", "Cost", "Settings", "Load"};
  html += "<label>Screens (applies on restart)</label>";
  html += "<table class=screens><tr><th></th><th>Show</th><th>Auto-cycle</th></tr>";
  for (PuckScreen screen : PUCK_SCREEN_ORDER) {
    const uint8_t i = static_cast<uint8_t>(screen);
    const bool server_only = (PUCK_SERVER_ONLY_SCREENS & (1u << i)) != 0;
    // A screen the source cannot fill is shown struck through rather than hidden,
    // so the list is the same shape whichever source is configured and nobody
    // wonders where two of them went.
    const bool unavailable = modbus && server_only;
    html += "<tr><td>";
    html += unavailable ? "<s>" : "";
    html += SCREEN_NAME[i];
    html += unavailable ? "</s>" : "";
    html += "</td><td><input type=checkbox name=scr" + String(i) + " value=1";
    if (settings.screens_visible & (1u << i)) {
      html += " checked";
    }
    // The settings screen is always built — it is the only way back to this page
    // from the device — so its box says so rather than pretending to be a choice.
    if (unavailable || i == PUCK_SCREEN_SETTINGS || i == PUCK_SCREEN_POWER) {
      html += " disabled";
    }
    html += "></td><td><input type=checkbox name=rotscr" + String(i) + " value=1";
    if (settings.screens_rotate & (1u << i)) {
      html += " checked";
    }
    if (unavailable) {
      html += " disabled";
    }
    html += "></td></tr>";
  }
  html += "</table>";
  html += "<p class=hint>Power and Settings are always shown: one is what the device is "
          "for, the other is the only way back to this page.</p>";

  html += "<button type=submit>Apply</button></form>";

  html += "<h2>Firmware</h2><p>Running <code>" PUCK_FW_VERSION "</code>.</p>";

  // The result gets its own paragraph rather than a <br> after the version. A line
  // break butts the two together; what is running and what is available are two
  // separate facts and want a gap between them.
  switch (update.state) {
    case UpdateState::Checking:
      html += "<p>Checking GitHub for a newer release&hellip;</p>";
      break;
    case UpdateState::Available:
      // The one state worth shouting about, and the only one that offers a button
      // that reboots the device.
      html += "<p class=ok><b>Version ";
      html += escape_html(update.latest_version);
      html += " is available.</b></p>";
      break;
    case UpdateState::UpToDate:
      html += "<p>This is the latest release.</p>";
      break;
    case UpdateState::Downloading:
      html += "<p>Downloading&hellip;</p>";
      break;
    case UpdateState::Failed:
      html += "<p class=bad>Could not check: ";
      html += escape_html(update.message);
      html += "</p>";
      break;
    case UpdateState::Idle:
      html += updater_enabled() ? "" : "<p>Update checks are turned off.</p>";
      break;
  }

  if (update.state == UpdateState::Available) {
    html += "<form method=post action=/update-apply><button type=submit>";
    html += "Install ";
    html += escape_html(update.latest_version);
    html += " and restart</button></form>";
  }

  html += "<form method=post action=/update-check><button class=secondary type=submit>";
  html += "Check now</button></form>";

  html += "<form method=post action=/updates>";
  html += "<label class=opt><input type=checkbox name=enabled value=1";
  html += updater_enabled() ? " checked" : "";
  html += "> Check for a newer release when this page is opened</label>";
  // Said plainly because it is the only thing on this page that talks to anywhere
  // other than your own network.
  html += "<p class='hint gap'>The only outbound call this device makes. "
          "Installing is always manual, and always reboots.</p>";
  html += "<button type=submit>Save</button></form>";

  html += "<h2>Danger</h2>";
  // First of the three, and ordered by what each costs you: a restart loses nothing,
  // forgetting the token means re-enrolling, forgetting WiFi means starting over at
  // the captive portal.
  html += "<form method=post action=/restart><button class=secondary type=submit>";
  html += "Restart Device</button></form>";
  html += "<form method=post action=/forget-server><button class=secondary type=submit>";
  html += "Forget server and token</button></form>";
  html += "<form method=post action=/forget-wifi><button class=secondary type=submit>";
  html += "Forget WiFi and restart</button></form>";

  html += "</body></html>";
  return html;
}

// Every response goes through here so none of them can be cached.
//
// WebServer sends no cache headers at all, which leaves a browser free to apply
// its own heuristics — and it does. Two things then go wrong. The page is built
// per request out of live values, so a cached copy shows a stale poll result and
// settings that are no longer stored. Worse, a firmware update changes the page's
// own markup: a browser holding the previous version renders controls that no
// longer exist and hides ones that now do, at the exact moment someone has just
// flashed a device and gone looking to configure it.
//
// There is nothing to lose by refusing to cache. The CSS is inline and there are
// no other assets, so the whole page is one request either way.
void send_html(int code, const String& body) {
  s_server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  // For anything in the path that predates Cache-Control. Captive-portal
  // interceptors and older Android webviews are the realistic cases, and both are
  // on the likely route to this page.
  s_server.sendHeader("Pragma", "no-cache");
  s_server.sendHeader("Expires", "0");
  s_server.send(code, "text/html; charset=utf-8", body);
}

void send_page(const String& message = String(), bool error = false) {
  send_html(200, page(message, error));
}

void handle_root() {
  send_page();
}

void handle_save() {
  const String enrol = s_server.arg("enrol");
  String base = s_server.arg("base");
  String token = s_server.arg("token");

  // The pasted URL wins when both are given: it is the one-step path, and someone
  // who pasted it clearly meant to use it.
  if (enrol.length() > 0) {
    const EnrolUrl parsed = enrol_url_parse(enrol.c_str());
    if (!parsed.ok) {
      send_page("That does not look like an enrolment URL. It should start http:// or https://.",
                true);
      return;
    }
    base = parsed.base;
    if (strlen(parsed.token) > 0) {
      token = parsed.token;
    }
  } else if (base.length() > 0) {
    // Normalise a hand-typed URL through the same parser, so a trailing slash or
    // a stray path cannot produce ".../api/summary" with a double slash.
    const EnrolUrl parsed = enrol_url_parse(base.c_str());
    if (!parsed.ok) {
      send_page("The server URL must start with http:// or https://.", true);
      return;
    }
    base = parsed.base;
  }

  if (base.length() == 0) {
    send_page("Enter an enrolment URL, or a server URL.", true);
    return;
  }
  if (token.length() == 0 && settings_get().token.isEmpty()) {
    send_page("No token stored yet, so one is needed.", true);
    return;
  }

  if (!settings_set_server(base, token)) {
    send_page("Could not write to storage.", true);
    return;
  }
  // Try the new details straight away rather than after a backoff.
  poller_wake();
  send_page("Saved. Testing in the background — the result appears above Test connection.",
            false);
}

void handle_test() {
  Snapshot snapshot;
  int status_code = 0;
  const FetchResult result = sigen_api_fetch(&snapshot, &status_code);

  String message;
  if (result == FetchResult::Ok) {
    message = "Connected. Plant reading is ";
    message += snapshot.age_s;
    message += "s old";
    if (snapshot.power.home.known) {
      message += ", house load ";
      message += String(snapshot.power.home.value, 2);
      message += " kW";
    }
    message += ".";
    send_page(message, false);
    return;
  }

  // The wording matters: these need different actions from the reader.
  switch (result) {
    case FetchResult::Unauthorised:
      message = "Rejected (HTTP " + String(status_code) +
                "). The kiosk token has been revoked — create a new one in Admin and paste "
                "the new enrolment URL.";
      break;
    case FetchResult::ClockUnset:
      message = "The clock is not set yet, so an HTTPS certificate cannot be checked. "
                "Wait a moment for NTP, or use an http:// URL on the LAN.";
      break;
    case FetchResult::TlsFailed:
      message = "TLS failed. The certificate could not be validated — check the hostname "
                "matches the certificate.";
      break;
    case FetchResult::NotConfigured:
      message = "No server URL or token stored yet.";
      break;
    default:
      message = String("Failed: ") + fetch_result_name(result);
      if (status_code != 0) {
        message += " (HTTP " + String(status_code) + ")";
      }
      break;
  }
  send_page(message, true);
}

void handle_display() {
  const long brightness = s_server.arg("bright").toInt();
  const long poll = s_server.arg("poll").toInt();
  const long dim_after = s_server.arg("dim").toInt();
  const long dim_brightness = s_server.arg("dimb").toInt();
  if (brightness >= 10 && brightness <= 255 && dim_brightness >= 1 && dim_brightness <= 255 &&
      dim_after >= 0) {
    settings_set_display(static_cast<uint8_t>(brightness), static_cast<uint32_t>(dim_after),
                         static_cast<uint8_t>(dim_brightness));
  }
  if (poll > 0) {
    settings_set_poll_interval(static_cast<uint32_t>(poll));
  }
  const String orient = s_server.arg("orient");
  bool restart_needed = false;
  if (orient.length() > 0) {
    const uint8_t turns = static_cast<uint8_t>(orient.toInt()) & 0x03;
    if (turns != settings_get().orientation) {
      settings_set_orientation(turns);
      restart_needed = true;
    }
  }
  const String fine = s_server.arg("fine");
  if (fine.length() > 0) {
    const int16_t tenths = static_cast<int16_t>(lroundf(fine.toFloat() * 10.0f));
    if (tenths != settings_get().fine_tenths) {
      settings_set_fine_rotation(tenths);
      ui_set_fine_rotation(settings_get().fine_tenths);
      touch_set_fine_rotation(settings_get().fine_tenths);
    }
  }

  const String rot = s_server.arg("rot");
  const String sweep = s_server.arg("sweep");
  if (rot.length() > 0 || sweep.length() > 0) {
    settings_set_screensaver(static_cast<uint32_t>(rot.toInt()),
                             static_cast<uint32_t>(sweep.toInt()));
    ui_set_rotate_interval(settings_get().rotate_s);
    ui_set_sweep_interval(settings_get().sweep_min);

    // Read alongside the interval rather than on its own, because an unchecked
    // box sends nothing and there is no way to tell that from a form that never
    // carried the field. The interval always arrives, so its presence is what
    // says this section was submitted.
    const bool rotate_on = s_server.hasArg("roten");
    if (rotate_on != settings_get().rotate_enabled) {
      settings_set_rotate_enabled(rotate_on);
      ui_set_rotate_enabled(rotate_on);
    }
  }

  // An unchecked box sends nothing at all, so the masks are rebuilt from what did
  // arrive rather than edited. A disabled box sends nothing either, which is why
  // the two always-on screens are forced back in here as well as in ui_create().
  uint8_t visible = (1u << PUCK_SCREEN_POWER) | (1u << PUCK_SCREEN_SETTINGS);
  uint8_t rotate = 0;
  for (uint8_t i = 0; i < PUCK_SCREEN_COUNT; ++i) {
    if (s_server.hasArg(("scr" + String(i)).c_str())) {
      visible |= (1u << i);
    }
    if (s_server.hasArg(("rotscr" + String(i)).c_str())) {
      rotate |= (1u << i);
    }
  }
  const bool screens_changed = visible != settings_get().screens_visible ||
                               rotate != settings_get().screens_rotate;
  if (screens_changed) {
    settings_set_screens(visible, rotate);
    restart_needed = true;
  }
  poller_wake();
  send_page(restart_needed ? "Applied. Restart for the new orientation or screen list."
                           : "Display settings applied.",
            false);
}

void handle_update_check() {
  // Pressing the button is asking for a check whether or not the automatic one is
  // switched on, so it is honoured either way — but it no longer flips the setting
  // as a side effect. That behaviour changed a stored preference from a button
  // labelled as doing something else, and left no way to change it back.
  if (!updater_enabled()) {
    updater_check();  // synchronous: nothing else will do it for us
    send_page("", false);
    return;
  }
  updater_request_check(/*force=*/true);
  send_page("", false);
}

void handle_updates() {
  const bool enabled = s_server.arg("enabled") == "1";
  if (!settings_set_check_updates(enabled)) {
    send_page("Could not store the update setting.", true);
    return;
  }
  send_page(enabled ? "This page will check for a newer release when it is opened."
                    : "Update checks turned off. This device now makes no outbound calls "
                      "beyond your own network.",
            false);
}

void handle_update_apply() {
  // Answer first, then queue it. The download happens on the poll task, so this
  // handler no longer has to stay alive through it — and the reply cannot be lost
  // to the reboot that ends it.
  send_html(200,
            "<!doctype html><p>Installing and restarting. This page will stop "
            "responding for a minute or so.");
  s_server.client().flush();
  updater_request_apply();
}

void handle_source() {
  const String choice = s_server.arg("src");
  if (choice != "server" && choice != "modbus") {
    send_page("Pick a data source.", true);
    return;
  }
  const DataSource source = choice == "modbus" ? DataSource::Modbus : DataSource::Server;
  if (!settings_set_source(source)) {
    send_page("Could not store the data source.", true);
    return;
  }
  // No live switch: the poll task chose its fetch function and its stack size at
  // boot, and the screen count is fixed once the tileview is built (§D1).
  send_page("Data source saved. Restart to apply it.", false);
}

void handle_hostname() {
  const String cleaned = settings_clean_hostname(s_server.arg("host"));
  if (cleaned.isEmpty()) {
    send_page("A device name needs at least one letter or digit.", true);
    return;
  }
  if (!settings_set_hostname(cleaned)) {
    send_page("Could not store the device name.", true);
    return;
  }
  // Echoing the cleaned name back matters: "Front Room" is stored as
  // "front-room", and someone who is not told that will look for the wrong
  // address after the restart.
  send_page("Device name saved as \"" + cleaned + "\". Restart to apply it.", false);
}

void handle_modbus() {
  const String host = s_server.arg("mbhost");
  const long port = s_server.arg("mbport").toInt();
  const long plant = s_server.arg("mbplant").toInt();

  ModbusDevice devices[SETTINGS_MAX_MODBUS_DEVICES];
  for (size_t i = 0; i < SETTINGS_MAX_MODBUS_DEVICES; ++i) {
    const String suffix(static_cast<unsigned>(i));
    const long id = s_server.arg("mbid" + suffix).toInt();
    if (id <= 0 || id > 246) {
      continue;  // an empty or out-of-range slot stays unused
    }
    devices[i].slave_id = static_cast<uint8_t>(id);
    devices[i].type = s_server.arg("mbtype" + suffix) == "acc" ? ModbusDeviceType::AcCharger
                                                               : ModbusDeviceType::Inverter;
    // An unchecked box is simply absent from the form, so this reads false.
    devices[i].dc_charger = s_server.arg("mbdc" + suffix) == "1";
  }

  if (!settings_set_modbus(host, static_cast<uint16_t>(port), static_cast<uint8_t>(plant),
                           devices, SETTINGS_MAX_MODBUS_DEVICES)) {
    send_page("Could not store the plant settings.", true);
    return;
  }
  // Drops the cached slow readings and the midnight baseline, so a new plant does
  // not inherit the previous one's day.
  modbus_api_reset();
  poller_wake();
  send_page(settings_get().source == DataSource::Modbus
                ? "Plant settings saved."
                : "Plant settings saved. Switch the data source to use them.",
            false);
}

void handle_restart() {
  send_html(200,
            "<!doctype html><p>Restarting. This page will stop responding for a "
            "few seconds.");
  s_server.client().flush();
  delay(200);
  ESP.restart();
}

void handle_forget_server() {
  settings_forget_server();
  send_page("Server details cleared.", false);
}

void handle_forget_wifi() {
  send_html(200, "<!doctype html><p>Forgetting WiFi and restarting. Join \"" +
                     String(net_setup_ap_name()) + "\" to set it up again.");
  delay(200);
  net_forget_wifi();
}

}  // namespace

void settings_server_begin() {
  if (s_running) {
    return;
  }
  s_server.on("/", HTTP_GET, handle_root);
  s_server.on("/save", HTTP_POST, handle_save);
  s_server.on("/source", HTTP_POST, handle_source);
  s_server.on("/hostname", HTTP_POST, handle_hostname);
  s_server.on("/modbus", HTTP_POST, handle_modbus);
  s_server.on("/restart", HTTP_POST, handle_restart);
  s_server.on("/test", HTTP_POST, handle_test);
  s_server.on("/display", HTTP_POST, handle_display);
  s_server.on("/update-check", HTTP_POST, handle_update_check);
  s_server.on("/updates", HTTP_POST, handle_updates);
  s_server.on("/update-apply", HTTP_POST, handle_update_apply);
  s_server.on("/forget-server", HTTP_POST, handle_forget_server);
  s_server.on("/forget-wifi", HTTP_POST, handle_forget_wifi);
  s_server.onNotFound(handle_root);
  s_server.begin();
  s_running = true;
  Serial.println("[web] settings page up on port 80");
}

void settings_server_loop() {
  if (s_running) {
    s_server.handleClient();
  }
}

bool settings_server_running() {
  return s_running;
}

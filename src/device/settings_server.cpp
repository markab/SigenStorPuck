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
    "h1{font-size:1.3rem}h2{font-size:1rem;margin-top:2rem;color:#9ab}"
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
    "p.hint{font-size:.8rem;color:#888;margin-top:-.6rem}";

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

  String html;
  html.reserve(4096);
  html += "<!doctype html><html><head><meta charset=utf-8>";
  html += "<meta name=viewport content='width=device-width,initial-scale=1'>";
  html += "<title>SigenStorPuck</title><style>";
  html += PAGE_STYLE;
  html += "</style></head><body>";
  html += "<h1>SigenStorPuck</h1>";
  html += "<p class=hint>firmware " PUCK_FW_VERSION " &middot; ";
  html += escape_html(net_ip());
  html += "</p>";

  if (message.length() > 0) {
    html += "<p class=";
    html += message_is_error ? "bad" : "ok";
    html += ">";
    html += escape_html(message);
    html += "</p>";
  }

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
  html += "> Plant over Modbus (LAN only, no cost screen)</label>";
  html += "<p class=hint>Takes effect after a restart, like orientation.</p>";
  html += "<button type=submit>Save</button></form>";
  html += "<form method=post action=/restart><button class=secondary type=submit>";
  html += "Restart now</button></form>";

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
  html += "<button type=submit>Save plant</button></form>";

  html += "<h2>Status</h2><p>";
  html += "Last poll: <code>";
  html += fetch_result_name(status.last_result);
  html += "</code>";
  if (status.last_http_status != 0) {
    html += " (HTTP ";
    html += status.last_http_status;
    html += ")";
  }
  html += "<br>Consecutive failures: ";
  html += status.consecutive_failures;
  html += "<br>Clock: ";
  html += net_time_synced() ? "set from NTP" : "not set (HTTPS will fail)";
  const PowerStatus power = power_status();
  html += "<br>Puck power: ";
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
  html += "</code>. Letters, digits and hyphens; takes effect after a restart.</p>";
  html += "<button type=submit>Save name</button></form>";

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
  html += "<div class=row>";
  html += "<div><label for=rot>Rotate screens (s, 0 = off)</label><input id=rot name=rot type=number min=0 max=3600 value=";
  html += settings.rotate_s;
  html += "></div>";
  html += "<div><label for=sweep>Sweep every (min, 0 = off)</label><input id=sweep name=sweep type=number min=0 max=1440 value=";
  html += settings.sweep_min;
  html += "></div></div>";
  html += "<button type=submit>Apply</button></form>";

  html += "<h2>Firmware</h2><p>Running <code>" PUCK_FW_VERSION "</code>.";
  const UpdateStatus update = updater_status();
  if (!update.latest_version.isEmpty()) {
    html += " Latest release <code>";
    html += escape_html(update.latest_version);
    html += "</code>.";
  }
  if (update.message.length() > 0) {
    html += "<br>";
    html += escape_html(update.message);
  }
  html += "</p>";
  html += "<form method=post action=/update-check><button class=secondary type=submit>";
  html += updater_enabled() ? "Check for updates" : "Enable update checks and check now";
  html += "</button></form>";
  if (update.state == UpdateState::Available) {
    html += "<form method=post action=/update-apply><button type=submit>";
    html += "Install ";
    html += escape_html(update.latest_version);
    html += " and restart</button></form>";
  }

  html += "<h2>Danger</h2>";
  html += "<form method=post action=/forget-server><button class=secondary type=submit>";
  html += "Forget server and token</button></form>";
  html += "<form method=post action=/forget-wifi><button class=secondary type=submit>";
  html += "Forget WiFi and restart</button></form>";

  html += "</body></html>";
  return html;
}

void send_page(const String& message = String(), bool error = false) {
  s_server.send(200, "text/html; charset=utf-8", page(message, error));
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
  send_page("Saved. Testing in the background — see Status.", false);
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
  }
  poller_wake();
  send_page(restart_needed ? "Applied. Restart for the new orientation."
                           : "Display settings applied.",
            false);
}

void handle_update_check() {
  // Ticking the box is implied by asking: someone pressing "check for updates" has
  // consented to the outbound call that requires.
  if (!updater_enabled()) {
    settings_set_check_updates(true);
  }
  updater_check();
  send_page("", false);
}

void handle_update_apply() {
  // Answer first: applying reboots the device, so a reply sent afterwards never
  // arrives and the browser shows a connection error instead of an explanation.
  s_server.send(200, "text/html; charset=utf-8",
                "<!doctype html><p>Installing and restarting. This page will stop "
                "responding for a minute or so.");
  s_server.client().flush();
  delay(200);
  updater_apply();
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
  s_server.send(200, "text/html; charset=utf-8",
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
  s_server.send(200, "text/html; charset=utf-8",
                "<!doctype html><p>Forgetting WiFi and restarting. Join \"" +
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

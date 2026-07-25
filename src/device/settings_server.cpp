#include "settings_server.h"

#include <WebServer.h>

#include "board_config.h"
#include "enrol_url.h"
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
    "input,button{font-size:1rem;padding:.5rem;width:100%;box-sizing:border-box;margin:.25rem 0 1rem;"
    "border-radius:.4rem;border:1px solid #444;background:#1c1c1e;color:#eee}"
    "button{background:#0a84ff;border:0;font-weight:600;cursor:pointer}"
    "button.secondary{background:#333}"
    "label{font-size:.85rem;color:#9ab}"
    ".row{display:flex;gap:.75rem}.row>*{flex:1}"
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

  html += "<h2>Server</h2>";
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

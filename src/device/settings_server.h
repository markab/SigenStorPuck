// The on-network settings page, at http://sigenstorpuck.local/
//
// This is where the server URL and kiosk token are entered, because a round
// 466 px screen is the wrong place to type either (PLAN.md §B2).

#pragma once

// Starts the web server. Call once WiFi is connected — the captive portal uses
// port 80 too, so starting earlier would collide with it.
void settings_server_begin();

// Must be called from loop().
void settings_server_loop();

bool settings_server_running();

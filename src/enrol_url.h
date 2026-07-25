// Splitting a kiosk enrolment URL into a base URL and a token.
//
// SigenStor Display's Admin page hands you one absolute URL:
//
//   https://host/kiosk-enroll?token=abc123...
//
// A 466 px round screen is a terrible place to type a hostname and a 43-character
// token, so the device never asks for them separately: you paste that whole URL
// into one field and this splits it. Entering them by hand still works.
//
// Deliberately free of Arduino headers so the simulator compiles it too — it is
// the one piece of provisioning that is pure logic and worth being able to
// exercise on the desktop.

#pragma once

#include <stddef.h>

struct EnrolUrl {
  bool ok = false;
  // Scheme and authority only, no trailing slash: "https://host" or
  // "http://192.168.1.10:8000". This is what gets "/api/summary" appended.
  char base[128] = {};
  char token[96] = {};
};

// Parses an enrolment URL. Accepts the full form with ?token=..., and also a
// bare base URL with no query, in which case `token` is left empty and `ok` is
// still true — entering a URL now and a token later is a reasonable thing to do.
//
// Returns a result with ok=false if there is no usable http/https base.
EnrolUrl enrol_url_parse(const char* text);

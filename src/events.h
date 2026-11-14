// The parsed form of GET /api/events: grid events the household takes part in.
//
// Octopus Saving Sessions, Octopus's weekend Happy Hours, other free-import
// periods and Axle events, as SigenStor Display (0.45.0+) records them. One
// call returns the event happening now (or the next booked one) plus the most
// recent finished ones, newest first.
//
// Like snapshot.h this is the whole contract between the server and the screens,
// and it is Arduino-free so the simulator compiles it and the selftest checks it.
// The model carries the whole list, not just the one event the round screen
// shows: a bigger screen draws rows of past events from the same struct.
//
// The same rules as the summary apply. Unknown is null, never 0 — every figure
// is a MaybeFloat/MaybeInt and renders as "--". A kind or state string this
// firmware does not know maps to Unknown rather than failing the parse, so a
// newer server adding an event type cannot blank the screen; the screens skip
// what they cannot name (events_renderable()).
//
// The text helpers at the bottom live here rather than in either UI so every
// board words an event identically, and so the selftest can hold them to it —
// "Tonight" against a UTC device clock is exactly the kind of thing that looks
// right on the glass until 1 am in the wrong timezone.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"  // MaybeFloat, MaybeInt

enum class EventKind : uint8_t {
  Unknown = 0,
  Saving,      // "saving": an Octopus Saving Session
  HappyHour,   // "happy_hour": an Octopus weekend Happy Hour, free import to a cap
  FreeImport,  // "free_import": any other free-import period
  Axle,        // "axle": an Axle event, export or import
};

enum class EventState : uint8_t {
  Unknown = 0,
  Live,      // happening now
  Upcoming,  // booked, not started
  Done,      // finished
  Deferred,  // a saving session the household did not take part in
};

enum class EventDirection : uint8_t {
  Unknown = 0,
  Export,
  Import,
};

struct GridEvent {
  EventKind kind = EventKind::Unknown;
  EventState state = EventState::Unknown;
  EventDirection direction = EventDirection::Unknown;

  // Unix seconds. `end` can be null on the wire — an Axle event stored without
  // an end time — so it carries its own flag: a null end is unknown, never the
  // epoch. The server treats such an event as an hour long when deciding it is
  // over; the screens show the start time alone.
  uint32_t start = 0;
  uint32_t end = 0;
  bool has_end = false;

  MaybeFloat import_kwh;
  MaybeFloat export_kwh;
  MaybeFloat cut_kwh;     // saving: reduction against the baseline
  MaybeFloat cap_kwh;     // happy hour: the free-import allowance
  MaybeFloat earned_gbp;  // axle: what the event paid
  MaybeFloat price_p;     // axle: the event's price, p/kWh
  MaybeFloat soc_start;   // battery %, at the start and end of the window
  MaybeFloat soc_end;
  MaybeInt points;        // saving: Octopoints
  // True while the points are the server's estimate rather than Octopus's award.
  // Always false when `points` is null and for every kind but saving.
  bool points_estimated = false;
};

// Past events kept. The server is asked for EVENTS_FETCH_LIMIT; extras beyond
// the capacity are dropped at parse time.
static constexpr size_t EVENTS_MAX_PAST = 8;
static constexpr size_t EVENTS_FETCH_LIMIT = 6;

struct EventList {
  // False until a payload has parsed. Also false when the server has no events
  // support at all (older than 0.45.0 answers 404), which the screens show the
  // same way as an empty list.
  bool valid = false;
  uint32_t now = 0;  // the server's clock when it answered, unix seconds

  // Live now, else the soonest upcoming (which may be a deferred session), else
  // none.
  bool has_current = false;
  GridEvent current;

  // Finished, newest first.
  uint8_t past_count = 0;
  GridEvent past[EVENTS_MAX_PAST];
};

// Parses one /api/events payload. Returns false for malformed JSON, a payload
// that is not an object, or one without a usable `now`; `out` is then left
// untouched, so the caller keeps its last good list.
bool events_parse(const char* json, size_t length, EventList* out);

// Whether a screen can name and place this event: a known kind and state and a
// start time. Unknown kinds and states parse, but are not drawn.
bool events_renderable(const GridEvent& event);

// The one event a small screen shows: the current one, else the most recently
// finished, else none — skipping any the firmware cannot render. Both UIs pick
// through this so they always agree. Returns nullptr when there is nothing.
const GridEvent* events_featured(const EventList& list);

// The event's state at `now` (unix seconds), which may have moved on since the
// server answered: the list is fetched every few minutes, but "42 min left"
// should count down and a booked event should go live on time. Upcoming becomes
// Live at the start and Live becomes Done at the end; an event with no end stays
// Live until the server says otherwise. Deferred and Done are never changed.
EventState events_state_at(const GridEvent& event, uint32_t now);

// "SAVING SESSION", "HAPPY HOUR", "FREE IMPORT", "AXLE EXPORT", "AXLE IMPORT"
// (or "AXLE EVENT" with no direction). Empty for an unknown kind.
const char* events_title(const GridEvent& event);

// The day and time window in local time, `tz_offset_min` minutes east of UTC:
// "Tonight 17:30–19:00", "Today 13:00–15:00", "Tomorrow 17:30–18:30",
// "Yesterday 16:30–18:00", or "Thu 13 Nov 17:30–19:00" further away. With no end
// time, the start alone: "Tonight 17:30". "Tonight" is today from 17:00 on.
void events_format_when(const GridEvent& event, uint32_t now, int32_t tz_offset_min,
                        char* out, size_t size);

// "Live · 42 min left", "Live · 1 h 05 min left", "Live" (no end), "Starting",
// "In 42 min", "In 2 h 15 min", "In 3 days", "Done" or "Deferred".
void events_format_status(const GridEvent& event, uint32_t now, char* out, size_t size);

// One headline figure: the value with its unit, and a caption naming it.
struct EventFigure {
  char value[24];
  char caption[16];
};

// The kind's key figures, at most two, unknown values as "--":
//   saving       cut kWh ("CUT"), and points ("AWARDED" or "ESTIMATED", estimated
//                ones marked "~1,520 pts"); a deferred session shows the cut alone
//   happy hour   "11.2 of 16 kWh" ("IMPORTED"), or "11.2 kWh" with no cap
//   free import  kWh imported ("IMPORTED")
//   axle         kWh exported or imported, and "£1.26" ("EARNED")
// A booked (upcoming) event has no outcome yet, so it shows only what is known
// ahead, and may show nothing: a happy hour's cap ("16 kWh", "FREE UP TO"), an
// Axle event's price ("30.0p/kWh", "PRICE"), a joined session's estimated points.
// Returns how many were written to `out` (which must hold `max`).
size_t events_figures(const GridEvent& event, EventFigure* out, size_t max);

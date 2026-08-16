// Checks for the parts of the Modbus path that do not need a plant (PLAN.md §D).
//
// Run with `.pio/build/sim/program --selftest`. Desktop-only, like the rest of
// src/sim/, but everything it exercises is shared with the firmware — which is
// the reason modbus_regs.cpp and history.cpp live outside src/device/.
//
// What is deliberately *not* covered, because only real hardware can settle it:
// whether Sigenergy's reversed function codes are the right way round on this
// firmware, whether the inverter tolerates a second Modbus client alongside the
// Pi, and the sign convention of plant_active_power.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "button_gesture.h"
#include "day_series.h"
#include "history.h"
#include "modbus_regs.h"
#include "screen_window.h"
#include "solar_forecast.h"

namespace {

int s_failures = 0;
int s_checks = 0;

void check(bool condition, const char* what) {
  ++s_checks;
  if (!condition) {
    ++s_failures;
    printf("  FAIL  %s\n", what);
  }
}

void check_near(float actual, float expected, const char* what) {
  ++s_checks;
  if (fabsf(actual - expected) > 0.005f) {
    ++s_failures;
    printf("  FAIL  %s (got %.4f, wanted %.4f)\n", what, actual, expected);
  }
}

// A proportional tolerance, for figures compared against a reference computed in
// double precision. The firmware is float throughout — an S3 has hardware FP32
// and nothing else — so a day's worth of accumulated slots cannot match to the
// absolute 5 mWh check_near() wants.
void check_within(float actual, float expected, float fraction, const char* what) {
  ++s_checks;
  if (fabsf(actual - expected) > fabsf(expected) * fraction) {
    ++s_failures;
    printf("  FAIL  %s (got %.5f, wanted %.5f)\n", what, actual, expected);
  }
}

float decode_one(uint8_t key, const uint16_t* words, size_t count) {
  const ModbusReg& reg = MODBUS_REGS[key];
  float value = 0.0f;
  if (!modbus_decode(reg, reg.address, words, count, &value)) {
    return NAN;
  }
  return value;
}

// --- decoding -------------------------------------------------------------

void test_decode() {
  printf("decode\n");

  // U16 with a gain: SOC of 78.5 % is raw 785 at gain 10.
  const uint16_t soc[] = {785};
  check_near(decode_one(MB_PLANT_ESS_SOC, soc, 1), 78.5f, "U16 gain 10");

  // S16 negative: -5.5 C is raw -55 at gain 10, two's complement.
  const uint16_t temp[] = {0xFFC9};
  check_near(decode_one(MB_INV_ESS_MAX_TEMP, temp, 1), -5.5f, "S16 negative");

  // S32 positive, big-endian word order: 3420 W -> 3.42 kW at gain 1000.
  const uint16_t pv[] = {0x0000, 0x0D5C};
  check_near(decode_one(MB_PLANT_PV_POWER, pv, 2), 3.42f, "S32 positive");

  // S32 negative: exporting 1.1 kW is raw -1100.
  const uint16_t grid[] = {0xFFFF, 0xFBB4};
  check_near(decode_one(MB_PLANT_GRID_POWER, grid, 2), -1.1f, "S32 negative");

  // U32: 16.00 kWh capacity is raw 1600 at gain 100.
  const uint16_t capacity[] = {0x0000, 0x0640};
  check_near(decode_one(MB_PLANT_ESS_CAPACITY, capacity, 2), 16.0f, "U32 gain 100");

  // U64 accumulator, past 32 bits so a truncating decode would show up:
  // 0x0000000100000000 = 4294967296 raw, /100 = 42949672.96 kWh.
  const uint16_t total[] = {0x0000, 0x0001, 0x0000, 0x0000};
  const float imported = decode_one(MB_PLANT_TOTAL_IMPORTED, total, 4);
  check(imported > 42949000.0f && imported < 42950500.0f, "U64 beyond 32 bits");

  // A register that does not fit inside the words it was given must refuse
  // rather than read past the end.
  const ModbusReg& reg = MODBUS_REGS[MB_PLANT_PV_POWER];
  float ignored = 0.0f;
  check(!modbus_decode(reg, reg.address, pv, 1, &ignored), "short span refused");
  check(!modbus_decode(reg, reg.address + 1, pv, 2, &ignored), "span starting late refused");
}

// --- batching -------------------------------------------------------------

void test_plan() {
  printf("plan\n");
  ModbusSpan spans[8];

  // The plant's fast set is one read of 94 words, 30000-30093, exactly as §D2
  // works out. If this splits, the gap tolerance has crept back in from the
  // server's batcher and every poll costs an extra second.
  size_t count = modbus_plan(ModbusScope::Plant, ModbusCadence::Fast, spans, 8);
  check(count == 1, "plant fast is a single request");
  if (count == 1) {
    check(spans[0].start == 30000, "plant fast starts at 30000");
    check(spans[0].words == 94, "plant fast is 94 words");
  }

  count = modbus_plan(ModbusScope::Plant, ModbusCadence::Slow, spans, 8);
  check(count == 1, "plant slow is a single request");
  if (count == 1) {
    check(spans[0].start == 30216, "plant slow starts at 30216");
    check(spans[0].words == 8, "plant slow is 8 words");
  }

  // The inverter's slow set spans 30566-30620; its DC output at 31502 is nearly
  // a thousand registers away and cannot join it.
  count = modbus_plan(ModbusScope::Inverter, ModbusCadence::Slow, spans, 8);
  check(count == 2, "inverter slow needs two requests");
  if (count == 2) {
    check(spans[0].start == 30566 && spans[0].words == 55, "inverter slow first span");
    check(spans[1].start == 31509 && spans[1].words == 2, "inverter slow second span");
  }

  count = modbus_plan(ModbusScope::Inverter, ModbusCadence::Fast, spans, 8);
  check(count == 1 && spans[0].start == 31502 && spans[0].words == 2, "inverter fast span");

  count = modbus_plan(ModbusScope::AcCharger, ModbusCadence::Fast, spans, 8);
  check(count == 1 && spans[0].start == 32003 && spans[0].words == 2, "charger span");

  // No span may exceed the protocol's own ceiling.
  for (int scope = 0; scope < 3; ++scope) {
    for (int cadence = 0; cadence < 2; ++cadence) {
      const size_t n = modbus_plan(static_cast<ModbusScope>(scope),
                                   static_cast<ModbusCadence>(cadence), spans, 8);
      for (size_t i = 0; i < n; ++i) {
        check(spans[i].words <= MODBUS_MAX_WORDS, "span within the 124-word limit");
      }
    }
  }
}

// --- derivation -----------------------------------------------------------

void test_snapshot() {
  printf("snapshot\n");

  ModbusValues values;
  // A plant read: PV 3.42, grid -1.1 (exporting), battery +1.2 (charging).
  const uint16_t plant[] = {
      0x0000, 0x0D5C,  // 30035 pv 3420 W
  };
  modbus_apply(&values, ModbusScope::Plant, 30035, plant, 2);

  const uint16_t grid[] = {0xFFFF, 0xFBB4};  // 30005 -1100 W
  modbus_apply(&values, ModbusScope::Plant, 30005, grid, 2);

  const uint16_t batt[] = {0x0000, 0x04B0};  // 30037 1200 W
  modbus_apply(&values, ModbusScope::Plant, 30037, batt, 2);

  Snapshot snapshot;
  modbus_to_snapshot(values, &snapshot);

  // home = pv - batt + grid - ev = 3.42 - 1.2 + (-1.1) - 0 = 1.12, which is the
  // figure 01_normal.json carries for the same inputs.
  check(snapshot.power.home.known, "home derived");
  check_near(snapshot.power.home.value, 1.12f, "home matches the server's formula");
  check(snapshot.power.ev.known && snapshot.power.ev.value == 0.0f, "no charger means 0 kW EV");

  // EV must come out of the house figure, or a charging car is counted twice.
  ModbusValues with_ev = values;
  const uint16_t charger[] = {0x0000, 0x1B58};  // 32003, 7000 W
  modbus_apply(&with_ev, ModbusScope::AcCharger, 32003, charger, 2);
  modbus_to_snapshot(with_ev, &snapshot);
  check_near(snapshot.power.ev.value, 7.0f, "EV power read");
  check_near(snapshot.power.home.value, 0.0f, "home floors at zero, never negative");

  // Two inverters: temperatures average, daily generation sums.
  ModbusValues two;
  const uint16_t temp_a[] = {0x00FA};  // 25.0 C
  const uint16_t temp_b[] = {0x0136};  // 31.0 C
  modbus_apply(&two, ModbusScope::Inverter, 30620, temp_a, 1);
  modbus_apply(&two, ModbusScope::Inverter, 30620, temp_b, 1);
  const uint16_t gen[] = {0x0000, 0x0258};  // 6.00 kWh
  modbus_apply(&two, ModbusScope::Inverter, 31509, gen, 2);
  modbus_apply(&two, ModbusScope::Inverter, 31509, gen, 2);
  modbus_to_snapshot(two, &snapshot);
  check_near(snapshot.battery.temp_c.value, 28.0f, "inverter temperatures average");
  check_near(snapshot.today.solar.value, 12.0f, "inverter generation sums");

  // Alarms are bitfields, counted not summed.
  ModbusValues alarmed;
  const uint16_t bits[] = {0x0005};  // two bits set
  modbus_apply(&alarmed, ModbusScope::Plant, 30027, bits, 1);
  modbus_to_snapshot(alarmed, &snapshot);
  check(snapshot.alarms == 2, "alarm bits are counted");

  // Nothing read at all must not claim a day of zeroes.
  ModbusValues empty;
  modbus_to_snapshot(empty, &snapshot);
  check(!snapshot.today.present, "no day block without any daily register");
  check(!snapshot.power.home.known, "home unknown when its inputs are");
}

void test_day_baseline() {
  printf("day baseline\n");

  ModbusDayBaseline baseline;
  MaybeFloat imported;
  MaybeFloat exported;

  // First read of the day latches, so today starts at zero rather than at the
  // lifetime total.
  modbus_day_totals(&baseline, 20000, 1000.0f, 400.0f, true, &imported, &exported);
  check_near(imported.value, 0.0f, "first read latches import");
  check_near(exported.value, 0.0f, "first read latches export");

  modbus_day_totals(&baseline, 20000, 1004.5f, 402.25f, true, &imported, &exported);
  check_near(imported.value, 4.5f, "import accumulates through the day");
  check_near(exported.value, 2.25f, "export accumulates through the day");

  // Midnight: the day rolls and the baseline moves with it.
  modbus_day_totals(&baseline, 20001, 1004.5f, 402.25f, true, &imported, &exported);
  check_near(imported.value, 0.0f, "new day re-latches");

  // A firmware upgrade zeroes the counters. Re-latch instead of reporting a
  // day of minus a thousand kilowatt-hours.
  modbus_day_totals(&baseline, 20001, 0.0f, 0.0f, true, &imported, &exported);
  check_near(imported.value, 0.0f, "counter reset re-latches rather than going negative");
  modbus_day_totals(&baseline, 20001, 1.5f, 0.5f, true, &imported, &exported);
  check_near(imported.value, 1.5f, "accumulates again after a reset");

  // Unknown totals must stay unknown, not become zero.
  MaybeFloat none_in;
  modbus_day_totals(&baseline, 20001, 0.0f, 0.0f, false, &none_in, nullptr);
  check(!none_in.known, "unread totals stay unknown");
}

// --- history --------------------------------------------------------------

void test_history() {
  printf("history\n");

  history_reset(HistoryBank::Live);
  check(history_head_minute(HistoryBank::Live) == 0, "empty ring has no head");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 0, "empty ring has no samples");

  uint32_t from = 0;
  uint32_t to = 0;
  check(!history_window(HistoryBank::Live, &from, &to), "no window before anything is recorded");

  // A day anchored to UTC midnight: minute 30000000 is a round day boundary
  // only by construction, so pick one and work relative to it.
  const uint32_t midnight = (30000000u / 1440u) * 1440u;
  history_set_timezone(HistoryBank::Live, 0);
  for (uint32_t m = 0; m <= 600; ++m) {
    history_put(HistoryBank::Live, HistorySeries::Pv, midnight + m, static_cast<float>(m) / 100.0f);
  }
  check(history_head_minute(HistoryBank::Live) == midnight + 600, "head follows the newest sample");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 601, "every minute recorded");

  check(history_window(HistoryBank::Live, &from, &to), "window available once recorded");
  check(from == midnight, "window anchors to local midnight");
  check(to == midnight + 1440, "window covers the whole day");

  // Reduction: 1440 minutes onto 144 columns is ten minutes each, and only the
  // first 601 minutes hold anything.
  HistoryColumn columns[144];
  history_reduce(HistoryBank::Live, HistorySeries::Pv, from, to, columns, 144);
  check(columns[0].known, "first column has data");
  check_near(columns[0].min_value, 0.0f, "first column min");
  check_near(columns[0].max_value, 0.09f, "first column max");
  check(!columns[100].known, "columns past the last sample stay empty");
  check(columns[59].known, "last populated column");

  // A gap must not be bridged.
  history_reset(HistoryBank::Live);
  history_set_timezone(HistoryBank::Live, 0);
  history_put(HistoryBank::Live, HistorySeries::Soc, midnight + 10, 50.0f);
  history_put(HistoryBank::Live, HistorySeries::Soc, midnight + 400, 60.0f);
  history_reduce(HistoryBank::Live, HistorySeries::Soc, midnight, midnight + 1440, columns, 144);
  check(columns[1].known, "sample at minute 10 lands in column 1");
  check(!columns[20].known, "the gap between them stays a gap");

  // A clock jump backwards of more than a day starts again rather than
  // interleaving two eras of samples in the same ring.
  const uint32_t before = history_generation(HistoryBank::Live);
  history_put(HistoryBank::Live, HistorySeries::Soc, midnight - 5000, 42.0f);
  check(history_generation(HistoryBank::Live) != before, "a backwards clock jump resets the ring");

  history_reset(HistoryBank::Live);
}

// A day payload straight from /api/day/series, at the shape the device asks for.
// Four 15-minute slots is enough to exercise every branch and short enough to
// read; the device uses 5-minute slots and 288 of them.
void test_day_series() {
  printf("day series\n");
  history_reset(HistoryBank::Live);

  // A real local midnight for the +60 offset below: minute % 1440 must be 1380,
  // so that adding the offset lands on a UTC midnight. Picking a round-looking
  // number instead just tests that the window code disagrees with the fixture.
  const uint32_t midnight_minute = 28928100;
  const uint32_t day_start = midnight_minute * 60;
  char json[512];
  snprintf(json, sizeof(json),
           "{\"slot_minutes\":15,\"day_start\":%u,\"tz_offset_min\":60,"
           "\"solar_kw\":[0.0,1.5,3.0,4.5],"
           "\"soc_pct\":[null,40.0,55.0,70.0]}",
           static_cast<unsigned>(day_start));

  // "Now" is 40 minutes into the day: slots 0 and 1 are fully elapsed, slot 2 is
  // part way through, slot 3 has not started.
  const uint32_t now = midnight_minute + 40;
  check(day_series_parse(HistoryBank::Live, json, strlen(json), now), "day payload parses");

  check(history_head_minute(HistoryBank::Live) == now, "the ring stops at now, not at the end of the day");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 41,
        "every elapsed minute filled, and none beyond");

  // A null slot is a gap, not a zero: SoC was not recorded for the first quarter
  // hour, and drawing that as 0 % would claim the battery was flat at midnight.
  check(history_sample_count(HistoryBank::Live, HistorySeries::Soc) == 26, "null slots leave gaps");

  HistoryColumn columns[4];
  history_reduce(HistoryBank::Live, HistorySeries::Pv, midnight_minute, midnight_minute + 60, columns, 4);
  check(columns[0].known && columns[0].max_value == 0.0f, "slot 0 is a recorded zero");
  check(columns[1].known && columns[1].max_value == 1.5f, "slot 1 carries its value");
  check(columns[2].known && columns[2].max_value == 3.0f, "the part-elapsed slot is filled");
  check(!columns[3].known, "the slot that has not happened stays empty");

  // The timezone came from the payload, which is the only place the server path
  // gets a trustworthy one.
  uint32_t from = 0;
  uint32_t to = 0;
  check(history_window(HistoryBank::Live, &from, &to), "window available");
  check(from == midnight_minute && to == midnight_minute + 1440,
        "window anchors to the day the payload named");

  check(!day_series_parse(HistoryBank::Live, "{\"slot_minutes\":15}", 19, now), "a payload with no day is rejected");
  check(!day_series_parse(HistoryBank::Live, json, strlen(json), 0), "no clock means nothing is filed");

  history_reset(HistoryBank::Live);
}

// The two rings are the point of HistoryBank: one ring cannot hold two days,
// because a minute more than a day behind the head is indistinguishable from a
// clock jump and rightly wipes it.
void test_history_banks() {
  printf("history banks\n");
  history_reset(HistoryBank::Live);
  history_reset(HistoryBank::Day);
  history_set_timezone(HistoryBank::Live, 0);
  history_set_timezone(HistoryBank::Day, 0);

  // Computed rather than written out: with the offset at 0 a local midnight is a
  // whole multiple of a day, and picking a round-looking number instead only
  // tests that history_window disagrees with the fixture.
  const uint32_t today = (28928100u / 1440u) * 1440u;
  const uint32_t yesterday = today - 1440;

  for (uint32_t m = 0; m <= 600; ++m) {
    history_put(HistoryBank::Live, HistorySeries::Pv, today + m, 2.0f);
  }
  for (uint32_t m = 0; m < 1440; ++m) {
    history_put(HistoryBank::Day, HistorySeries::Pv, yesterday + m, 5.0f);
  }

  // Filling the day bank must not have touched today, which is exactly what a
  // single ring could not manage.
  check(history_head_minute(HistoryBank::Live) == today + 600, "live head untouched");
  check(history_head_minute(HistoryBank::Day) == yesterday + 1439, "day head is yesterday");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 601,
        "live bank keeps its samples");
  check(history_sample_count(HistoryBank::Day, HistorySeries::Pv) == 1440,
        "day bank holds a whole day");

  uint32_t from = 0;
  uint32_t to = 0;
  check(history_window(HistoryBank::Live, &from, &to) && from == today,
        "live window anchors to today");
  check(history_window(HistoryBank::Day, &from, &to) && from == yesterday,
        "day window anchors to yesterday");

  // Values stay with their own bank.
  HistoryColumn columns[4];
  history_reduce(HistoryBank::Day, HistorySeries::Pv, yesterday, yesterday + 1440, columns, 4);
  check(columns[0].known && columns[0].max_value == 5.0f, "day bank reads back its own value");
  history_reduce(HistoryBank::Live, HistorySeries::Pv, today, today + 1440, columns, 4);
  check(columns[0].known && columns[0].max_value == 2.0f, "live bank reads back its own value");

  // The view is what the charts follow, and switching it must not disturb data.
  history_set_view(HistoryBank::Day);
  check(history_view() == HistoryBank::Day, "view follows the setter");
  history_set_view(HistoryBank::Live);

  // Clearing one leaves the other alone — stepping back to today must not cost
  // the day that was loaded.
  history_reset(HistoryBank::Day);
  check(history_sample_count(HistoryBank::Day, HistorySeries::Pv) == 0, "day bank cleared");
  check(history_sample_count(HistoryBank::Live, HistorySeries::Pv) == 601,
        "clearing the day bank spares the live one");

  history_reset(HistoryBank::Live);
}

// The button gesture recogniser, driven by synthetic timelines.
//
// Worth testing rather than eyeballing on hardware: it shipped with two bugs a
// finger found and reading did not.
namespace gesture {

// Feeds a level for `ms` milliseconds at a 5 ms tick, the rate loop() polls at,
// and records every gesture it produces.
struct Recorder {
  ButtonGestureState state;
  uint32_t now = 1000;  // not zero, so an uninitialised timestamp would show up
  int presses = 0;
  int holds = 0;
  int long_holds = 0;

  void feed(bool down, uint32_t ms) {
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 5) {
      switch (state.update(down, now)) {
        case ButtonGesture::Press: ++presses; break;
        case ButtonGesture::Hold: ++holds; break;
        case ButtonGesture::LongHold: ++long_holds; break;
        default: break;
      }
      now += 5;
    }
  }
  void tap(uint32_t ms) { feed(true, ms); feed(false, 5); }
};

}  // namespace gesture

void test_button_gestures() {
  printf("button gestures\n");
  // BOOT's shape: a bouncing GPIO, two seconds and five.
  const ButtonGestureConfig boot{40, 2000, 5000};

  {  // A press fires the moment the button comes up, with nothing to wait for.
    // This is what dropping the double press bought.
    gesture::Recorder r{{boot}};
    r.tap(120);
    check(r.presses == 1, "a press fires on release");
    r.feed(false, 600);
    check(r.presses == 1, "and only once");
    check(r.holds == 0 && r.long_holds == 0, "and is not a hold");
  }

  {  // Two taps in quick succession are simply two presses now.
    gesture::Recorder r{{boot}};
    r.tap(120);
    r.feed(false, 150);
    r.tap(120);
    r.feed(false, 300);
    check(r.presses == 2, "quick taps are two presses, not a double");
  }

  {  // Held past two seconds and let go before five.
    gesture::Recorder r{{boot}};
    r.feed(true, 2500);
    r.feed(false, 600);
    check(r.holds == 1, "a two second hold fires once");
    check(r.long_holds == 0, "and does not reach the long one");
    check(r.presses == 0, "and its release is not also a press");
  }

  {  // Held through both. The short one fires on the way, which is deliberate:
    // it is the feedback that says the button is working, and both long actions
    // end with the device restarting or powering off anyway.
    gesture::Recorder r{{boot}};
    r.feed(true, 5500);
    r.feed(false, 600);
    check(r.holds == 1 && r.long_holds == 1, "holding through fires both, once each");
    check(r.presses == 0, "and never a press");
  }

  {  // Just short of the threshold is still a press, not a hold.
    gesture::Recorder r{{boot}};
    r.feed(true, 1900);
    r.feed(false, 300);
    check(r.presses == 1 && r.holds == 0, "just under two seconds is a press");
  }

  {  // Debounce rejects contact noise on the raw GPIO...
    gesture::Recorder r{{boot}};
    r.feed(true, 10);
    r.feed(false, 300);
    check(r.presses == 0, "a bounce is not a press");
  }

  {  // ...but the PMIC button has none, because power.cpp can only report a tap
    // seen whole between two polls as lasting a single loop iteration.
    gesture::Recorder r{{ButtonGestureConfig{0, 2000, 5000}}};
    r.feed(true, 5);
    r.feed(false, 300);
    check(r.presses == 1, "an undebounced button keeps its shortest tap");
  }

  {  // A poll gap wide enough to skip both thresholds reports the one the finger
    // earned rather than the one it passed through.
    gesture::Recorder r{{boot}};
    r.state.update(true, 10000);
    check(r.state.update(true, 16000) == ButtonGesture::LongHold,
          "a skipped poll still reports the long hold");
  }
}

// --- solar forecast --------------------------------------------------------
//
// Every expected figure here was produced by running the server's own
// app/solar.py over the same inputs, not derived independently. That is the
// point of the test: the model is a port, and what matters is not that it is a
// defensible forecast but that it is the *same* forecast — one house showing two
// different numbers on two screens would be worse than showing none.

void test_solar_forecast() {
  printf("solar forecast\n");

  constexpr float LAT = 51.5072f;   // London, the reference site
  constexpr float LON = -0.1276f;

  float elevation = 0.0f;
  float azimuth = 0.0f;

  // Summer solstice, near solar noon: the year's highest sun, and almost exactly
  // due south — which is the check that the azimuth convention did not come
  // across 180 degrees out.
  solar_position(1782043200u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, 61.9399f, "solstice noon elevation");
  check_near(azimuth, 179.0198f, "solstice noon azimuth is due south");

  // A day either side, to catch a declination that is out by one day: the
  // difference is small and a whole-day slip would still look like a plausible
  // number on its own.
  solar_position(1781956800u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, 61.9352f, "day before the solstice");

  // Midnight, and midnight in the far half of the year — the sun is well below
  // the horizon and the elevation has to go negative rather than clamp.
  solar_position(1755302400u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, -25.0308f, "midnight is below the horizon");
  solar_position(1798848000u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, -61.4097f, "midwinter midnight");

  // Morning, sun low in the east.
  solar_position(1755324000u, LAT, LON, &elevation, &azimuth);
  check_near(elevation, 9.8175f, "morning elevation");
  check_near(azimuth, 80.6894f, "morning azimuth is east of south");

  check_near(solar_poa_factor(40.0f, 180.0f, 35.0f, 180.0f), 0.965926f,
             "sun square-on to a south-facing array");
  check_near(solar_poa_factor(20.0f, 120.0f, 35.0f, 180.0f), 0.549659f, "sun off to one side");
  check_near(solar_poa_factor(5.0f, 90.0f, 90.0f, 270.0f), 0.0f, "sun behind the panel");
  check_near(solar_poa_factor(-3.0f, 180.0f, 35.0f, 180.0f), 0.0f, "sun below the horizon");

  // A synthetic day: a triangular irradiance profile peaking at midday. Made up
  // rather than fetched, so the test needs no network and the same numbers can be
  // put through the Python.
  constexpr uint32_t DAY = 1755302400u;  // 2025-08-16 00:00 UTC
  SolarHour hours[24];
  for (int h = 0; h < 24; ++h) {
    const float distance = static_cast<float>(h < 12 ? 12 - h : h - 12);
    hours[h].direct_normal = 900.0f - distance * 120.0f;
    hours[h].diffuse = 200.0f - distance * 20.0f;
    if (hours[h].direct_normal < 0.0f) {
      hours[h].direct_normal = 0.0f;
    }
    if (hours[h].diffuse < 0.0f) {
      hours[h].diffuse = 0.0f;
    }
  }

  SolarSite site;
  site.latitude = LAT;
  site.longitude = LON;
  site.system_loss = 0.85f;
  site.array_count = 2;
  site.array[0] = {5.4f, 35.0f, 180.0f};
  site.array[1] = {2.0f, 20.0f, 90.0f};  // a second roof facing east

  float slots[SOLAR_SLOTS_PER_DAY];
  solar_forecast_day(site, DAY, hours, 24, DAY, slots);
  SolarSummary summary = solar_summarise(slots, DAY, DAY);
  check_within(summary.forecast_kwh, 40.924067f, 0.001f, "two arrays over a day");
  check_within(summary.peak_kw, 6.393418f, 0.001f, "peak slot as kW");
  check(summary.remaining_kwh == summary.forecast_kwh, "all of it is still to come at midnight");

  summary = solar_summarise(slots, DAY, DAY + 12 * 3600);
  check_within(summary.remaining_kwh, 21.676796f, 0.001f, "remaining from midday");

  // The cap clips the combined throughput, so it shows up in the peak as well as
  // the total — an AC-only reading of it would leave the peak alone.
  site.inverter_cap_kw = 3.0f;
  solar_forecast_day(site, DAY, hours, 24, DAY, slots);
  summary = solar_summarise(slots, DAY, DAY);
  check_within(summary.forecast_kwh, 29.768547f, 0.001f, "clipped by the inverter cap");
  check_within(summary.peak_kw, 3.0f, 0.001f, "peak is the cap itself");

  site.inverter_cap_kw = 0.0f;
  site.array_count = 1;
  solar_forecast_day(site, DAY, hours, 24, DAY, slots);
  summary = solar_summarise(slots, DAY, DAY);
  check_within(summary.forecast_kwh, 31.161450f, 0.001f, "one array alone");

  // Hours that were not fetched contribute nothing rather than repeating the last
  // one: a truncated response should shorten the curve, not extend a plateau.
  solar_forecast_day(site, DAY, hours, 13, DAY, slots);
  summary = solar_summarise(slots, DAY, DAY);
  check(summary.forecast_kwh > 0.0f && summary.forecast_kwh < 31.0f,
        "a short response forecasts only the hours it covers");
  check(slots[SOLAR_SLOTS_PER_DAY - 1] == 0.0f, "slots past the response stay empty");

  // No arrays is not a dark day, it is nothing to forecast — and it must not read
  // back whatever the previous call left in the buffer.
  site.array_count = 0;
  solar_forecast_day(site, DAY, hours, 24, DAY, slots);
  summary = solar_summarise(slots, DAY, DAY);
  check(summary.forecast_kwh == 0.0f, "no arrays forecasts nothing");
}

// --- screen-off window -----------------------------------------------------

void test_screen_window() {
  printf("screen window\n");

  // 22:30 to 07:00 — the case anyone actually sets, and the one that wraps.
  constexpr uint16_t NIGHT_FROM = 22 * 60 + 30;
  constexpr uint16_t NIGHT_TO = 7 * 60;
  check(screen_window_contains(NIGHT_FROM, NIGHT_TO, 23 * 60), "23:00 is inside an overnight window");
  check(screen_window_contains(NIGHT_FROM, NIGHT_TO, 3 * 60), "03:00 is inside it");
  check(screen_window_contains(NIGHT_FROM, NIGHT_TO, NIGHT_FROM), "the start minute is inside");
  check(!screen_window_contains(NIGHT_FROM, NIGHT_TO, NIGHT_TO), "the end minute is outside");
  check(!screen_window_contains(NIGHT_FROM, NIGHT_TO, 12 * 60), "midday is outside");
  check(!screen_window_contains(NIGHT_FROM, NIGHT_TO, NIGHT_FROM - 1),
        "the minute before the start is outside");

  // Wholly within a day, which is the easy case but still has to work.
  check(screen_window_contains(60, 360, 120), "02:00 is inside 01:00-06:00");
  check(!screen_window_contains(60, 360, 30), "00:30 is outside it");
  check(!screen_window_contains(60, 360, 400), "06:40 is outside it");

  // An empty window blanks nothing. The alternative reading — a whole day — is
  // the worst thing a mistyped pair of equal times could do.
  check(!screen_window_contains(600, 600, 600), "equal times are an empty window");
  check(!screen_window_contains(600, 600, 0), "equal times blank nothing at all");

  // Midnight itself, at both ends.
  check(screen_window_contains(1380, 0, 1400), "a window ending at midnight holds 23:20");
  check(!screen_window_contains(1380, 0, 0), "and lets go at midnight exactly");

  // 2026-08-16 00:00:00 UTC. The offsets below are the ones that break a naive
  // implementation: an hour east pushes into the next day, and anything west
  // pushes into the previous one, where unsigned arithmetic wraps to seventy
  // million minutes and puts the device in the afternoon.
  constexpr uint32_t MIDNIGHT_UTC = 1786924800u;
  check(screen_local_minute(MIDNIGHT_UTC, 0) == 0, "UTC midnight is minute 0");
  check(screen_local_minute(MIDNIGHT_UTC, 60) == 60, "one hour east is 01:00");
  check(screen_local_minute(MIDNIGHT_UTC, -300) == 19 * 60, "New York is 19:00 the day before");
  check(screen_local_minute(MIDNIGHT_UTC, -1) == 1439, "one minute west wraps to 23:59");
  check(screen_local_minute(MIDNIGHT_UTC, 345) == 345, "Kathmandu's three quarters of an hour");
  check(screen_local_minute(MIDNIGHT_UTC + 13 * 3600 + 37 * 60, 60) == 14 * 60 + 37,
        "a time mid-afternoon");
  // Sydney in summer is +11, so UTC 14:00 is 01:00 the next day — the wrap in
  // the other direction.
  check(screen_local_minute(MIDNIGHT_UTC + 14 * 3600, 660) == 60, "wrapping forward past midnight");
}

}  // namespace

int run_selftest() {
  s_failures = 0;
  s_checks = 0;
  test_decode();
  test_plan();
  test_snapshot();
  test_day_baseline();
  test_history();
  test_day_series();
  test_history_banks();
  test_button_gestures();
  test_solar_forecast();
  test_screen_window();
  printf("\n%d checks, %d failed\n", s_checks, s_failures);
  return s_failures == 0 ? 0 : 1;
}

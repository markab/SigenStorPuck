// Checks for the parts of the Modbus path that do not need a plant (PLAN.md §D4).
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

#include "day_series.h"
#include "history.h"
#include "modbus_regs.h"

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

  history_reset();
  check(history_head_minute() == 0, "empty ring has no head");
  check(history_sample_count(HistorySeries::Pv) == 0, "empty ring has no samples");

  uint32_t from = 0;
  uint32_t to = 0;
  check(!history_window(&from, &to), "no window before anything is recorded");

  // A day anchored to UTC midnight: minute 30000000 is a round day boundary
  // only by construction, so pick one and work relative to it.
  const uint32_t midnight = (30000000u / 1440u) * 1440u;
  history_set_timezone(0);
  for (uint32_t m = 0; m <= 600; ++m) {
    history_put(HistorySeries::Pv, midnight + m, static_cast<float>(m) / 100.0f);
  }
  check(history_head_minute() == midnight + 600, "head follows the newest sample");
  check(history_sample_count(HistorySeries::Pv) == 601, "every minute recorded");

  check(history_window(&from, &to), "window available once recorded");
  check(from == midnight, "window anchors to local midnight");
  check(to == midnight + 1440, "window covers the whole day");

  // Reduction: 1440 minutes onto 144 columns is ten minutes each, and only the
  // first 601 minutes hold anything.
  HistoryColumn columns[144];
  history_reduce(HistorySeries::Pv, from, to, columns, 144);
  check(columns[0].known, "first column has data");
  check_near(columns[0].min_value, 0.0f, "first column min");
  check_near(columns[0].max_value, 0.09f, "first column max");
  check(!columns[100].known, "columns past the last sample stay empty");
  check(columns[59].known, "last populated column");

  // A gap must not be bridged.
  history_reset();
  history_set_timezone(0);
  history_put(HistorySeries::Soc, midnight + 10, 50.0f);
  history_put(HistorySeries::Soc, midnight + 400, 60.0f);
  history_reduce(HistorySeries::Soc, midnight, midnight + 1440, columns, 144);
  check(columns[1].known, "sample at minute 10 lands in column 1");
  check(!columns[20].known, "the gap between them stays a gap");

  // A clock jump backwards of more than a day starts again rather than
  // interleaving two eras of samples in the same ring.
  const uint32_t before = history_generation();
  history_put(HistorySeries::Soc, midnight - 5000, 42.0f);
  check(history_generation() != before, "a backwards clock jump resets the ring");

  history_reset();
}

// A day payload straight from /api/day/series, at the shape the device asks for.
// Four 15-minute slots is enough to exercise every branch and short enough to
// read; the device uses 5-minute slots and 288 of them.
void test_day_series() {
  printf("day series\n");
  history_reset();

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
  check(day_series_parse(json, strlen(json), now), "day payload parses");

  check(history_head_minute() == now, "the ring stops at now, not at the end of the day");
  check(history_sample_count(HistorySeries::Pv) == 41,
        "every elapsed minute filled, and none beyond");

  // A null slot is a gap, not a zero: SoC was not recorded for the first quarter
  // hour, and drawing that as 0 % would claim the battery was flat at midnight.
  check(history_sample_count(HistorySeries::Soc) == 26, "null slots leave gaps");

  HistoryColumn columns[4];
  history_reduce(HistorySeries::Pv, midnight_minute, midnight_minute + 60, columns, 4);
  check(columns[0].known && columns[0].max_value == 0.0f, "slot 0 is a recorded zero");
  check(columns[1].known && columns[1].max_value == 1.5f, "slot 1 carries its value");
  check(columns[2].known && columns[2].max_value == 3.0f, "the part-elapsed slot is filled");
  check(!columns[3].known, "the slot that has not happened stays empty");

  // The timezone came from the payload, which is the only place the server path
  // gets a trustworthy one.
  uint32_t from = 0;
  uint32_t to = 0;
  check(history_window(&from, &to), "window available");
  check(from == midnight_minute && to == midnight_minute + 1440,
        "window anchors to the day the payload named");

  check(!day_series_parse("{\"slot_minutes\":15}", 19, now), "a payload with no day is rejected");
  check(!day_series_parse(json, strlen(json), 0), "no clock means nothing is filed");

  history_reset();
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
  printf("\n%d checks, %d failed\n", s_checks, s_failures);
  return s_failures == 0 ? 0 : 1;
}

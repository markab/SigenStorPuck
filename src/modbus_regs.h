// The subset of the Sigenergy register map the Puck reads, and everything that
// turns raw words into a Snapshot (docs/PLAN.md §D2).
//
// About twenty registers against the server's several hundred. The full map,
// with every device type and write support, lives in SigenStorDisplay's
// app/modbus/registers/ and is not worth porting: the Puck renders, it does not
// administer a plant.
//
// Lives outside src/device/ so the simulator compiles it. The socket is the only
// part that needs Arduino, and it is the only part that cannot be tested on a
// desktop.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"

// Sigenergy maps function codes the *reverse* of the usual convention, matching
// the leading digit of the address range: 0x03 reads the read-only 30000-range
// and 0x04 reads the holding 40000-range. In standard Modbus naming those are
// read_holding_registers and read_input_registers respectively, so every library
// you might reach for has them the other way round.
//
// Transcribed from app/modbus/registers/base.py:13, which verified it against
// the worked hex examples in section 6.1 of the protocol PDF. Getting this
// backwards is the most likely first bug on real hardware.
static constexpr uint8_t MODBUS_FC_READ_ONLY = 0x03;
static constexpr uint8_t MODBUS_FC_HOLDING = 0x04;

// The protocol's ceiling on one read. The batcher merges anything that fits
// inside this, however wide the gap.
static constexpr uint16_t MODBUS_MAX_WORDS = 124;

// The plant's own Modbus address; individual devices use 1-246, set in the Sigen
// app.
static constexpr uint8_t MODBUS_PLANT_ADDRESS = 247;

enum class ModbusType : uint8_t { U16, S16, U32, S32, U64 };

enum class ModbusScope : uint8_t { Plant, Inverter, AcCharger };

// Which poll cycle a register belongs to. Fast is everything the power and
// battery screens need; slow is the Today screen, which does not change fast
// enough to be worth a request every cycle.
enum class ModbusCadence : uint8_t { Fast, Slow };

enum ModbusKey : uint8_t {
  // --- plant, fast: all inside one 94-word read ---------------------------
  MB_PLANT_SYSTEM_TIME = 0,
  MB_PLANT_TIMEZONE,
  MB_PLANT_GRID_POWER,
  MB_PLANT_ON_OFF_GRID,
  MB_PLANT_ESS_SOC,
  MB_PLANT_ALARM1,
  MB_PLANT_ALARM2,
  MB_PLANT_ALARM3,
  MB_PLANT_ALARM4,
  MB_PLANT_ACTIVE_POWER,
  MB_PLANT_PV_POWER,
  MB_PLANT_ESS_POWER,
  MB_PLANT_ALARM5,
  MB_PLANT_ESS_CAPACITY,
  MB_PLANT_ESS_SOH,
  MB_PLANT_LOAD_DAILY,

  // --- plant, slow --------------------------------------------------------
  //
  // Lifetime accumulators, not daily ones: there is no daily import or export
  // register, so today's figures need a midnight baseline subtracted.
  MB_PLANT_TOTAL_IMPORTED,
  MB_PLANT_TOTAL_EXPORTED,

  // --- per inverter -------------------------------------------------------
  MB_INV_ESS_DAILY_CHARGE,
  MB_INV_ESS_DAILY_DISCHARGE,
  MB_INV_ESS_MAX_TEMP,
  MB_INV_DC_OUTPUT_POWER,  // fast: feeds EV, which house load is derived from
  MB_INV_PV_DAILY_GEN,

  // --- per AC charger -----------------------------------------------------
  MB_ACC_CHARGING_POWER,  // fast, same reason

  MB_KEY_COUNT,
};

struct ModbusReg {
  uint8_t key;
  uint16_t address;
  uint8_t words;
  ModbusType type;
  // Engineering value is raw / gain, matching the server's register map.
  int32_t gain;
  ModbusScope scope;
  ModbusCadence cadence;
};

extern const ModbusReg MODBUS_REGS[MB_KEY_COUNT];

// One batched read.
struct ModbusSpan {
  uint16_t start = 0;
  uint16_t words = 0;
};

// Merges the registers matching `scope` and `cadence` into as few reads as fit
// under MODBUS_MAX_WORDS.
//
// Deliberately merges across any gap that fits, unlike the server's batcher,
// which stops at a gap of 16. The protocol mandates a 1000 ms minimum request
// period, so a wasted word is free and a wasted request costs a full second —
// the opposite trade to a server sharing one connection between many callers.
//
// Returns the number of spans written.
size_t modbus_plan(ModbusScope scope, ModbusCadence cadence, ModbusSpan* out, size_t max_spans);

// Decodes one register out of a span's words. `span_start` is the address the
// words begin at. Returns false when the register does not fall entirely inside
// the span.
bool modbus_decode(const ModbusReg& reg, uint16_t span_start, const uint16_t* words,
                   size_t word_count, float* out);

// Everything read this cycle, before it becomes a Snapshot.
//
// Plant registers are single-valued; the per-device ones are summed or averaged
// across however many inverters and chargers are configured, which is why they
// cannot just live in the same array.
struct ModbusValues {
  bool known[MB_KEY_COUNT] = {};
  float value[MB_KEY_COUNT] = {};

  float ev_kw = 0.0f;
  bool ev_any = false;

  float temp_sum = 0.0f;
  int temp_count = 0;

  float pv_daily = 0.0f;
  bool pv_daily_any = false;

  float ess_charge = 0.0f;
  bool ess_charge_any = false;

  float ess_discharge = 0.0f;
  bool ess_discharge_any = false;
};

// Folds one completed read into the accumulator. Plant registers overwrite;
// per-device ones add, so calling this once per device sums them the way
// summary.py's _ev_power and _avg_inverter do.
void modbus_apply(ModbusValues* values, ModbusScope scope, uint16_t span_start,
                  const uint16_t* words, size_t word_count);

// Today's grid import and export, in kWh, worked out by subtracting a baseline
// latched at local midnight from the lifetime accumulators.
struct ModbusDayBaseline {
  bool known = false;
  uint32_t day = 0;  // local days since the epoch, so a change means a new day
  float imported = 0.0f;
  float exported = 0.0f;
};

// Updates the baseline if the local day has rolled over, and returns today's
// figures through `imported`/`exported`.
//
// The monotonicity guard matters: plant.py notes the new-statistics counters
// reset to 0 on a firmware upgrade, and subtracting yesterday's baseline from a
// counter that has gone back to zero would report a hugely negative day. A
// counter below its own baseline re-latches instead.
void modbus_day_totals(ModbusDayBaseline* baseline, uint32_t local_day, float total_imported,
                       float total_exported, bool totals_known, MaybeFloat* imported,
                       MaybeFloat* exported);

// Turns a cycle's readings into the Snapshot the screens already understand.
//
// `home` and `eta_min` are the only derived figures, and both mirror
// summary.py's _power_block and _eta_minutes exactly. In particular house load
// subtracts EV, because EV is reported as its own leg — getting that wrong
// silently inflates the house figure and nothing on screen would look wrong.
void modbus_to_snapshot(const ModbusValues& values, Snapshot* out);

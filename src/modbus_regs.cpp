#include "modbus_regs.h"

namespace {

constexpr ModbusScope PLANT = ModbusScope::Plant;
constexpr ModbusScope INV = ModbusScope::Inverter;
constexpr ModbusScope ACC = ModbusScope::AcCharger;
constexpr ModbusCadence FAST = ModbusCadence::Fast;
constexpr ModbusCadence SLOW = ModbusCadence::Slow;

// Reads a big-endian word sequence as an unsigned integer. Sigenergy puts the
// most significant register first.
uint64_t words_to_uint(const uint16_t* words, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i) {
    value = (value << 16) | words[i];
  }
  return value;
}

double to_signed(uint64_t raw, unsigned bits) {
  const uint64_t sign_bit = static_cast<uint64_t>(1) << (bits - 1);
  if (raw & sign_bit) {
    // Widened to double before subtracting, so a full-scale negative does not
    // overflow on the way out.
    return static_cast<double>(raw) - static_cast<double>(static_cast<uint64_t>(1) << bits);
  }
  return static_cast<double>(raw);
}

int popcount16(uint16_t value) {
  int bits = 0;
  while (value != 0) {
    bits += value & 1u;
    value >>= 1;
  }
  return bits;
}

void set(ModbusValues* values, uint8_t key, float value) {
  values->known[key] = true;
  values->value[key] = value;
}

MaybeFloat maybe(const ModbusValues& values, uint8_t key) {
  MaybeFloat out;
  out.known = values.known[key];
  out.value = out.known ? values.value[key] : 0.0f;
  return out;
}

// Battery time-to-full or time-to-empty, straight from summary.py's
// _eta_minutes: a straight-line estimate that ignores the taper near the ends,
// withheld at trickle rates where it would read as an absurd number of hours.
constexpr float ETA_MIN_POWER_KW = 0.05f;
constexpr int32_t ETA_MAX_MINUTES = 24 * 60;

MaybeInt eta_minutes(const MaybeFloat& soc, const MaybeFloat& capacity, const MaybeFloat& power) {
  MaybeInt out;
  if (!soc.known || !capacity.known || !power.known || capacity.value == 0.0f) {
    return out;
  }
  const float magnitude = power.value < 0.0f ? -power.value : power.value;
  if (magnitude < ETA_MIN_POWER_KW) {
    return out;
  }
  const float remaining_pct = power.value > 0.0f ? (100.0f - soc.value) : soc.value;
  const float minutes = (capacity.value * remaining_pct / 100.0f) / magnitude * 60.0f;
  if (minutes <= 0.0f || minutes > static_cast<float>(ETA_MAX_MINUTES)) {
    return out;
  }
  out.known = true;
  out.value = static_cast<int32_t>(minutes + 0.5f);
  return out;
}

}  // namespace

// Addresses, word counts, types and gains transcribed from SigenStorDisplay's
// app/modbus/registers/{plant,hybrid_inverter,ac_charger}.py. Kept in ascending
// address order within each scope, which is what lets modbus_plan() walk them
// once.
const ModbusReg MODBUS_REGS[MB_KEY_COUNT] = {
    {MB_PLANT_SYSTEM_TIME, 30000, 2, ModbusType::U32, 1, PLANT, FAST},
    {MB_PLANT_TIMEZONE, 30002, 1, ModbusType::S16, 1, PLANT, FAST},
    {MB_PLANT_GRID_POWER, 30005, 2, ModbusType::S32, 1000, PLANT, FAST},
    {MB_PLANT_ON_OFF_GRID, 30009, 1, ModbusType::U16, 1, PLANT, FAST},
    {MB_PLANT_ESS_SOC, 30014, 1, ModbusType::U16, 10, PLANT, FAST},
    {MB_PLANT_ALARM1, 30027, 1, ModbusType::U16, 1, PLANT, FAST},
    {MB_PLANT_ALARM2, 30028, 1, ModbusType::U16, 1, PLANT, FAST},
    {MB_PLANT_ALARM3, 30029, 1, ModbusType::U16, 1, PLANT, FAST},
    {MB_PLANT_ALARM4, 30030, 1, ModbusType::U16, 1, PLANT, FAST},
    {MB_PLANT_ACTIVE_POWER, 30031, 2, ModbusType::S32, 1000, PLANT, FAST},
    {MB_PLANT_PV_POWER, 30035, 2, ModbusType::S32, 1000, PLANT, FAST},
    {MB_PLANT_ESS_POWER, 30037, 2, ModbusType::S32, 1000, PLANT, FAST},
    {MB_PLANT_ALARM5, 30072, 1, ModbusType::U16, 1, PLANT, FAST},
    {MB_PLANT_ESS_CAPACITY, 30083, 2, ModbusType::U32, 100, PLANT, FAST},
    {MB_PLANT_ESS_SOH, 30087, 1, ModbusType::U16, 10, PLANT, FAST},
    {MB_PLANT_LOAD_DAILY, 30092, 2, ModbusType::U32, 100, PLANT, FAST},

    {MB_PLANT_TOTAL_IMPORTED, 30216, 4, ModbusType::U64, 100, PLANT, SLOW},
    {MB_PLANT_TOTAL_EXPORTED, 30220, 4, ModbusType::U64, 100, PLANT, SLOW},

    {MB_INV_ESS_DAILY_CHARGE, 30566, 2, ModbusType::U32, 100, INV, SLOW},
    {MB_INV_ESS_DAILY_DISCHARGE, 30572, 2, ModbusType::U32, 100, INV, SLOW},
    {MB_INV_ESS_MAX_TEMP, 30620, 1, ModbusType::S16, 10, INV, SLOW},
    {MB_INV_DC_OUTPUT_POWER, 31502, 2, ModbusType::S32, 1000, INV, FAST},
    {MB_INV_PV_DAILY_GEN, 31509, 2, ModbusType::U32, 100, INV, SLOW},

    {MB_ACC_CHARGING_POWER, 32003, 2, ModbusType::S32, 1000, ACC, FAST},
};

size_t modbus_plan(ModbusScope scope, ModbusCadence cadence, ModbusSpan* out, size_t max_spans) {
  if (out == nullptr || max_spans == 0) {
    return 0;
  }
  size_t count = 0;
  bool open = false;
  uint16_t start = 0;
  uint16_t end = 0;  // exclusive

  for (size_t i = 0; i < MB_KEY_COUNT; ++i) {
    const ModbusReg& reg = MODBUS_REGS[i];
    if (reg.scope != scope || reg.cadence != cadence) {
      continue;
    }
    const uint16_t reg_end = static_cast<uint16_t>(reg.address + reg.words);

    if (open && (reg_end - start) <= MODBUS_MAX_WORDS) {
      if (reg_end > end) {
        end = reg_end;
      }
      continue;
    }
    if (open) {
      out[count].start = start;
      out[count].words = static_cast<uint16_t>(end - start);
      if (++count == max_spans) {
        return count;
      }
    }
    open = true;
    start = reg.address;
    end = reg_end;
  }

  if (open) {
    out[count].start = start;
    out[count].words = static_cast<uint16_t>(end - start);
    ++count;
  }
  return count;
}

bool modbus_decode(const ModbusReg& reg, uint16_t span_start, const uint16_t* words,
                   size_t word_count, float* out) {
  if (words == nullptr || out == nullptr || reg.address < span_start) {
    return false;
  }
  const size_t offset = static_cast<size_t>(reg.address - span_start);
  if (offset + reg.words > word_count) {
    return false;
  }

  const uint64_t raw = words_to_uint(words + offset, reg.words);
  double value = 0.0;
  switch (reg.type) {
    case ModbusType::S16:
      value = to_signed(raw, 16);
      break;
    case ModbusType::S32:
      value = to_signed(raw, 32);
      break;
    case ModbusType::U16:
    case ModbusType::U32:
    case ModbusType::U64:
      value = static_cast<double>(raw);
      break;
  }
  if (reg.gain != 0 && reg.gain != 1) {
    value /= static_cast<double>(reg.gain);
  }
  *out = static_cast<float>(value);
  return true;
}

void modbus_apply(ModbusValues* values, ModbusScope scope, uint16_t span_start,
                  const uint16_t* words, size_t word_count) {
  if (values == nullptr || words == nullptr) {
    return;
  }
  for (size_t i = 0; i < MB_KEY_COUNT; ++i) {
    const ModbusReg& reg = MODBUS_REGS[i];
    if (reg.scope != scope) {
      continue;
    }
    float decoded = 0.0f;
    if (!modbus_decode(reg, span_start, words, word_count, &decoded)) {
      continue;
    }

    // Plant registers are single-valued; per-device ones accumulate, so that a
    // plant with two inverters reports one plant and the sum of both.
    switch (reg.key) {
      case MB_ACC_CHARGING_POWER:
      case MB_INV_DC_OUTPUT_POWER:
        values->ev_kw += decoded;
        values->ev_any = true;
        break;
      case MB_INV_ESS_MAX_TEMP:
        // Averaged rather than summed, matching summary.py's _avg_inverter.
        values->temp_sum += decoded;
        ++values->temp_count;
        break;
      case MB_INV_PV_DAILY_GEN:
        values->pv_daily += decoded;
        values->pv_daily_any = true;
        break;
      case MB_INV_ESS_DAILY_CHARGE:
        values->ess_charge += decoded;
        values->ess_charge_any = true;
        break;
      case MB_INV_ESS_DAILY_DISCHARGE:
        values->ess_discharge += decoded;
        values->ess_discharge_any = true;
        break;
      default:
        set(values, reg.key, decoded);
        break;
    }
  }
}

void modbus_day_totals(ModbusDayBaseline* baseline, uint32_t local_day, float total_imported,
                       float total_exported, bool totals_known, MaybeFloat* imported,
                       MaybeFloat* exported) {
  if (imported != nullptr) {
    *imported = MaybeFloat{};
  }
  if (exported != nullptr) {
    *exported = MaybeFloat{};
  }
  if (baseline == nullptr || !totals_known) {
    return;
  }

  const bool new_day = !baseline->known || baseline->day != local_day;
  // A counter below its own baseline means it was reset under us — a firmware
  // upgrade clears the new-statistics registers. Re-latch rather than report a
  // negative day.
  const bool went_backwards = baseline->known && (total_imported < baseline->imported ||
                                                  total_exported < baseline->exported);
  if (new_day || went_backwards) {
    baseline->known = true;
    baseline->day = local_day;
    baseline->imported = total_imported;
    baseline->exported = total_exported;
  }

  if (imported != nullptr) {
    imported->known = true;
    imported->value = total_imported - baseline->imported;
  }
  if (exported != nullptr) {
    exported->known = true;
    exported->value = total_exported - baseline->exported;
  }
}

void modbus_to_snapshot(const ModbusValues& values, Snapshot* out) {
  if (out == nullptr) {
    return;
  }
  Snapshot built;
  built.valid = true;
  built.version = 1;
  built.ok = true;

  if (values.known[MB_PLANT_SYSTEM_TIME]) {
    built.ts = static_cast<uint32_t>(values.value[MB_PLANT_SYSTEM_TIME]);
  }
  if (values.known[MB_PLANT_TIMEZONE]) {
    built.tz_offset_min.known = true;
    built.tz_offset_min.value = static_cast<int32_t>(values.value[MB_PLANT_TIMEZONE]);
  }
  // Read straight off the plant this cycle, so there is no server in between to
  // have gone stale. Age is left at zero for the same reason.
  built.age_s = 0;

  int alarms = 0;
  const uint8_t alarm_keys[] = {MB_PLANT_ALARM1, MB_PLANT_ALARM2, MB_PLANT_ALARM3,
                                MB_PLANT_ALARM4, MB_PLANT_ALARM5};
  for (uint8_t key : alarm_keys) {
    if (values.known[key]) {
      alarms += popcount16(static_cast<uint16_t>(values.value[key]));
    }
  }
  built.alarms = alarms;

  built.power.pv = maybe(values, MB_PLANT_PV_POWER);
  built.power.grid = maybe(values, MB_PLANT_GRID_POWER);
  built.power.batt = maybe(values, MB_PLANT_ESS_POWER);
  built.power.plant = maybe(values, MB_PLANT_ACTIVE_POWER);

  // Zero rather than unknown when no charging kit is configured, matching
  // _ev_power: a plant with no charger genuinely draws no EV power.
  built.power.ev.known = true;
  built.power.ev.value = values.ev_any ? values.ev_kw : 0.0f;

  // 0 = on grid, 1 or 2 = off grid. Unread is not "off grid".
  if (values.known[MB_PLANT_ON_OFF_GRID]) {
    built.power.off_grid = values.value[MB_PLANT_ON_OFF_GRID] != 0.0f;
  }

  // House load, derived exactly as the server derives it: EV comes out because
  // it is reported as its own leg, and the result is floored at zero.
  if (built.power.pv.known && built.power.grid.known && built.power.batt.known) {
    float home = built.power.pv.value - built.power.batt.value + built.power.grid.value -
                 built.power.ev.value;
    if (home < 0.0f) {
      home = 0.0f;
    }
    built.power.home.known = true;
    built.power.home.value = home;
  }

  built.battery.soc_pct = maybe(values, MB_PLANT_ESS_SOC);
  built.battery.soh_pct = maybe(values, MB_PLANT_ESS_SOH);
  built.battery.capacity_kwh = maybe(values, MB_PLANT_ESS_CAPACITY);
  if (values.temp_count > 0) {
    built.battery.temp_c.known = true;
    built.battery.temp_c.value = values.temp_sum / static_cast<float>(values.temp_count);
  }
  built.battery.eta_min =
      eta_minutes(built.battery.soc_pct, built.battery.capacity_kwh, built.power.batt);

  // Today is present once anything in it has been read. Import and export are
  // filled in by the caller, which owns the midnight baseline.
  built.today.present = values.pv_daily_any || values.known[MB_PLANT_LOAD_DAILY] ||
                        values.ess_charge_any || values.ess_discharge_any;
  if (values.pv_daily_any) {
    built.today.solar.known = true;
    built.today.solar.value = values.pv_daily;
  }
  built.today.load = maybe(values, MB_PLANT_LOAD_DAILY);
  if (values.ess_charge_any) {
    built.today.charge.known = true;
    built.today.charge.value = values.ess_charge;
  }
  if (values.ess_discharge_any) {
    built.today.discharge.known = true;
    built.today.discharge.value = values.ess_discharge;
  }

  // No tariff without the server: the cost screen is not built in this mode.
  built.cost.configured = false;

  *out = built;
}

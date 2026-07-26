#include "modbus_api.h"

#include <WiFi.h>

#include "modbus_regs.h"
#include "settings.h"

namespace {

// The protocol mandates a 1000 ms minimum request period. Everything about the
// batching in modbus_regs.cpp exists to keep the number of requests down for
// this reason, so honouring it here is not optional politeness.
constexpr uint32_t MIN_REQUEST_INTERVAL_MS = 1000;

// How often the Today figures are refreshed. Daily totals do not move fast
// enough to be worth three or four extra requests every cycle; at a 5 s poll
// this costs them once in twelve.
constexpr uint32_t SLOW_INTERVAL_MS = 60000;

constexpr uint32_t CONNECT_TIMEOUT_MS = 4000;
constexpr uint32_t RESPONSE_TIMEOUT_MS = 3000;

// MBAP header plus the PDU's own function code and byte count.
constexpr size_t MBAP_LENGTH = 7;
constexpr size_t MAX_SPANS = 4;

WiFiClient s_client;
uint16_t s_transaction = 0;
uint32_t s_last_request_ms = 0;

// The slow cycle's readings, kept between fast cycles so the Today screen does
// not blank out for the 59 seconds it is not being refreshed.
ModbusValues s_slow;
bool s_slow_valid = false;
uint32_t s_slow_at_ms = 0;

ModbusDayBaseline s_baseline;

// Set from the settings page, acted on by the poll task. Everything above is
// touched by the poll task alone, and it must stay that way: the settings page
// runs in loop() on the other core, and stopping a WiFiClient out from under a
// task that is blocked reading it is a crash, not a race you get away with.
volatile bool s_reset_pending = false;

void disconnect() {
  s_client.stop();
}

bool ensure_connected(const Settings& settings) {
  if (s_client.connected()) {
    return true;
  }
  s_client.setTimeout(CONNECT_TIMEOUT_MS / 1000);
  if (!s_client.connect(settings.modbus_host.c_str(), settings.modbus_port,
                        CONNECT_TIMEOUT_MS)) {
    return false;
  }
  // Nagle would hold a 12-byte request back waiting for company that never
  // comes, and every request here is a full round trip.
  s_client.setNoDelay(true);
  return true;
}

// Blocks until `count` bytes have arrived or the deadline passes.
bool read_exactly(uint8_t* buffer, size_t count, uint32_t deadline_ms) {
  size_t have = 0;
  while (have < count) {
    if (static_cast<int32_t>(millis() - deadline_ms) >= 0) {
      return false;
    }
    const int available = s_client.available();
    if (available <= 0) {
      if (!s_client.connected()) {
        return false;
      }
      delay(2);
      continue;
    }
    const int got = s_client.read(buffer + have, count - have);
    if (got <= 0) {
      delay(2);
      continue;
    }
    have += static_cast<size_t>(got);
  }
  return true;
}

// One read of `words` registers starting at `start`, from Modbus address `unit`.
//
// Only the read path exists here, and only FC 0x03. A status display has no
// business writing a register, so there is deliberately nothing to call.
FetchResult read_span(uint8_t unit, uint16_t start, uint16_t words, uint16_t* out, int* detail) {
  if (words == 0 || words > MODBUS_MAX_WORDS) {
    return FetchResult::ProtocolError;
  }

  // The mandated gap, measured from the last request rather than slept
  // unconditionally, so the first read of a cycle is usually free.
  const uint32_t since = millis() - s_last_request_ms;
  if (s_last_request_ms != 0 && since < MIN_REQUEST_INTERVAL_MS) {
    delay(MIN_REQUEST_INTERVAL_MS - since);
  }
  s_last_request_ms = millis();

  ++s_transaction;
  uint8_t request[12];
  request[0] = static_cast<uint8_t>(s_transaction >> 8);
  request[1] = static_cast<uint8_t>(s_transaction & 0xFF);
  request[2] = 0;  // protocol identifier, always 0 for Modbus
  request[3] = 0;
  request[4] = 0;  // length of everything after this field: unit + PDU
  request[5] = 6;
  request[6] = unit;
  // Sigenergy's reversed mapping: 0x03 reads the read-only 30000-range. See the
  // note on MODBUS_FC_READ_ONLY — this is not the standard meaning of 0x03.
  request[7] = MODBUS_FC_READ_ONLY;
  request[8] = static_cast<uint8_t>(start >> 8);
  request[9] = static_cast<uint8_t>(start & 0xFF);
  request[10] = static_cast<uint8_t>(words >> 8);
  request[11] = static_cast<uint8_t>(words & 0xFF);

  if (s_client.write(request, sizeof(request)) != sizeof(request)) {
    return FetchResult::ConnectFailed;
  }
  s_client.flush();

  const uint32_t deadline = millis() + RESPONSE_TIMEOUT_MS;
  uint8_t header[MBAP_LENGTH + 1];
  if (!read_exactly(header, sizeof(header), deadline)) {
    return FetchResult::ReadTimeout;
  }

  const uint16_t transaction = static_cast<uint16_t>((header[0] << 8) | header[1]);
  if (transaction != s_transaction) {
    // A reply to a request we have already given up on. The connection is out of
    // step, so drop it rather than try to resynchronise mid-stream.
    return FetchResult::ProtocolError;
  }

  const uint8_t function = header[7];
  if ((function & 0x80) != 0) {
    uint8_t code = 0;
    if (read_exactly(&code, 1, deadline) && detail != nullptr) {
      *detail = code;
    }
    return FetchResult::ProtocolError;
  }
  if (function != MODBUS_FC_READ_ONLY) {
    return FetchResult::ProtocolError;
  }

  uint8_t byte_count = 0;
  if (!read_exactly(&byte_count, 1, deadline)) {
    return FetchResult::ReadTimeout;
  }
  if (byte_count != words * 2) {
    return FetchResult::ProtocolError;
  }

  // Read straight into the output as bytes, then swap in place: Sigenergy sends
  // the most significant register first, and so does every Modbus device.
  uint8_t* raw = reinterpret_cast<uint8_t*>(out);
  if (!read_exactly(raw, byte_count, deadline)) {
    return FetchResult::ReadTimeout;
  }
  for (uint16_t i = 0; i < words; ++i) {
    out[i] = static_cast<uint16_t>((raw[i * 2] << 8) | raw[i * 2 + 1]);
  }
  return FetchResult::Ok;
}

// Reads every span for one scope and cadence at `unit`, folding the results in.
FetchResult read_scope(uint8_t unit, ModbusScope scope, ModbusCadence cadence,
                       ModbusValues* values, int* detail) {
  ModbusSpan spans[MAX_SPANS];
  const size_t count = modbus_plan(scope, cadence, spans, MAX_SPANS);
  static uint16_t words[MODBUS_MAX_WORDS];

  for (size_t i = 0; i < count; ++i) {
    const FetchResult result = read_span(unit, spans[i].start, spans[i].words, words, detail);
    if (result != FetchResult::Ok) {
      return result;
    }
    modbus_apply(values, scope, spans[i].start, words, spans[i].words);
  }
  return FetchResult::Ok;
}

// Local days since the epoch, for spotting midnight. Falls back to UTC when the
// plant has not reported a timezone, which only shifts the rollover by the
// offset — the day still rolls exactly once a day.
uint32_t local_day_of(const ModbusValues& values) {
  if (!values.known[MB_PLANT_SYSTEM_TIME]) {
    return 0;
  }
  int32_t offset_s = 0;
  if (values.known[MB_PLANT_TIMEZONE]) {
    offset_s = static_cast<int32_t>(values.value[MB_PLANT_TIMEZONE]) * 60;
  }
  const int64_t local = static_cast<int64_t>(values.value[MB_PLANT_SYSTEM_TIME]) + offset_s;
  return local > 0 ? static_cast<uint32_t>(local / 86400) : 0;
}

}  // namespace

void modbus_api_reset() {
  // Only a flag: the poll task owns the connection and the cached readings, and
  // clearing them from here would be reaching into another task's state.
  s_reset_pending = true;
}

FetchResult modbus_api_fetch(Snapshot* out, int* detail) {
  if (detail != nullptr) {
    *detail = 0;
  }
  if (s_reset_pending) {
    s_reset_pending = false;
    s_slow = ModbusValues{};
    s_slow_valid = false;
    s_slow_at_ms = 0;
    s_baseline = ModbusDayBaseline{};
    disconnect();
  }
  const Settings& settings = settings_get();
  if (settings.modbus_host.isEmpty()) {
    return FetchResult::NotConfigured;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return FetchResult::NoNetwork;
  }
  if (!ensure_connected(settings)) {
    return FetchResult::ConnectFailed;
  }

  const bool slow_due = !s_slow_valid || (millis() - s_slow_at_ms) >= SLOW_INTERVAL_MS;
  if (slow_due) {
    // Built fresh rather than added to, so the per-device sums do not grow by a
    // plant's worth every minute.
    ModbusValues slow;
    FetchResult result = read_scope(settings.modbus_plant_address, ModbusScope::Plant,
                                    ModbusCadence::Slow, &slow, detail);
    for (size_t i = 0; result == FetchResult::Ok && i < SETTINGS_MAX_MODBUS_DEVICES; ++i) {
      const ModbusDevice& device = settings.modbus_devices[i];
      if (device.slave_id == 0 || device.type != ModbusDeviceType::Inverter) {
        continue;
      }
      result = read_scope(device.slave_id, ModbusScope::Inverter, ModbusCadence::Slow, &slow,
                          detail);
    }
    if (result != FetchResult::Ok) {
      disconnect();
      return result;
    }
    s_slow = slow;
    s_slow_valid = true;
    s_slow_at_ms = millis();
  }

  // Start from the slow readings and overlay this cycle's fast ones. The two
  // sets are disjoint by construction, so nothing is double-counted.
  ModbusValues values = s_slow;

  FetchResult result = read_scope(settings.modbus_plant_address, ModbusScope::Plant,
                                  ModbusCadence::Fast, &values, detail);
  for (size_t i = 0; result == FetchResult::Ok && i < SETTINGS_MAX_MODBUS_DEVICES; ++i) {
    const ModbusDevice& device = settings.modbus_devices[i];
    if (device.slave_id == 0) {
      continue;
    }
    if (device.type == ModbusDeviceType::AcCharger) {
      result = read_scope(device.slave_id, ModbusScope::AcCharger, ModbusCadence::Fast, &values,
                          detail);
    } else if (device.dc_charger) {
      // Only an inverter with a DC charger fitted has anything at 31502, and
      // only its output counts towards EV — the same detection summary.py's
      // _ev_power does from the configured devices.
      result = read_scope(device.slave_id, ModbusScope::Inverter, ModbusCadence::Fast, &values,
                          detail);
    }
  }
  if (result != FetchResult::Ok) {
    disconnect();
    return result;
  }

  Snapshot built;
  modbus_to_snapshot(values, &built);

  // Import and export are the one pair with no daily register, so they are
  // worked out here against a baseline latched at local midnight.
  MaybeFloat imported;
  MaybeFloat exported;
  const bool totals_known =
      values.known[MB_PLANT_TOTAL_IMPORTED] && values.known[MB_PLANT_TOTAL_EXPORTED];
  modbus_day_totals(&s_baseline, local_day_of(values), values.value[MB_PLANT_TOTAL_IMPORTED],
                    values.value[MB_PLANT_TOTAL_EXPORTED], totals_known, &imported, &exported);
  built.today.imported = imported;
  built.today.exported = exported;
  if (imported.known) {
    built.today.present = true;
  }

  *out = built;
  return FetchResult::Ok;
}

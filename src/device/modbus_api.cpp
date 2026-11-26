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

// How long an inverter's PV strings are left alone after it answers them with an
// exception. An inverter without those registers would otherwise be asked, and
// log the refusal, on every poll; ten minutes keeps the log readable and still
// picks a strings figure back up soon after a firmware update adds it.
constexpr uint32_t STRINGS_RETRY_MS = 10UL * 60UL * 1000UL;
uint32_t s_strings_retry_at_ms[SETTINGS_MAX_MODBUS_DEVICES] = {};

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

// The exception codes a Sigenergy plant can realistically send back. Named
// rather than left as a number, because the number alone sends you reading a
// protocol specification to find out that you addressed a register the device
// does not have.
const char* modbus_exception_name(uint8_t code) {
  switch (code) {
    case 0x01: return "illegal function";
    case 0x02: return "illegal data address";
    case 0x03: return "illegal data value";
    case 0x04: return "device failure";
    case 0x05: return "acknowledge";
    case 0x06: return "device busy";
    case 0x0A: return "gateway path unavailable";
    case 0x0B: return "gateway target did not respond";
    default: return "unknown";
  }
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
    const bool got_code = read_exactly(&code, 1, deadline);
    if (got_code && detail != nullptr) {
      *detail = code;
    }
    // Named, and with the span that caused it. The poll log can only report one
    // number for the whole cycle, and "modbus error (http 2)" gives you nothing
    // to act on: an exception is the plant telling you precisely what it did not
    // like, and which unit and which registers were asked for is the whole of the
    // answer. Printed on every occurrence — a plant answering exceptions is not a
    // steady state, and the poll backoff keeps this to once a minute.
    Serial.printf("[modbus] unit %u rejected %u registers at %u: exception %u (%s)\n",
                  static_cast<unsigned>(unit), static_cast<unsigned>(words),
                  static_cast<unsigned>(start), static_cast<unsigned>(code),
                  modbus_exception_name(code));
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

// Reads one inverter's PV strings and folds them in (modbus_regs.h). The head
// read tells how many there are; a tail read follows only when there are more
// than four.
//
// A Modbus exception is the inverter declining these registers, not a broken
// connection, so it costs this cycle its strings figure — the losses fall back to
// the plant's PV figure — and never the poll. Anything else (timeout, connection
// lost, out-of-step reply) fails the cycle as any other read would.
FetchResult read_strings(size_t device_index, uint8_t unit, ModbusValues* values) {
  uint32_t& retry_at = s_strings_retry_at_ms[device_index];
  if (retry_at != 0 && static_cast<int32_t>(millis() - retry_at) < 0) {
    modbus_strings_failed(values);
    return FetchResult::Ok;
  }
  retry_at = 0;

  static uint16_t head[MB_PV_HEAD_WORDS];
  static uint16_t tail[2 * (MB_PV_MAX_STRINGS - 4)];
  int exception = 0;
  FetchResult result = read_span(unit, MB_PV_HEAD_START, MB_PV_HEAD_WORDS, head, &exception);
  size_t tail_words = 0;
  ModbusSpan tail_span;
  if (result == FetchResult::Ok &&
      modbus_pv_tail_span(modbus_pv_string_count(head, MB_PV_HEAD_WORDS), &tail_span)) {
    result = read_span(unit, tail_span.start, tail_span.words, tail, &exception);
    tail_words = tail_span.words;
  }
  if (result == FetchResult::ProtocolError && exception != 0) {
    retry_at = millis() + STRINGS_RETRY_MS;
    if (retry_at == 0) {
      retry_at = 1;  // 0 means "not backing off"
    }
    modbus_strings_failed(values);
    return FetchResult::Ok;
  }
  if (result != FetchResult::Ok) {
    return result;
  }
  modbus_apply_strings(values, head, MB_PV_HEAD_WORDS, tail_words > 0 ? tail : nullptr,
                       tail_words);
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
    } else {
      // Grid frequency and voltage, from every inverter every cycle.
      result = read_scope(device.slave_id, ModbusScope::Inverter, ModbusCadence::Fast, &values,
                          detail);
      if (result == FetchResult::Ok && device.dc_charger) {
        // Only an inverter with a DC charger fitted has anything at 31502, and
        // only its output counts towards EV — the same detection summary.py's
        // _ev_power does from the configured devices.
        result = read_scope(device.slave_id, ModbusScope::Inverter, ModbusCadence::DcCharger,
                            &values, detail);
      }
      // PV strings, for the losses under PLANT: at least one more request per
      // inverter, so a second or more on every poll.
      if (result == FetchResult::Ok) {
        result = read_strings(i, device.slave_id, &values);
      }
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

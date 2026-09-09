#pragma once

#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"

enum class HaEntity : uint8_t {
  PvPower = 0,
  GridPower,
  BatteryPower,
  HomePower,
  EvPower,
  PlantPower,
  OffGrid,
  BatterySoc,
  BatterySoh,
  BatteryCapacity,
  BatteryTemperature,
  TodayPv,
  TodayLoad,
  TodayGridImport,
  TodayGridExport,
  TodayBatteryCharge,
  TodayBatteryDischarge,
  Count,
};

enum class HaValueKind : uint8_t { Power, Energy, Percent, Temperature, Boolean };

struct HaEntityDescriptor {
  const char* payload_key;
  const char* nvs_key;
  const char* form_name;
  const char* label;
  HaValueKind kind;
};

static constexpr size_t HA_ENTITY_COUNT = static_cast<size_t>(HaEntity::Count);
static constexpr size_t HA_ENTITY_ID_MAX = 96;
extern const HaEntityDescriptor HA_ENTITIES[HA_ENTITY_COUNT];

struct HaParseInfo {
  size_t configured = 0;
  size_t available = 0;
  size_t unavailable = 0;
  size_t invalid_number = 0;
  size_t unsupported_unit = 0;
};

bool ha_entity_id_valid(const char* entity_id);

static constexpr size_t HA_TEMPLATE_MAX = 6144;
// Builds the Jinja template posted to Home Assistant. Only configured entities
// are referenced, so HA does no all-state download and the response stays small.
bool ha_template_build(const char* const entity_ids[HA_ENTITY_COUNT], char* out,
                       size_t out_size);

// Parses the compact JSON rendered by the /api/template request. A malformed
// document returns false and leaves *out untouched. Individual bad states remain
// unknown and are counted in info, allowing a partial response to stay useful.
bool ha_payload_parse(const char* json, size_t length, Snapshot* out,
                      HaParseInfo* info = nullptr);

// Maps REST status codes without requiring a live Home Assistant in tests.
enum class HaHttpStatus : uint8_t { Ok, Unauthorised, HttpError };
HaHttpStatus ha_http_status(int status_code);

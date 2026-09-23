// The board profile: every pin and tunable for the target board.
//
// This file selects one hardware profile at compile time and includes it.
// Nothing else in the tree carries a hardware constant, and nothing else
// includes a board_*.h profile directly — they all include "board_config.h"
// (put on the include path by -I src). Adding a board is a new profile header
// plus a case here; no downstream include changes. See CLAUDE.md.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// --------------------------------------------------------------- firmware ---

// Shared across every board variant: one repo, one version. package_release.sh
// greps this file for this line, so keep it here in board_config.h.
#define PUCK_FW_VERSION "0.15.1"

// -------------------------------------------------------- profile selection ---

#if defined(PUCK_BOARD_2P41)
#  include "board_2p41.h"  // Waveshare ESP32-S3-Touch-AMOLED-2.41 (600x450)
#else
#  include "board_1p75.h"  // Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466) — default
#endif

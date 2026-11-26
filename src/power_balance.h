// House load and system losses from the plant's live power figures.
//
// Used by the two sources that have no server to ask — Modbus and Home
// Assistant — so the formulas live in one place and match SigenStorDisplay's
// 0.47.3 summary.py. The server source never calls this: it sends both figures.
//
// Why the AC side: `pv` (30035) and `batt` (30037) are DC-side figures and
// `grid` is AC, so the old `pv - batt + grid - ev` counted the system's own
// losses — conversion, ~75 W standby, battery heating — as house load. About
// 0.4 kW of phantom "house" on a grid-charging night. The house load measured
// at the AC terminals is instead:
//
//   home   = grid + plant - ev_ac
//   losses = pv - batt - plant - ev_dc
//
// `plant` (30031) is negative while the inverter takes AC in. An AC charger is
// on the house wiring, so it comes off home; a DC charger leaves the DC bus
// before the plant's AC terminals, so it is already outside `grid + plant` and
// belongs in the losses balance instead.
//
// Outside src/device/ so the simulator's selftest covers it.

#pragma once

#include "snapshot.h"

struct PowerBalance {
  MaybeFloat home;    // kW, floored at zero
  MaybeFloat losses;  // kW, not floored: a small negative is sensor noise, and a
                      // floor would hide a sign error rather than show it
};

// Every input in kW. `ev_ac` and `ev_dc` must be known (zero when no charger of
// that kind exists) for any figure that uses them to come out known.
//
// With `plant` unknown, home falls back to the old DC-side formula so a source
// that cannot read the plant's power still gets a house figure, and losses is
// unknown.
PowerBalance power_balance(const MaybeFloat& pv, const MaybeFloat& batt, const MaybeFloat& grid,
                           const MaybeFloat& plant, const MaybeFloat& ev_ac,
                           const MaybeFloat& ev_dc);

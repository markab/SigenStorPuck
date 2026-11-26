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
//   solar  = max(strings, pv)
//   home   = grid + plant - ev_ac
//   losses = solar - batt - plant - ev_dc
//
// `strings` is the power at the inverters' PV inputs, V x I summed over every
// string. The plant's own PV figure (`pv`, 30035) cannot stand in for it while
// the sun is up: it is what is left once the battery and the AC output are
// accounted for, already net of nearly every loss (standby, the battery's
// conversion, the solar conversion), so with it the losses read ~0 all day, and
// in weak sun it reads nothing at all while the strings deliver 0.1 kW.
//
// So Solar is what the panels delivered, the strings, as in SigenStorDisplay
// 0.47.7 — and the same figure goes into the losses, so the power screen adds
// up: Solar + battery out = plant + loss. The max is for sensor error in low
// light: never show less than Sigenergy's own figure. Without string data `pv`
// stands in for both.
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
  MaybeFloat solar;   // kW the panels delivered; what the Solar node shows
  MaybeFloat home;    // kW, floored at zero
  MaybeFloat losses;  // kW, the system's total; not floored: a small negative is
                      // sensor noise, and a floor would hide a sign error
};

// Every input in kW. `ev_ac` and `ev_dc` must be known (zero when no charger of
// that kind exists) for any figure that uses them to come out known.
//
// With `plant` unknown, home falls back to the old DC-side formula — on `pv`,
// the register, as the server's does — so a source that cannot read the plant's
// power still gets a house figure, and losses is unknown. With `strings` unknown
// — no string data, or a read that failed this cycle, never zero — solar is `pv`
// and losses misses most daytime loss.
PowerBalance power_balance(const MaybeFloat& pv, const MaybeFloat& batt, const MaybeFloat& grid,
                           const MaybeFloat& plant, const MaybeFloat& ev_ac,
                           const MaybeFloat& ev_dc, const MaybeFloat& strings = MaybeFloat{});

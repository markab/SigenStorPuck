#include "power_balance.h"

PowerBalance power_balance(const MaybeFloat& pv, const MaybeFloat& batt, const MaybeFloat& grid,
                           const MaybeFloat& plant, const MaybeFloat& ev_ac,
                           const MaybeFloat& ev_dc, const MaybeFloat& strings) {
  PowerBalance out;

  if (strings.known) {
    out.solar.known = true;
    out.solar.value = pv.known && pv.value > strings.value ? pv.value : strings.value;
  } else {
    out.solar = pv;
  }

  if (grid.known && plant.known && ev_ac.known) {
    const float home = grid.value + plant.value - ev_ac.value;
    out.home.known = true;
    out.home.value = home > 0.0f ? home : 0.0f;
  } else if (pv.known && batt.known && grid.known && ev_ac.known && ev_dc.known) {
    // The pre-0.47.3 formula: counts the losses as house load, but it is the
    // best available without the plant's own power.
    const float home = pv.value - batt.value + grid.value - ev_ac.value - ev_dc.value;
    out.home.known = true;
    out.home.value = home > 0.0f ? home : 0.0f;
  }

  if (out.solar.known && batt.known && plant.known && ev_dc.known) {
    out.losses.known = true;
    out.losses.value = out.solar.value - batt.value - plant.value - ev_dc.value;
  }

  return out;
}

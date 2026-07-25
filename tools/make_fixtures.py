"""Write the /api/summary fixtures in test/fixtures.

Run from anywhere:  python3 tools/make_fixtures.py

Kept as a generator rather than hand-typed JSON so the derived `home` figure in
each fixture is computed the same way the server computes it, instead of being a
plausible-looking number that silently contradicts the other legs.
"""
import datetime
import json
import pathlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "test" / "fixtures"
OUT.mkdir(parents=True, exist_ok=True)

TS = int(datetime.datetime(2026, 7, 25, 13, 0, 0, tzinfo=datetime.timezone.utc).timestamp())
HALF_HOUR = 1800


def home(pv, grid, batt, ev=0.0, dc_out=0.0):
    """The server's derivation, from overview.js:516 via PLAN.md Part A."""
    inv_out = pv - batt - dc_out
    return round(max(0.0, inv_out + grid - ev), 2)


def plant(pv, batt, dc_out=0.0):
    """Stand-in for the plant_active_power register.

    The real value comes off the inverter and is NOT this expression -- it will
    differ by conversion losses. Computed here only so the fixtures carry a
    plausible, self-consistent number until the server sends the register.
    """
    return round(pv - batt - dc_out, 2)


def summary(*, age=3, ok=True, pv=0.0, grid=0.0, batt=0.0, ev=0.0, off_grid=False,
            soc=50.0, soh=99.0, capacity=16.0, temp=24.5, eta=None,
            today=..., cost=..., alarms=0, home_override=...):
    doc = {
        "v": 1,
        "ts": TS,
        "ok": ok,
        "age": age,
        "power": {
            "pv": pv,
            "grid": grid,
            "batt": batt,
            "home": home(pv, grid, batt, ev) if home_override is ... else home_override,
            "ev": ev,
            "plant": plant(pv, batt) if home_override is ... else None,
            "off_grid": off_grid,
        },
        "battery": {
            "soc": soc,
            "soh": soh,
            "capacity_kwh": capacity,
            "temp_c": temp,
            "eta_min": eta,
        },
        "today": {
            "solar": 12.4, "import": 3.1, "export": 5.2,
            "load": 9.8, "charge": 6.0, "discharge": 4.2,
        } if today is ... else today,
        "cost": {
            "configured": True,
            "saving_gbp": 1.23,
            "rate_p": 7.5,
            "next": [
                {"from": TS + HALF_HOUR, "p": 23.1},
                {"from": TS + 2 * HALF_HOUR, "p": 7.5},
                {"from": TS + 3 * HALF_HOUR, "p": 7.5},
            ],
        } if cost is ... else cost,
        "alarms": alarms,
    }
    return doc


FIXTURES = {
    # Midday: solar covering the house, surplus split between battery and export.
    "01_normal.json": summary(pv=3.42, grid=-1.10, batt=1.20, soc=78.5, eta=96),

    # After dark: battery carrying the house, a little topped up from the grid.
    "02_evening_discharge.json": summary(pv=0.0, grid=0.35, batt=-2.10, soc=41.5, eta=118,
                                         temp=22.0),

    # Battery full, everything spare going out to the grid.
    "03_exporting.json": summary(pv=6.80, grid=-5.42, batt=0.0, soc=100.0, eta=None,
                                 temp=27.5),

    # EV charging — the leg that only appears when power.ev > 0.
    "04_ev_charging.json": summary(pv=4.10, grid=0.60, batt=-0.50, ev=3.20, soc=63.0,
                                   eta=74),

    # Islanded: the grid leg must be suppressed entirely, not drawn as zero.
    "05_off_grid.json": summary(pv=1.80, grid=0.0, batt=-1.10, off_grid=True, soc=55.0,
                                eta=92, alarms=1),

    # No tariff set on the server: cost block present but unconfigured.
    "06_cost_unconfigured.json": summary(pv=2.90, grid=-0.40, batt=0.80, soc=70.0, eta=110,
                                        cost={"configured": False}),

    # The documented gotcha: `today` is null, not an object.
    "07_today_null.json": summary(pv=3.10, grid=-0.90, batt=1.00, soc=66.0, eta=104,
                                  today=None),

    # Unread registers arrive as null. "Unknown" must not render as "0.0 kW".
    "08_registers_null.json": summary(ok=False, age=12, soc=None, soh=None, temp=None,
                                      eta=None, home_override=None,
                                      pv=None, grid=None, batt=None, ev=None),

    # Server alive but the plant reading is minutes old — drives the stale chip.
    "09_stale.json": summary(age=187, pv=2.20, grid=-0.30, batt=0.60, soc=72.0, eta=100),
}

for name, doc in FIXTURES.items():
    path = OUT / name
    path.write_text(json.dumps(doc, indent=2) + "\n")
    print(f"{name:28} {len(json.dumps(doc, separators=(',', ':'))):4} bytes minified")

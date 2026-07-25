# SigenStorPuck

A round touchscreen status display for a [SigenStor Display](https://github.com/markab/SigenStorDisplay)
solar and battery installation.

Runs on a **Waveshare ESP32-S3-Touch-AMOLED-1.75** — a 466×466 round AMOLED with
capacitive touch — and shows live power flow, battery state, today's energy totals
and tariff costs, swiping between screens.

## Status

Early. Board bring-up works — the display, LVGL and touch all run on real hardware —
but none of the four screens, the provisioning flow or the network client exist yet. The
design is in [docs/PLAN.md](docs/PLAN.md); the server half it depends on shipped in
SigenStor Display v0.13.0 as `GET /api/summary`.

## How it works

The Puck is a renderer, not a calculator. It polls one small endpoint on your
SigenStor Display server:

```
GET /api/summary
```

which returns ~350 bytes of already-derived numbers — house load, battery state,
today's totals, savings, upcoming unit rates. All the arithmetic stays on the
server, where it is tested; the firmware just draws.

It works against a Raspberry Pi on the LAN over plain HTTP, or a public VPS over
HTTPS, with the same build. Authentication uses the server's existing read-only
**kiosk token**, which is long-lived and revocable from the Admin page.

## Setup (planned)

1. Flash from the web installer in Chrome or Edge — no toolchain needed.
2. On first boot the Puck raises a WiFi access point; join it and pick your network.
3. Visit `http://sigenstorpuck.local/` and paste the kiosk enrolment URL from
   SigenStor Display's Admin → Kiosk devices.

After that it updates itself from GitHub Releases.

## Hardware

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.75 |
| Display | 466×466 round AMOLED, CO5300 over QSPI |
| Touch | CST9217, I2C |
| MCU | ESP32-S3R8, 8 MB PSRAM, 16 MB flash |

## Licence

MIT.

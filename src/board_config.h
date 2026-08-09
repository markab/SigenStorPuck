// Every pin and tunable for the Waveshare ESP32-S3-Touch-AMOLED-1.75.
//
// Nothing else in the tree carries a hardware constant — see CLAUDE.md.
//
// Pin values are Waveshare's own, from
//   waveshareteam/ESP32-S3-Touch-AMOLED-1.75
//   examples/arduino/libraries/Mylibrary/pin_config.h
// and match the cross-check recorded in docs/PLAN.md §B1. The panel window
// offsets come from that repo's examples/arduino/06_LVGL_Widgets sketch.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// --------------------------------------------------------------- firmware ---

#define PUCK_FW_VERSION "0.8.3"

// ---------------------------------------------------------------- display ---

// 466x466 round AMOLED driven by a CO5300 over QSPI.
static constexpr int16_t PUCK_LCD_WIDTH = 466;
static constexpr int16_t PUCK_LCD_HEIGHT = 466;

static constexpr int8_t PUCK_LCD_CS = 12;
static constexpr int8_t PUCK_LCD_SCLK = 38;
static constexpr int8_t PUCK_LCD_D0 = 4;
static constexpr int8_t PUCK_LCD_D1 = 5;
static constexpr int8_t PUCK_LCD_D2 = 6;
static constexpr int8_t PUCK_LCD_D3 = 7;
static constexpr int8_t PUCK_LCD_RST = 39;

// The CO5300's addressable window does not start at the origin: panel column 0
// is column 6 of controller RAM. Omit this and every frame is shifted sideways.
static constexpr uint8_t PUCK_LCD_COL_OFFSET = 6;
static constexpr uint8_t PUCK_LCD_ROW_OFFSET = 0;

static constexpr uint8_t PUCK_LCD_ROTATION = 0;

// Arduino_ESP32QSPI's own default, stated here so it is tunable in one place if
// QSPI turns out to be marginal.
static constexpr int32_t PUCK_LCD_QSPI_HZ = 40000000;

// There is no PWM backlight pin on this board. Brightness is a CO5300 command,
// 0..255, issued via Arduino_CO5300::setBrightness().
static constexpr uint8_t PUCK_LCD_BRIGHTNESS = 200;

// LVGL partial draw buffer, in whole display lines. 40 x 466 x 2 B = 37 KB,
// which fits in DMA-capable internal RAM — the QSPI transfer needs that.
static constexpr uint16_t PUCK_LVGL_BUFFER_LINES = 40;

// ---------------------------------------------------------------- buttons ---

// GPIO0, the standard ESP32-S3 strapping pin. Readable at runtime; still forces
// download mode if held while power is applied.
static constexpr int8_t PUCK_BUTTON_BOOT = 0;

// ------------------------------------------------------------------ touch ---

static constexpr int8_t PUCK_TOUCH_INT = 11;
static constexpr int8_t PUCK_TOUCH_RST = 40;

// Published sources disagree on the CST9217's I2C address, so the firmware
// probes the bus and uses whichever answers (touch.cpp). SensorLib and
// Waveshare's sketches both say 0x5A; 0x15 is the other value in circulation.
static constexpr uint8_t PUCK_TOUCH_ADDR_PRIMARY = 0x5A;
static constexpr uint8_t PUCK_TOUCH_ADDR_ALT = 0x15;

static constexpr uint8_t PUCK_TOUCH_MAX_POINTS = 2;

// The touch layer is mounted flipped relative to the panel's scan order.
static constexpr bool PUCK_TOUCH_MIRROR_X = true;
static constexpr bool PUCK_TOUCH_MIRROR_Y = true;

// -------------------------------------------------------------------- i2c ---

// One bus, shared by touch, the AXP2101 PMIC, the PCF85063 RTC, the QMI8658
// IMU and the ES8311 codec.
static constexpr int8_t PUCK_I2C_SDA = 15;
static constexpr int8_t PUCK_I2C_SCL = 14;
static constexpr uint32_t PUCK_I2C_HZ = 400000;

// Known occupants, so the boot-time bus scan can name what it finds. All of
// these were confirmed present by the scan on real hardware; 0x40 also answers
// and has not been identified — it is not in Waveshare's BSP or examples.
static constexpr uint8_t PUCK_I2C_ADDR_ES8311 = 0x18;
static constexpr uint8_t PUCK_I2C_ADDR_TCA9554 = 0x20;
static constexpr uint8_t PUCK_I2C_ADDR_AXP2101 = 0x34;
static constexpr uint8_t PUCK_I2C_ADDR_LC76G_GPS = 0x50;
static constexpr uint8_t PUCK_I2C_ADDR_PCF85063 = 0x51;
static constexpr uint8_t PUCK_I2C_ADDR_QMI8658_L = 0x6B;
static constexpr uint8_t PUCK_I2C_ADDR_QMI8658_H = 0x6A;

// ----------------------------------------------------------------- serial ---

static constexpr uint32_t PUCK_SERIAL_BAUD = 115200;

// --------------------------------------------------------------------- ui ---

// Text must stay inside the square inscribed in the round bezel, or the corners
// of a label fall off the glass.
static constexpr int16_t PUCK_SAFE_SQUARE = 410;

// How old a reading may get before the screen calls it stale. The server's poll
// interval defaults to 5 s, so this allows several missed cycles before
// complaining (PLAN.md §A4).
static constexpr uint32_t PUCK_STALE_AFTER_S = 30;

// Decimal places on live kW figures. The payload carries two.
static constexpr int PUCK_KW_DECIMALS = 2;

// How far back the PWR button may step through past days. The server keeps far
// more than this; the limit is what is useful to reach one press at a time.
static constexpr int PUCK_MAX_DAYS_BACK = 7;

// Below this the battery arc turns amber. An exception, not a gradient: the ring
// means "the battery" everywhere else, so it should only change colour when
// there is genuinely something to notice.
static constexpr float PUCK_SOC_LOW_PCT = 15.0f;

// ------------------------------------------------------- panel longevity ---
//
// This is a static image on an AMOLED running continuously, so wear is
// cumulative per pixel and concentrated wherever something is always lit — the
// state-of-charge ring, the leg labels, the page dots.

// A band sweeping across the panel, rather than flashing the whole screen white.
// Every pixel is still lit once per cycle, but a band lights a fraction of them
// at any instant, so the total added emission is far lower. Flashing all 217k
// pixels to even out wear on a handful risks costing more life than it saves.
static constexpr uint32_t PUCK_SWEEP_DURATION_MS = 2600;
static constexpr lv_coord_t PUCK_SWEEP_BAND_PX = 56;
static constexpr lv_opa_t PUCK_SWEEP_OPACITY = 150;

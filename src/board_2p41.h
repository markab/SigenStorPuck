// Every pin and tunable for the Waveshare ESP32-S3-Touch-AMOLED-2.41.
//
// One board profile — board_config.h selects it with -D PUCK_BOARD_2P41.
// It defines exactly the same symbol names as board_1p75.h, so every
// translation unit compiles unchanged; only the values differ.
//
// 600x450 landscape AMOLED, RM690B0 over QSPI, FT6336 capacitive touch,
// ESP32-S3R8 / 16 MB flash / 8 MB octal PSRAM (same silicon as the 1.75).
//
// !!! PLACEHOLDER PINS !!!  The display/touch pin numbers below are copied from
// the 1.75 so the puck241 env links during the Phase 1 refactor. Phase 2
// replaces them with the real values from
//   waveshareteam/ESP32-S3-Touch-AMOLED-2.41  (pin_config.h)
// before flashing real hardware. Anything marked PLACEHOLDER is not yet verified.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// --------------------------------------------------------- board identity ---

#define PUCK_BOARD_SLUG "puck-2.41"
#define PUCK_BOARD_NAME "Waveshare ESP32-S3-Touch-AMOLED-2.41"

// Landscape rectangle, not a round bezel.
static constexpr bool PUCK_DISPLAY_ROUND = false;

// ---------------------------------------------------------------- display ---

// 600x450 landscape AMOLED driven by an RM690B0 over QSPI.
static constexpr int16_t PUCK_LCD_WIDTH = 600;
static constexpr int16_t PUCK_LCD_HEIGHT = 450;

static constexpr int8_t PUCK_LCD_CS = 12;    // PLACEHOLDER — source from 2.41 BSP
static constexpr int8_t PUCK_LCD_SCLK = 38;  // PLACEHOLDER
static constexpr int8_t PUCK_LCD_D0 = 4;     // PLACEHOLDER
static constexpr int8_t PUCK_LCD_D1 = 5;     // PLACEHOLDER
static constexpr int8_t PUCK_LCD_D2 = 6;     // PLACEHOLDER
static constexpr int8_t PUCK_LCD_D3 = 7;     // PLACEHOLDER
static constexpr int8_t PUCK_LCD_RST = 39;   // PLACEHOLDER

// RM690B0 panel window offsets. PLACEHOLDER — verify against the 2.41 sketch;
// unlike the CO5300's column-6 quirk these are likely 0.
static constexpr uint8_t PUCK_LCD_COL_OFFSET = 0;
static constexpr uint8_t PUCK_LCD_ROW_OFFSET = 0;

static constexpr uint8_t PUCK_LCD_ROTATION = 0;

static constexpr int32_t PUCK_LCD_QSPI_HZ = 40000000;

// AMOLED brightness command, 0..255. On the RM690B0 this is a controller
// command (no PWM backlight pin), same shape as the CO5300 path.
static constexpr uint8_t PUCK_LCD_BRIGHTNESS = 200;

// LVGL partial draw buffer, in whole display lines. 40 x 600 x 2 B = 48 KB in
// DMA-capable internal RAM.
static constexpr uint16_t PUCK_LVGL_BUFFER_LINES = 40;

// ---------------------------------------------------------------- buttons ---

// GPIO0 strapping pin, same as the 1.75.
static constexpr int8_t PUCK_BUTTON_BOOT = 0;

// ------------------------------------------------------------------ touch ---

static constexpr int8_t PUCK_TOUCH_INT = 11;  // PLACEHOLDER
static constexpr int8_t PUCK_TOUCH_RST = 40;  // PLACEHOLDER

// FT6336 (FocalTech) answers at 0x38 and has no alternate address. The shared
// CST92xx probe/labeller still expects two DISTINCT candidates (it lists both
// as switch cases), so ALT is an unused sentinel (0x39, no device) until the
// Phase 2 FT6336 backend replaces touch.cpp for this board.
static constexpr uint8_t PUCK_TOUCH_ADDR_PRIMARY = 0x38;
static constexpr uint8_t PUCK_TOUCH_ADDR_ALT = 0x39;

static constexpr uint8_t PUCK_TOUCH_MAX_POINTS = 2;

// Mounting orientation relative to the panel scan order. PLACEHOLDER — confirm
// on hardware in Phase 2.
static constexpr bool PUCK_TOUCH_MIRROR_X = false;
static constexpr bool PUCK_TOUCH_MIRROR_Y = false;

// -------------------------------------------------------------------- i2c ---

// Shared bus. PLACEHOLDER pins — likely the same GPIO15/14 as the 1.75.
static constexpr int8_t PUCK_I2C_SDA = 15;
static constexpr int8_t PUCK_I2C_SCL = 14;
static constexpr uint32_t PUCK_I2C_HZ = 400000;

// Known/expected occupants, for the boot-time bus scan's labels. The 2.41
// carries the QMI8658 and PCF85063; the AXP2101 is unconfirmed (see power path,
// Phase 2). These addresses only feed the diagnostic labeller.
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

// No round bezel to inscribe; use the short (450 px) dimension as the safe
// bound so any shared round-UI overlay still fits during the Phase 1 refactor.
// The landscape screen-set (Phase 3) carries its own geometry.
static constexpr int16_t PUCK_SAFE_SQUARE = 450;

static constexpr uint32_t PUCK_STALE_AFTER_S = 30;
static constexpr int PUCK_KW_DECIMALS = 2;
static constexpr int PUCK_MAX_DAYS_BACK = 7;
static constexpr uint32_t PUCK_DAY_RETURN_S = 120;
static constexpr uint32_t PUCK_SCREEN_WAKE_S = 30;
static constexpr float PUCK_SOC_LOW_PCT = 15.0f;

// ------------------------------------------------------- panel longevity ---
//
// Also an AMOLED running a static image, so the same sweep-saver applies.
static constexpr uint32_t PUCK_SWEEP_DURATION_MS = 2600;
static constexpr lv_coord_t PUCK_SWEEP_BAND_PX = 56;
static constexpr lv_opa_t PUCK_SWEEP_OPACITY = 150;

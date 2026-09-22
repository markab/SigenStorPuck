// Every pin and tunable for the Waveshare ESP32-S3-Touch-AMOLED-2.41, V1 board.
//
// One board profile — board_config.h selects it with -D PUCK_BOARD_2P41.
// It defines exactly the same symbol names as board_1p75.h, so every
// translation unit compiles unchanged; only the values differ.
//
// 600x450 landscape AMOLED, RM690B0 over QSPI, FT6336 capacitive touch (FocalTech
// FT5x06 family), ESP32-S3R8 / 16 MB flash / 8 MB octal PSRAM.
//
// The 2.41 ships in two incompatible revisions. This profile targets the V1 (the
// revision *without* a "Rev2.0" silkscreen), which is what has been verified on
// hardware. Its pins come from Waveshare's V1 demo (the wiki download, not the
// -V2 GitHub repo). The V2 revision swaps the reset and tearing-effect lines and
// moves touch INT/RST, so it would need its own profile.
//
// Facts confirmed on real V1 hardware:
//   * OLED reset is GPIO21 and touch reset is GPIO3 — both real GPIOs, not the
//     TCA9554 expander (on V2 the resets are expander lines and GPIO21 is TE).
//     Without pulsing OLED reset the panel never leaves reset and stays dark.
//   * The RM690B0 is driven with the SH8601-compatible init the demo uses; the
//     panel is natively 450x600 portrait with a 16-pixel column offset in its
//     482-wide RAM, shown as 600x450 landscape via a software rotation in the
//     display backend (a hardware MADCTL swap transposes Arduino_GFX's row-major
//     pixel stream). PUCK_LCD_WIDTH/HEIGHT are the landscape (logical) size;
//     PUCK_LCD_NATIVE_* are the portrait size the panel is constructed at.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// --------------------------------------------------------- board identity ---

#define PUCK_BOARD_SLUG "puck-2.41"
#define PUCK_BOARD_NAME "Waveshare ESP32-S3-Touch-AMOLED-2.41"

// Landscape rectangle, not a round bezel.
static constexpr bool PUCK_DISPLAY_ROUND = false;

// ---------------------------------------------------------------- display ---

// 600x450 landscape (logical) — the coordinate space LVGL and every screen use.
static constexpr int16_t PUCK_LCD_WIDTH = 600;
static constexpr int16_t PUCK_LCD_HEIGHT = 450;

// Native panel size (450x600 portrait): what Arduino_SH8601 is constructed at,
// before the backend's 90-degree software rotation presents it as landscape.
static constexpr int16_t PUCK_LCD_NATIVE_WIDTH = 450;
static constexpr int16_t PUCK_LCD_NATIVE_HEIGHT = 600;

static constexpr int8_t PUCK_LCD_CS = 9;
static constexpr int8_t PUCK_LCD_SCLK = 10;
static constexpr int8_t PUCK_LCD_D0 = 11;
static constexpr int8_t PUCK_LCD_D1 = 12;
static constexpr int8_t PUCK_LCD_D2 = 13;
static constexpr int8_t PUCK_LCD_D3 = 14;
// OLED reset is GPIO21, a real GPIO. Confirmed on hardware: pulsing GPIO21 is
// what finally brought the panel out of reset (see the header note on V1 vs V2).
static constexpr int8_t PUCK_LCD_RST = 21;

// Native-portrait window offsets. The visible 450-wide band sits at column 16 in
// the RM690B0's 482-wide RAM (the demo's +16). Arduino_GFX adds col_offset1 to x
// in writeAddrWindow. Software rotation (below) does the portrait->landscape turn,
// because Arduino_GFX's row-major pixel stream transposes under a hardware
// MADCTL swap.
static constexpr uint8_t PUCK_LCD_COL_OFFSET = 16;
static constexpr uint8_t PUCK_LCD_ROW_OFFSET = 0;

// Base rotation the backend applies in flush_cb: native portrait to logical
// landscape. 3 = 270 degrees clockwise, which came out upright on this panel.
// Composed with the runtime orientation.
static constexpr uint8_t PUCK_LCD_ROTATION = 3;

static constexpr int32_t PUCK_LCD_QSPI_HZ = 40000000;

// AMOLED brightness command, 0..255 (RM690B0 controller command, no PWM pin).
static constexpr uint8_t PUCK_LCD_BRIGHTNESS = 200;

// LVGL partial draw buffer, in whole display lines. 40 x 600 x 2 B = 48 KB in
// DMA-capable internal RAM.
static constexpr uint16_t PUCK_LVGL_BUFFER_LINES = 40;

// ---------------------------------------------------------------- buttons ---

// GPIO0 strapping pin, same as the 1.75. (The 2.41's PWR button GPIO is not
// wired up here yet — the no-PMIC PWR-as-GPIO path is a follow-up.)
static constexpr int8_t PUCK_BUTTON_BOOT = 0;

// ------------------------------------------------------------------ touch ---

// Touch reset is GPIO3 on V1. The interrupt line is EXIO2 (on the expander) and
// is left unused — the poll in touch_ft6336.cpp reads over I2C every frame, so
// INT = -1. (On V2 these are swapped: INT on GPIO3, reset on the expander.)
static constexpr int8_t PUCK_TOUCH_INT = -1;
static constexpr int8_t PUCK_TOUCH_RST = 3;

// FT6336 answers at 0x38 and has no alternate address. The shared CST92xx
// probe/labeller wants two DISTINCT candidates, so ALT is an unused sentinel;
// the FT6336 backend (touch_ft6336.cpp) only uses PRIMARY.
static constexpr uint8_t PUCK_TOUCH_ADDR_PRIMARY = 0x38;
static constexpr uint8_t PUCK_TOUCH_ADDR_ALT = 0x39;

static constexpr uint8_t PUCK_TOUCH_MAX_POINTS = 2;

// Mount orientation relative to the panel scan order. Confirm on hardware.
static constexpr bool PUCK_TOUCH_MIRROR_X = false;
static constexpr bool PUCK_TOUCH_MIRROR_Y = false;

// -------------------------------------------------------------------- i2c ---

// The touch + expander bus (the demo uses GPIO47/48 at 300 kHz).
static constexpr int8_t PUCK_I2C_SDA = 47;
static constexpr int8_t PUCK_I2C_SCL = 48;
static constexpr uint32_t PUCK_I2C_HZ = 300000;

// Known/expected occupants for the boot-time bus scan's labels. The TCA9554
// (0x20) is present (it carries the touch INT and TE lines) but this profile
// drives resets over GPIO and does not use it. The 2.41 also carries the QMI8658
// and PCF85063. These feed the diagnostic labeller only.
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

// No round bezel to inscribe; the short (450 px) dimension is the safe bound for
// any shared round-UI overlay. The landscape screen-set carries its own geometry.
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

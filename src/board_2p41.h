// Every pin and tunable for the Waveshare ESP32-S3-Touch-AMOLED-2.41 (V2).
//
// One board profile — board_config.h selects it with -D PUCK_BOARD_2P41.
// It defines exactly the same symbol names as board_1p75.h, so every
// translation unit compiles unchanged; only the values differ.
//
// 600x450 landscape AMOLED, SH8601 over QSPI, FT6336 capacitive touch (FocalTech
// FT5x06 family), ESP32-S3R8 / 16 MB flash / 8 MB octal PSRAM.
//
// Pin values are the real V2 pins, taken from Waveshare's own example
//   waveshareteam/ESP32-S3-Touch-AMOLED-2.41-V2
//   02_Example/Arduino/09_LVGL_Test/09_LVGL_Test.ino
// The 2.41 has two incompatible board revisions; these are the V2 pins. V1
// differs (different touch INT/RST and expander mapping) and is not supported by
// this profile.
//
// Two hardware notes that still need confirming on real glass:
//   * The panel and touch RESET lines are NOT GPIOs — both are driven through
//     the TCA9554 I/O expander (0x20). PUCK_LCD_RST / PUCK_TOUCH_RST are -1, and
//     the backends must pulse the expander's reset lines before init (see
//     display_sh8601.cpp / touch_ft6336.cpp). The exact EXIO bit mapping is a
//     bring-up item.
//   * The SH8601 panel is natively 450x600 portrait; the 2.41 is shown as
//     600x450 landscape via a 90-degree rotation, which the display backend must
//     apply. PUCK_LCD_WIDTH/HEIGHT below are the landscape (logical) size.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// --------------------------------------------------------- board identity ---

#define PUCK_BOARD_SLUG "puck-2.41"
#define PUCK_BOARD_NAME "Waveshare ESP32-S3-Touch-AMOLED-2.41"

// Landscape rectangle, not a round bezel.
static constexpr bool PUCK_DISPLAY_ROUND = false;

// ---------------------------------------------------------------- display ---

// 600x450 landscape (logical). Native panel is 450x600 portrait; see the note
// at the top about the 90-degree rotation the backend applies.
static constexpr int16_t PUCK_LCD_WIDTH = 600;
static constexpr int16_t PUCK_LCD_HEIGHT = 450;

static constexpr int8_t PUCK_LCD_CS = 9;
static constexpr int8_t PUCK_LCD_SCLK = 10;
static constexpr int8_t PUCK_LCD_D0 = 11;
static constexpr int8_t PUCK_LCD_D1 = 12;
static constexpr int8_t PUCK_LCD_D2 = 13;
static constexpr int8_t PUCK_LCD_D3 = 14;
// -1: panel reset is on the TCA9554 expander, not a GPIO (see top note).
static constexpr int8_t PUCK_LCD_RST = -1;

// SH8601 panel window offsets. Verify against the example; likely 0.
static constexpr uint8_t PUCK_LCD_COL_OFFSET = 0;
static constexpr uint8_t PUCK_LCD_ROW_OFFSET = 0;

static constexpr uint8_t PUCK_LCD_ROTATION = 0;

static constexpr int32_t PUCK_LCD_QSPI_HZ = 40000000;

// AMOLED brightness command, 0..255 (SH8601 controller command, no PWM pin).
static constexpr uint8_t PUCK_LCD_BRIGHTNESS = 200;

// LVGL partial draw buffer, in whole display lines. 40 x 600 x 2 B = 48 KB in
// DMA-capable internal RAM.
static constexpr uint16_t PUCK_LVGL_BUFFER_LINES = 40;

// ---------------------------------------------------------------- buttons ---

// GPIO0 strapping pin, same as the 1.75. (The 2.41's PWR button GPIO is not
// wired up here yet — the no-PMIC PWR-as-GPIO path is a follow-up.)
static constexpr int8_t PUCK_BUTTON_BOOT = 0;

// ------------------------------------------------------------------ touch ---

static constexpr int8_t PUCK_TOUCH_INT = 3;
// -1: touch reset is on the TCA9554 expander (EXIO1 on V2), not a GPIO.
static constexpr int8_t PUCK_TOUCH_RST = -1;

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

// The touch + expander bus on the V2 (09_LVGL_Test uses GPIO47/48 at 300 kHz).
static constexpr int8_t PUCK_I2C_SDA = 47;
static constexpr int8_t PUCK_I2C_SCL = 48;
static constexpr uint32_t PUCK_I2C_HZ = 300000;

// Known/expected occupants for the boot-time bus scan's labels. The TCA9554
// (0x20) drives the LCD and touch reset lines. The 2.41 also carries the
// QMI8658 and PCF85063 (which may sit on a separate bus — verify). These feed
// the diagnostic labeller only.
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

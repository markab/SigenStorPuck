// The look of every screen, in one place.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// True black. On an AMOLED an unlit pixel draws no current and gives contrast a
// backlit panel cannot, so black is both the cheapest and the best-looking
// background here.
static constexpr uint32_t PUCK_COLOUR_BG = 0x000000;
static constexpr uint32_t PUCK_COLOUR_TEXT = 0xF2F2F7;
static constexpr uint32_t PUCK_COLOUR_MUTED = 0x8E8E93;
static constexpr uint32_t PUCK_COLOUR_TRACK = 0x2C2C2E;  // unlit arcs, hairlines

// One hue per source, never per direction.
//
// Colouring by direction (red for import, green for export) means the palette
// flips every time the battery swaps between charge and discharge, which is
// exactly when a glanceable display should stay stable. Direction is carried by
// the flow animation and a word instead — which also survives being read by
// someone who cannot separate red from green.
static constexpr uint32_t PUCK_COLOUR_SOLAR = 0xFFD60A;
static constexpr uint32_t PUCK_COLOUR_GRID = 0x0A84FF;
static constexpr uint32_t PUCK_COLOUR_BATTERY = 0x30D158;
static constexpr uint32_t PUCK_COLOUR_HOME = 0xF2F2F7;
static constexpr uint32_t PUCK_COLOUR_EV = 0xBF5AF2;

static constexpr uint32_t PUCK_COLOUR_WARN = 0xFF9F0A;
static constexpr uint32_t PUCK_COLOUR_ALARM = 0xFF453A;

// The state-of-charge ring at the bezel. Here rather than in either screen
// because screens 1 and 2 both draw it, and a ring that changes weight or radius
// as you swipe between them reads as two different rings rather than the same
// one seen twice.
static constexpr lv_coord_t PUCK_RING_DIAMETER = 456;
static constexpr lv_coord_t PUCK_RING_WIDTH = 6;

// Montserrat subsets from src/ui/fonts/, declared by LV_FONT_CUSTOM_DECLARE in
// include/lv_conf.h. Named by role so a size change is one edit here.
#define PUCK_FONT_SMALL (&puck_font_14)
#define PUCK_FONT_BODY (&puck_font_20)
#define PUCK_FONT_LARGE (&puck_font_28)
#define PUCK_FONT_HERO (&puck_font_48)

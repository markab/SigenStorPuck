// LVGL's allocator on the device: PSRAM, not internal RAM.
//
// The pool holds objects, styles and transient draw masks. None of it is touched
// by DMA — the display's draw buffers are allocated separately in display.cpp and
// must stay in DMA-capable internal RAM — so there is no reason for it to occupy
// the scarce 320 KB when the board has 8 MB of PSRAM doing nothing.
//
// This exists because raising LV_MEM_SIZE to fit a sixth screen took internal RAM
// from 41 % to 51 %, and the ~33 KB that bought cost the TLS handshake more than
// it cost LVGL: mbedTLS wants tens of kilobytes at once for its record buffers and
// certificate parsing, and when it cannot get them HTTPClient returns a negative
// code that sigen_api.cpp can only report as "TLS failed". A pool sized for the UI
// should not be able to break the network.
//
// Falling back to internal RAM rather than returning NULL: a board that came up
// without PSRAM should render a bit and then run out, not fail inside lv_init()
// with nothing on screen to say why.

#pragma once

#include <stddef.h>

#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void* puck_lv_malloc(size_t size) {
  void* block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return block != NULL ? block : heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline void* puck_lv_realloc(void* block, size_t size) {
  // realloc leaves the original block intact when it fails, so trying the second
  // heap after the first is safe rather than a leak.
  void* moved = heap_caps_realloc(block, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return moved != NULL ? moved
                       : heap_caps_realloc(block, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline void puck_lv_free(void* block) {
  heap_caps_free(block);
}

#ifdef __cplusplus
}
#endif

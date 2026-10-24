// Where large, CPU-only buffers go.
//
// Internal RAM is the scarce kind: it holds the display's DMA buffers, and a TLS
// handshake needs tens of kilobytes of it in one piece. On the 2.41, whose portrait
// panel is rotated to landscape through a second full-size buffer, the history
// rings and chart caches as static arrays left ~35 KB free — too little for the
// update check, and at times for a poll. Anything the CPU alone reads and writes
// belongs in PSRAM instead, where there are 8 MB.
//
// Falls back to internal RAM where there is no PSRAM, and is plain malloc in the
// simulator. Header-only and outside src/device/ so the sim compiles every caller.

#pragma once

#include <stddef.h>
#include <stdlib.h>

#if defined(ARDUINO)
#include "esp_heap_caps.h"
#endif

inline void* psram_alloc(size_t bytes) {
#if defined(ARDUINO)
  void* block = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return block != nullptr ? block : heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  return malloc(bytes);
#endif
}

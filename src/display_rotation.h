// Rotation of one RGB565 display-buffer block.
//
// Kept independent of the panel driver so every orientation can be verified by
// the host self-tests. `dst` must hold width * height pixels and must not alias
// `src`.

#pragma once

#include <stdint.h>

void display_rotate_rgb565(uint8_t rotation, const uint16_t* src, int32_t width,
                           int32_t height, uint16_t* dst);

#include "display_rotation.h"

void display_rotate_rgb565(uint8_t rotation, const uint16_t* src, int32_t width,
                           int32_t height, uint16_t* dst) {
  if (src == nullptr || dst == nullptr || width <= 0 || height <= 0) {
    return;
  }

  switch (rotation & 0x03) {
    case 1: {  // 90 clockwise: each source row becomes a bottom-to-top column.
      const uint16_t* source = src;
      for (int32_t y = 0; y < height; ++y) {
        uint16_t* target = dst + height - 1 - y;
        for (int32_t x = 1; x < width; ++x) {
          *target = *source++;
          target += height;
        }
        *target = *source++;
      }
      break;
    }
    case 2: {  // 180: row-major output is simply the reversed input block.
      const uint16_t* source = src;
      uint16_t* target = dst + width * height;
      while (target != dst) {
        *--target = *source++;
      }
      break;
    }
    case 3: {  // 270 clockwise: each source row becomes a top-to-bottom column.
      const uint16_t* source = src;
      for (int32_t y = 0; y < height; ++y) {
        uint16_t* target = dst + (width - 1) * height + y;
        for (int32_t x = 1; x < width; ++x) {
          *target = *source++;
          target -= height;
        }
        *target = *source++;
      }
      break;
    }
    default:
      // Rotation zero is transferred directly and never needs this workspace.
      break;
  }
}

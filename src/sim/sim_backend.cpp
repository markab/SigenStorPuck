#include "sim_backend.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "board_config.h"

namespace {

SDL_Window* s_window = nullptr;
SDL_Renderer* s_renderer = nullptr;
SDL_Texture* s_texture = nullptr;

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
lv_color_t* s_pixels = nullptr;

lv_indev_drv_t s_indev_drv;
lv_coord_t s_pointer_x = PUCK_LCD_WIDTH / 2;
lv_coord_t s_pointer_y = PUCK_LCD_HEIGHT / 2;
bool s_pointer_down = false;
bool s_quit = false;

// A tiny ring buffer is enough — keys are consumed once per frame.
constexpr size_t KEY_QUEUE_SIZE = 16;
int s_keys[KEY_QUEUE_SIZE];
size_t s_key_head = 0;
size_t s_key_tail = 0;

void push_key(int code) {
  const size_t next = (s_key_tail + 1) % KEY_QUEUE_SIZE;
  if (next == s_key_head) {
    return;  // full; dropping a keypress is harmless here
  }
  s_keys[s_key_tail] = code;
  s_key_tail = next;
}

// Replicated from display.cpp on purpose. The desktop does not need 2-pixel
// alignment, but matching it means the sim redraws exactly the areas the device
// does — so a bug that depends on redraw geometry shows up here too.
void rounder_cb(lv_disp_drv_t* /*drv*/, lv_area_t* area) {
  area->x1 &= ~1;
  area->y1 &= ~1;
  area->x2 |= 1;
  area->y2 |= 1;
}

void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
  const int width = area->x2 - area->x1 + 1;
  const int height = area->y2 - area->y1 + 1;
  const SDL_Rect rect = {area->x1, area->y1, width, height};

  SDL_UpdateTexture(s_texture, &rect, pixels, width * static_cast<int>(sizeof(lv_color_t)));

  // Present once per refresh rather than once per chunk, or partial rendering
  // tears on screen for no reason.
  if (lv_disp_flush_is_last(drv)) {
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);
    SDL_RenderPresent(s_renderer);
  }
  lv_disp_flush_ready(drv);
}

void indev_read_cb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
  data->point.x = s_pointer_x;
  data->point.y = s_pointer_y;
  data->state = s_pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

bool sim_backend_begin() {
  // Built with -D SDL_MAIN_HANDLED so SDL does not rewrite main() out from
  // under us; that makes announcing readiness our job.
  SDL_SetMainReady();

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[sim] SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }

  // Deliberately not SDL_WINDOW_ALLOW_HIGHDPI: one window pixel must be one
  // panel pixel, or the sim flatters a design that will not fit on the glass.
  s_window = SDL_CreateWindow("SigenStorPuck", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT, 0);
  if (s_window == nullptr) {
    fprintf(stderr, "[sim] SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }

  s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (s_renderer == nullptr) {
    fprintf(stderr, "[sim] SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return false;
  }

  // RGB565 to match LV_COLOR_DEPTH 16 with no byte swap, so the sim renders the
  // same colour values the panel receives rather than a truer-than-life 24-bit
  // version of them.
  s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  if (s_texture == nullptr) {
    fprintf(stderr, "[sim] SDL_CreateTexture failed: %s\n", SDL_GetError());
    return false;
  }

  // Same partial-buffer geometry as the device, so LVGL chunks its redraws
  // identically here.
  const size_t pixel_count = static_cast<size_t>(PUCK_LCD_WIDTH) * PUCK_LVGL_BUFFER_LINES;
  s_pixels = static_cast<lv_color_t*>(malloc(pixel_count * sizeof(lv_color_t)));
  if (s_pixels == nullptr) {
    fprintf(stderr, "[sim] could not allocate the draw buffer\n");
    return false;
  }

  lv_disp_draw_buf_init(&s_draw_buf, s_pixels, nullptr, pixel_count);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = PUCK_LCD_WIDTH;
  s_disp_drv.ver_res = PUCK_LCD_HEIGHT;
  s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.rounder_cb = rounder_cb;
  lv_disp_drv_register(&s_disp_drv);

  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = indev_read_cb;
  lv_indev_drv_register(&s_indev_drv);

  printf("[sim] %dx%d window up, %u-line draw buffer\n", PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT,
         static_cast<unsigned>(PUCK_LVGL_BUFFER_LINES));
  return true;
}

bool sim_backend_pump() {
  SDL_Event event;
  while (SDL_PollEvent(&event) != 0) {
    switch (event.type) {
      case SDL_QUIT:
        s_quit = true;
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (event.button.button == SDL_BUTTON_LEFT) {
          s_pointer_down = true;
          s_pointer_x = static_cast<lv_coord_t>(event.button.x);
          s_pointer_y = static_cast<lv_coord_t>(event.button.y);
        }
        break;
      case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT) {
          s_pointer_down = false;
        }
        break;
      case SDL_MOUSEMOTION:
        s_pointer_x = static_cast<lv_coord_t>(event.motion.x);
        s_pointer_y = static_cast<lv_coord_t>(event.motion.y);
        break;
      case SDL_KEYDOWN:
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          s_quit = true;
        } else {
          push_key(event.key.keysym.sym);
        }
        break;
      default:
        break;
    }
  }
  return !s_quit;
}

int sim_backend_take_key() {
  if (s_key_head == s_key_tail) {
    return 0;
  }
  const int code = s_keys[s_key_head];
  s_key_head = (s_key_head + 1) % KEY_QUEUE_SIZE;
  return code;
}

void sim_backend_set_title(const char* title) {
  if (s_window != nullptr && title != nullptr) {
    SDL_SetWindowTitle(s_window, title);
  }
}

void sim_backend_delay(uint32_t milliseconds) {
  SDL_Delay(milliseconds);
}

bool sim_backend_save_bmp(const char* path) {
  if (s_renderer == nullptr || s_texture == nullptr) {
    return false;
  }

  // Re-copy rather than reading back after a Present: the contents of a
  // presented backbuffer are undefined on some renderers.
  SDL_RenderClear(s_renderer);
  SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);

  int width = 0;
  int height = 0;
  SDL_GetRendererOutputSize(s_renderer, &width, &height);

  SDL_Surface* surface =
      SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
  if (surface == nullptr) {
    fprintf(stderr, "[sim] screenshot surface failed: %s\n", SDL_GetError());
    return false;
  }

  bool ok = SDL_RenderReadPixels(s_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels,
                                 surface->pitch) == 0;
  if (ok) {
    ok = SDL_SaveBMP(surface, path) == 0;
  }
  if (!ok) {
    fprintf(stderr, "[sim] screenshot failed: %s\n", SDL_GetError());
  }
  SDL_FreeSurface(surface);
  return ok;
}

void sim_backend_end() {
  if (s_texture != nullptr) {
    SDL_DestroyTexture(s_texture);
  }
  if (s_renderer != nullptr) {
    SDL_DestroyRenderer(s_renderer);
  }
  if (s_window != nullptr) {
    SDL_DestroyWindow(s_window);
  }
  free(s_pixels);
  SDL_Quit();
}

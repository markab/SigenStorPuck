// The last screen, landscape: how to reach this device's settings page. QR on
// the left, both addresses on the right — the same content as the round board,
// laid out wide.

#include "screen_settings.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "qr_block.h"
#include "theme.h"

namespace {

lv_obj_t* s_root = nullptr;
lv_obj_t* s_qr = nullptr;
lv_obj_t* s_host = nullptr;
lv_obj_t* s_ip = nullptr;
char s_url[80] = {};

constexpr lv_coord_t QR_PX = 150;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour,
                     lv_coord_t dx, lv_coord_t dy) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(label, LV_ALIGN_CENTER, dx, dy);
  return label;
}

}  // namespace

lv_obj_t* screen_settings_create(lv_obj_t* parent) {
  s_root = lv_obj_create(parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  s_qr = puck_qr_block_create(s_root, QR_PX);
  lv_obj_align(s_qr, LV_ALIGN_CENTER, -150, 0);

  lv_obj_t* caption = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_TEXT, 130, -60);
  lv_label_set_text(caption, "Scan to set up");
  lv_obj_t* orlabel = make_label(s_root, PUCK_FONT_SMALL, PUCK_COLOUR_MUTED, 130, -25);
  lv_label_set_text(orlabel, "or browse to");
  s_host = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_GRID, 130, 10);
  lv_label_set_text(s_host, "");
  s_ip = make_label(s_root, PUCK_FONT_BODY, PUCK_COLOUR_GRID, 130, 45);
  lv_label_set_text(s_ip, "");
  return s_root;
}

void screen_settings_set_address(const char* host, const char* ip) {
  if (s_root == nullptr) {
    return;
  }
  if (s_host != nullptr) {
    lv_label_set_text(s_host, (host != nullptr && host[0] != '\0') ? host : "");
  }
  if (s_ip != nullptr) {
    lv_label_set_text(s_ip, (ip != nullptr && ip[0] != '\0') ? ip : "");
  }

  char url[sizeof(s_url)];
  if (ip == nullptr || ip[0] == '\0') {
    url[0] = '\0';
  } else {
    snprintf(url, sizeof(url), "http://%s/", ip);
  }
  if (strncmp(s_url, url, sizeof(s_url)) == 0) {
    return;
  }
  snprintf(s_url, sizeof(s_url), "%s", url);
  if (s_qr != nullptr && s_url[0] != '\0') {
    puck_qr_block_set_url(s_qr, s_url);
  }
}

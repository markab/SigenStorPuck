#include "qr_block.h"

#include <string.h>

namespace {

// The margin the QR spec calls a quiet zone. lv_qrcode draws modules edge to
// edge and adds none, and without it a reader has nothing to lock the finder
// patterns against — on a black background that is the difference between a
// code that scans instantly and one that never does.
constexpr lv_coord_t QUIET_ZONE = 10;

}  // namespace

lv_obj_t* puck_qr_block_create(lv_obj_t* parent, lv_coord_t modules_px) {
  // White card, black modules — not the screen's own palette.
  //
  // Inverted codes (light on dark) are optional in the spec and plenty of phone
  // cameras will not read them. This is the one place on the device where
  // matching the theme would cost the feature its whole purpose.
  lv_obj_t* card = lv_obj_create(parent);
  lv_obj_remove_style_all(card);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(card, modules_px + 2 * QUIET_ZONE, modules_px + 2 * QUIET_ZONE);
  lv_obj_set_style_bg_color(card, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 6, LV_PART_MAIN);

  lv_obj_t* code = lv_qrcode_create(card, modules_px, lv_color_black(), lv_color_white());
  lv_obj_center(code);

  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
  return card;
}

void puck_qr_block_set_url(lv_obj_t* card, const char* url) {
  if (card == nullptr) {
    return;
  }
  lv_obj_t* code = lv_obj_get_child(card, 0);
  if (code == nullptr || url == nullptr || url[0] == '\0') {
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  if (lv_qrcode_update(code, url, strlen(url)) != LV_RES_OK) {
    LV_LOG_WARN("QR: '%s' does not fit", url);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
}

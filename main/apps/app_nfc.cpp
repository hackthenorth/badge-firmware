#include "app.h"
#include "app_registry.h"
#include "hal_nfc.h"

#include <cstdio>
#include <cstring>

extern "C" const lv_image_dsc_t icon_nfc;

class NfcApp : public App {
public:
    const char *name() const override { return "NFC"; }
    const char *icon() const override { return "NFC"; }
    const char *slug() const override { return "nfc"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_nfc; }

    void on_enter(lv_obj_t *root) override
    {
        hal_nfc_enable();
        hal_nfc_clear_last_card();

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "NFC");
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        status_ = lv_label_create(root);
        lv_obj_set_style_text_color(status_, lv_color_white(), 0);
        lv_label_set_text(status_, "Tap a card");
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, -10);

        detail_ = lv_label_create(root);
        lv_obj_set_style_text_color(detail_, lv_color_hex(0x808080), 0);
        lv_label_set_text(detail_, "");
        lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 20);

        memset(&last_shown_, 0, sizeof(last_shown_));
        have_shown_ = false;
    }

    void on_exit() override
    {
        status_ = detail_ = nullptr;
    }

    void on_tick() override
    {
        NfcCard card;
        if (!hal_nfc_last_card(&card)) return;

        if (have_shown_
            && card.uid_len == last_shown_.uid_len
            && memcmp(card.uid, last_shown_.uid, card.uid_len) == 0) {
            return;
        }

        char uid_buf[64];
        int n = 0;
        for (size_t i = 0; i < card.uid_len && n < (int)sizeof(uid_buf) - 3; i++) {
            n += snprintf(uid_buf + n, sizeof(uid_buf) - n, "%02X ", card.uid[i]);
        }
        if (status_) lv_label_set_text_fmt(status_, "UID: %s", uid_buf);

        if (detail_) lv_label_set_text_fmt(detail_, "SAK=0x%02X ATQA=0x%04X",
            card.sak, card.atqa);

        last_shown_ = card;
        have_shown_ = true;
    }

private:
    lv_obj_t *status_ = nullptr;
    lv_obj_t *detail_ = nullptr;
    NfcCard last_shown_ = {};
    bool have_shown_ = false;
};

static NfcApp s_instance;
App *app_nfc_instance() { return &s_instance; }

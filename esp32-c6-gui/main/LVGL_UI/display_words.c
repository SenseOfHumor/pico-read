#include "display_words.h"
#include "display.h"
#include "sdkconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_LV_FONT_MONTSERRAT_24
LV_FONT_DECLARE(lv_font_montserrat_24);
#endif

#if CONFIG_LV_FONT_MONTSERRAT_28
LV_FONT_DECLARE(lv_font_montserrat_28);
#endif

#if CONFIG_LV_FONT_MONTSERRAT_32
LV_FONT_DECLARE(lv_font_montserrat_32);
#endif

#if CONFIG_LV_FONT_MONTSERRAT_40
LV_FONT_DECLARE(lv_font_montserrat_40);
#endif

#define WPM 500
#define MS_PER_WORD (60000 / WPM)
#define GUIDE_COLOR 0x909090
#define WORD_COLOR 0xD8D8D8
#define ANCHOR_GLOW_COLOR 0xC76868
#define ANCHOR_COLOR 0xD88484
#define SPEED_COLOR 0x707070
#define GUIDE_MARGIN_X 6
#define GUIDE_TOP_Y 25
#define GUIDE_BOTTOM_Y -25
#define GUIDE_THICKNESS 4
#define GUIDE_TICK_LEN 20
#define WORD_Y_OFFSET -4
#define WORD_SIDE_MARGIN 12

/* Adjust this manually to tune the reader text size. Supported values: 16, 24, 28, 32, 40 */
static uint16_t word_font_size = 32;
/* Adjust this manually to add a pause after punctuation. Units: milliseconds */
static uint16_t punctuation_pause_ms = 500;
/* Adjust this manually to tune word opacity. Range: 0..100 percent */
static uint8_t word_opacity_percent = 100;
/* Adjust this manually to tune guideline opacity. Range: 0..100 percent */
static uint8_t guide_opacity_percent = 50;

static lv_obj_t *left_label;
static lv_obj_t *left_bold_label;
static lv_obj_t *anchor_label;
static lv_obj_t *anchor_bold_label;
static lv_obj_t *right_label;
static lv_obj_t *right_bold_label;
static lv_obj_t *speed_label;

static char token_buf[DISPLAY_TOKEN_MAX_LEN];
static size_t max_word_len;
static char *word_buf;
static char *left_buf;
static char *right_buf;
static char anchor_buf[2];
static lv_coord_t word_max_width;

static lv_opa_t opacity_percent_to_lv(uint8_t percent)
{
    if (percent >= 100) {
        return LV_OPA_COVER;
    }
    return (lv_opa_t)((percent * LV_OPA_COVER) / 100);
}

static uint32_t get_word_delay_ms(const char *word)
{
    size_t len = strlen(word);
    if (len == 0) {
        return MS_PER_WORD;
    }

    for (size_t i = len; i > 0; i--) {
        char ch = word[i - 1];
        if (ch == '.' || ch == ',' || ch == ';' || ch == ':' || ch == '!' || ch == '?') {
            return MS_PER_WORD + punctuation_pause_ms;
        }
        if (isalnum((unsigned char)ch)) {
            break;
        }
    }

    return MS_PER_WORD;
}

static const lv_font_t *get_word_font(void)
{
    switch (word_font_size) {
    case 16:
        return &lv_font_montserrat_16;
    case 24:
        return &lv_font_montserrat_24;
    case 28:
        return &lv_font_montserrat_28;
    case 32:
        return &lv_font_montserrat_32;
    case 40:
        return &lv_font_montserrat_40;
    default:
        return &lv_font_montserrat_40;
    }
}

static void sync_word_positions(void)
{
    lv_obj_align(anchor_label, LV_ALIGN_CENTER, 0, WORD_Y_OFFSET);
    lv_obj_align(anchor_bold_label, LV_ALIGN_CENTER, 1, WORD_Y_OFFSET);

    lv_obj_align_to(left_label, anchor_label, LV_ALIGN_OUT_LEFT_MID, 0, 0);
    lv_obj_align_to(left_bold_label, anchor_bold_label, LV_ALIGN_OUT_LEFT_MID, 0, 0);

    lv_obj_align_to(right_label, anchor_label, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_align_to(right_bold_label, anchor_bold_label, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
}

static size_t find_chunk_len(const char *word, size_t len, const lv_font_t *font)
{
    lv_coord_t width = 0;
    size_t last_delim_split = 0;

    for (size_t i = 0; i < len; i++) {
        uint32_t letter = (unsigned char)word[i];
        uint32_t next = (i + 1 < len) ? (unsigned char)word[i + 1] : 0;

        width += lv_font_get_glyph_width(font, letter, next);
        if (i + 1 < len) {
            width += 1;
        }

        if (word[i] == '/' || word[i] == '-' || word[i] == '_') {
            last_delim_split = i + 1;
        }

        if (width > word_max_width) {
            if (last_delim_split > 0) {
                return last_delim_split;
            }
            return i > 0 ? i : 1;
        }
    }

    return len;
}

static bool next_chunk(char **chunk_start, size_t *chunk_len)
{
    const lv_font_t *word_font = get_word_font();
    static size_t pending_token_len;
    static size_t pending_token_offset;

    if (pending_token_offset < pending_token_len) {
        *chunk_start = token_buf + pending_token_offset;
        *chunk_len = find_chunk_len(*chunk_start, pending_token_len - pending_token_offset, word_font);
        pending_token_offset += *chunk_len;

        if (pending_token_offset >= pending_token_len) {
            pending_token_len = 0;
            pending_token_offset = 0;
        }

        return true;
    }

    if (!display_next_token(token_buf, sizeof(token_buf))) {
        return false;
    }

    pending_token_len = strlen(token_buf);
    pending_token_offset = 0;
    return next_chunk(chunk_start, chunk_len);
}

static void build_word_list(void)
{
    max_word_len = DISPLAY_TOKEN_MAX_LEN - 1;
    display_reset();

    word_buf = (char *)malloc(max_word_len + 1);
    left_buf = (char *)malloc(max_word_len + 1);
    right_buf = (char *)malloc(max_word_len + 1);
    if (!word_buf || !left_buf || !right_buf) {
        return;
    }

}

static void format_word(const char *word)
{
    size_t len = strlen(word);
    if (len == 0 || !left_buf || !right_buf) {
        return;
    }

    size_t anchor_idx = (len - 1) / 2;

    memcpy(left_buf, word, anchor_idx);
    left_buf[anchor_idx] = '\0';

    strcpy(right_buf, word + anchor_idx + 1);

    anchor_buf[0] = word[anchor_idx];
    anchor_buf[1] = '\0';

    lv_label_set_text(left_bold_label, left_buf);
    lv_label_set_text(left_label, left_buf);

    lv_label_set_text(anchor_bold_label, anchor_buf);
    lv_label_set_text(anchor_label, anchor_buf);

    lv_label_set_text(right_bold_label, right_buf);
    lv_label_set_text(right_label, right_buf);

    sync_word_positions();
}

static void next_word_cb(lv_timer_t *timer)
{
    char *chunk_start;
    size_t chunk_len;

    if (!word_buf || !left_buf || !right_buf) {
        return;
    }
    if (!next_chunk(&chunk_start, &chunk_len)) {
        return;
    }

    memcpy(word_buf, chunk_start, chunk_len);
    word_buf[chunk_len] = '\0';

    format_word(word_buf);
    if (timer) {
        lv_timer_set_period(timer, get_word_delay_ms(word_buf));
    }
}

static void style_word_label(lv_obj_t *label, const lv_font_t *font, lv_color_t color)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, opacity_percent_to_lv(word_opacity_percent), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
}

static lv_obj_t *create_guide(lv_obj_t *parent, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *guide = lv_obj_create(parent);
    lv_obj_remove_style_all(guide);
    lv_obj_set_size(guide, width, height);
    lv_obj_set_style_bg_color(guide, lv_color_hex(GUIDE_COLOR), 0);
    lv_obj_set_style_bg_opa(guide, opacity_percent_to_lv(guide_opacity_percent), 0);
    return guide;
}

void display_words_start(void)
{
    const lv_font_t *word_font = get_word_font();
    lv_obj_t *screen = lv_scr_act();
    lv_coord_t screen_w = lv_obj_get_width(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    word_max_width = screen_w - (WORD_SIDE_MARGIN * 2);

    build_word_list();

    lv_obj_t *top_guide = create_guide(screen, screen_w - (GUIDE_MARGIN_X * 2), GUIDE_THICKNESS);
    lv_obj_align(top_guide, LV_ALIGN_TOP_MID, 0, GUIDE_TOP_Y);

    lv_obj_t *bottom_guide = create_guide(screen, screen_w - (GUIDE_MARGIN_X * 2), GUIDE_THICKNESS);
    lv_obj_align(bottom_guide, LV_ALIGN_BOTTOM_MID, 0, GUIDE_BOTTOM_Y);

    lv_obj_t *top_tick = create_guide(screen, GUIDE_THICKNESS, GUIDE_TICK_LEN);
    lv_obj_align_to(top_tick, top_guide, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    lv_obj_t *bottom_tick = create_guide(screen, GUIDE_THICKNESS, GUIDE_TICK_LEN);
    lv_obj_align_to(bottom_tick, bottom_guide, LV_ALIGN_OUT_TOP_MID, 0, 0);

    speed_label = lv_label_create(screen);
    lv_label_set_text_fmt(speed_label, "%d WPM", WPM);
    lv_obj_set_style_text_color(speed_label, lv_color_hex(SPEED_COLOR), 0);
    lv_obj_set_style_text_font(speed_label, &lv_font_montserrat_12, 0);
    lv_obj_align(speed_label, LV_ALIGN_BOTTOM_RIGHT, -14, -4);

    left_bold_label = lv_label_create(screen);
    style_word_label(left_bold_label, word_font, lv_color_hex(WORD_COLOR));

    left_label = lv_label_create(screen);
    style_word_label(left_label, word_font, lv_color_hex(WORD_COLOR));

    anchor_bold_label = lv_label_create(screen);
    style_word_label(anchor_bold_label, word_font, lv_color_hex(ANCHOR_GLOW_COLOR));

    anchor_label = lv_label_create(screen);
    style_word_label(anchor_label, word_font, lv_color_hex(ANCHOR_COLOR));

    right_bold_label = lv_label_create(screen);
    style_word_label(right_bold_label, word_font, lv_color_hex(WORD_COLOR));

    right_label = lv_label_create(screen);
    style_word_label(right_label, word_font, lv_color_hex(WORD_COLOR));

    next_word_cb(NULL);
    lv_timer_create(next_word_cb, MS_PER_WORD, NULL);
}

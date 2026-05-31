#include "display_words.h"
#include "display.h"
#include "RGB.h"
#include "ST7789.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_sleep.h"

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

#define DEFAULT_WPM 400
#define WPM_STEP 50
#define WPM_MIN 50
#define WPM_MAX 1000

#define BOOT_BUTTON_GPIO GPIO_NUM_9
#define BOOT_BUTTON_ACTIVE_LEVEL 0
#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 1000
#define BUTTON_DOUBLE_CLICK_MS 300

#define BG_COLOR 0x000000
#define GUIDE_COLOR 0x909090
#define WORD_COLOR 0xD8D8D8
#define ANCHOR_GLOW_COLOR 0xC76868
#define ANCHOR_COLOR 0xD88484
#define SPEED_COLOR 0x707070
#define MENU_TEXT_COLOR 0xB8B8B8
#define MENU_HIGHLIGHT_COLOR 0xD88484
#define MENU_SUBTEXT_COLOR 0x707070
#define MENU_LIST_X 18
#define ROOT_LIST_Y 14
#define SUBMENU_TITLE_Y 12
#define SUBMENU_LIST_Y 48
#define MENU_LINE_STEP 28

#define GUIDE_MARGIN_X 6
#define GUIDE_TOP_Y 25
#define GUIDE_BOTTOM_Y -25
#define GUIDE_THICKNESS 4
#define GUIDE_TICK_LEN 20
#define WORD_Y_OFFSET -4
#define WORD_SIDE_MARGIN 12

/* Adjust this manually to tune the reader text size. Supported values: 16, 24, 28, 32, 40 */
static uint16_t word_font_size = 32;
/* Adjust this manually to tune word opacity. Range: 0..100 percent */
static uint8_t word_opacity_percent = 100;
/* Adjust this manually to tune guideline opacity. Range: 0..100 percent */
static uint8_t guide_opacity_percent = 50;
/* Adjust this manually to set the default reading speed. Units: words per minute */
static uint16_t reading_speed_wpm = DEFAULT_WPM;
/* Adjust this manually to auto-sleep the device after inactivity. Units: milliseconds */
static uint32_t inactivity_sleep_ms = 60000;
/* Delay tuning. Multipliers are stored as tenths to keep them easy to tweak in C. */
static uint8_t sentence_end_multiplier_x10 = 50;   /* . ! ? => 2.5x */
static uint8_t major_pause_multiplier_x10 = 50;    /* ; :   => 2.0x */
static uint8_t minor_pause_multiplier_x10 = 25;    /* ,     => 1.5x */
static uint8_t paragraph_break_multiplier_x10 = 40;/* break => 4.0x */
static uint8_t long_word_multiplier_x10 = 40;      /* 9+ chars => 1.2x */
static uint8_t long_word_min_chars = 10;
static uint16_t sentence_blank_delay_ms = 80;

typedef enum {
    UI_SCREEN_SPLASH = 0,
    UI_SCREEN_MENU,
    UI_SCREEN_BOOKS,
    UI_SCREEN_BOOK_ACTION,
    UI_SCREEN_SPEED,
    UI_SCREEN_BOOKMARKS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_BRIGHTNESS,
    UI_SCREEN_LED,
    UI_SCREEN_JUMP_MENU,
    UI_SCREEN_JUMP_CUSTOM,
    UI_SCREEN_READER,
} ui_screen_t;

static const char *menu_items[] = {
    "BOOKS",
    "SET READING SPEED",
    "BOOKMARKS",
    "STORAGE",
    "SCREEN BRIGHTNESS",
    "LED LIGHT",
    "SHUT DOWN",
};

static const char *book_action_items[] = {
    "CONTINUE",
    "START OVER",
    "JUMP",
};

static const char *jump_items[] = {
    "10%",
    "25%",
    "50%",
    "75%",
    "90%",
    "CUSTOM",
};

static const char *brightness_items[] = {
    "5%",
    "10%",
    "25%",
    "50%",
    "75%",
    "100%",
};

static const char *led_items[] = {
    "OFF",
    "BLUE",
    "GREEN",
    "AMBER",
    "WHITE",
    "RED",
    "CYAN",
    "MAGENTA",
    "YELLOW",
    "PURPLE",
    "MINT",
    "ORANGE",
    "PINK",
    "LIME",
    "SKY",
};

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_color_t;

static const led_color_t led_colors[] = {
    { 0, 0, 0 },      /* OFF */
    { 0, 0, 64 },     /* BLUE */
    { 0, 64, 0 },     /* GREEN */
    { 64, 48, 0 },    /* AMBER */
    { 48, 48, 48 },   /* WHITE */
    { 64, 0, 0 },     /* RED */
    { 0, 48, 48 },    /* CYAN */
    { 48, 0, 48 },    /* MAGENTA */
    { 48, 48, 0 },    /* YELLOW */
    { 32, 0, 64 },    /* PURPLE */
    { 0, 64, 32 },    /* MINT */
    { 64, 24, 0 },    /* ORANGE */
    { 64, 16, 32 },   /* PINK */
    { 32, 64, 0 },    /* LIME */
    { 0, 32, 64 },    /* SKY */
};

static ui_screen_t current_screen = UI_SCREEN_SPLASH;
static ui_screen_t screen_stack[8];
static uint8_t screen_stack_len;
static uint8_t root_menu_index;
static uint8_t book_menu_index;
static uint8_t bookmark_menu_index;
static uint8_t book_action_index;
static uint8_t jump_menu_index;
static uint8_t jump_digit_tens;
static uint8_t jump_digit_ones;
static uint8_t jump_digit_stage;
static uint8_t brightness_menu_index;
static uint8_t led_menu_index;

static lv_obj_t *left_label;
static lv_obj_t *left_bold_label;
static lv_obj_t *anchor_label;
static lv_obj_t *anchor_bold_label;
static lv_obj_t *right_label;
static lv_obj_t *right_bold_label;
static lv_obj_t *speed_label;
static lv_obj_t *toast_label;

static lv_timer_t *reader_timer;
static lv_timer_t *button_timer;
static lv_timer_t *toast_timer;
static lv_timer_t *splash_timer;
static bool reader_paused = true;

static char token_buf[DISPLAY_TOKEN_MAX_LEN];
static size_t max_word_len;
static char *word_buf;
static char *left_buf;
static char *right_buf;
static char anchor_buf[2];
static lv_coord_t word_max_width;
static size_t pending_token_len;
static size_t pending_token_offset;
static size_t current_token_visible_len;
static uint32_t pending_blank_delay_ms;

static bool button_raw_pressed;
static bool button_stable_pressed;
static bool button_long_fired;
static uint8_t pending_clicks;
static uint32_t button_change_tick;
static uint32_t button_press_tick;
static uint32_t last_click_tick;
static uint32_t last_activity_tick;

static lv_opa_t opacity_percent_to_lv(uint8_t percent)
{
    if (percent >= 100) {
        return LV_OPA_COVER;
    }
    return (lv_opa_t)((percent * LV_OPA_COVER) / 100);
}

static uint32_t get_base_word_delay_ms(void)
{
    uint16_t wpm = reading_speed_wpm ? reading_speed_wpm : DEFAULT_WPM;
    return 60000U / wpm;
}

static size_t get_visible_word_len(const char *word)
{
    size_t visible_len = 0;

    if (!word) {
        return 0;
    }

    for (size_t i = 0; word[i] != '\0'; i++) {
        if (isalnum((unsigned char)word[i])) {
            visible_len++;
        }
    }

    return visible_len;
}

static bool is_paragraph_break_token(const char *word)
{
    return word && word[0] == '\x1E' && word[1] == '\0';
}

static uint32_t apply_multiplier_x10(uint32_t base_delay, uint8_t multiplier_x10)
{
    return (base_delay * multiplier_x10) / 10U;
}

static uint32_t get_word_delay_ms(const char *word, uint32_t *blank_delay_ms)
{
    size_t len = strlen(word);
    uint32_t base_delay = get_base_word_delay_ms();
    uint32_t delay = base_delay;
    size_t visible_len = current_token_visible_len ? current_token_visible_len : get_visible_word_len(word);

    if (blank_delay_ms) {
        *blank_delay_ms = 0;
    }
    if (len == 0) {
        return base_delay;
    }

    if (is_paragraph_break_token(word)) {
        return apply_multiplier_x10(base_delay, paragraph_break_multiplier_x10);
    }

    for (size_t i = len; i > 0; i--) {
        char ch = word[i - 1];
        if (ch == '.' || ch == '!' || ch == '?') {
            delay = apply_multiplier_x10(base_delay, sentence_end_multiplier_x10);
            if (blank_delay_ms) {
                *blank_delay_ms = sentence_blank_delay_ms;
            }
            break;
        }
        if (ch == ';' || ch == ':') {
            delay = apply_multiplier_x10(base_delay, major_pause_multiplier_x10);
            break;
        }
        if (ch == ',') {
            delay = apply_multiplier_x10(base_delay, minor_pause_multiplier_x10);
            break;
        }
        if (isalnum((unsigned char)ch)) {
            break;
        }
    }

    if (visible_len >= long_word_min_chars) {
        uint32_t long_word_delay = apply_multiplier_x10(base_delay, long_word_multiplier_x10);
        if (delay < long_word_delay) {
            delay = long_word_delay;
        }
    }

    return delay;
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
        return &lv_font_montserrat_32;
    }
}

static const lv_font_t *get_menu_font(void)
{
#if CONFIG_LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#else
    return &lv_font_montserrat_16;
#endif
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
    current_token_visible_len = get_visible_word_len(token_buf);
    return next_chunk(chunk_start, chunk_len);
}

static void prepare_reader_buffers(void)
{
    pending_token_len = 0;
    pending_token_offset = 0;
    current_token_visible_len = 0;
    pending_blank_delay_ms = 0;

    if (word_buf && left_buf && right_buf) {
        display_reset();
        return;
    }

    max_word_len = DISPLAY_TOKEN_MAX_LEN - 1;
    word_buf = malloc(max_word_len + 1);
    left_buf = malloc(max_word_len + 1);
    right_buf = malloc(max_word_len + 1);
    display_reset();
}

static void push_screen(ui_screen_t next_screen)
{
    if (screen_stack_len < (sizeof(screen_stack) / sizeof(screen_stack[0]))) {
        screen_stack[screen_stack_len++] = current_screen;
    }
    current_screen = next_screen;
}

static void pop_screen(void)
{
    if (screen_stack_len == 0) {
        current_screen = UI_SCREEN_MENU;
        return;
    }

    current_screen = screen_stack[--screen_stack_len];
}

static void format_word(const char *word)
{
    size_t len = strlen(word);
    size_t anchor_idx;

    if (len == 0 || !left_buf || !right_buf) {
        return;
    }

    anchor_idx = (len - 1) / 2;

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

static void clear_word_display(void)
{
    if (!left_label || !left_bold_label || !anchor_label || !anchor_bold_label || !right_label || !right_bold_label) {
        return;
    }

    lv_label_set_text(left_bold_label, "");
    lv_label_set_text(left_label, "");
    lv_label_set_text(anchor_bold_label, "");
    lv_label_set_text(anchor_label, "");
    lv_label_set_text(right_bold_label, "");
    lv_label_set_text(right_label, "");
}

static void next_word_cb(lv_timer_t *timer)
{
    char *chunk_start;
    size_t chunk_len;
    uint32_t delay_ms;

    if (!word_buf || !left_buf || !right_buf) {
        return;
    }

    if (pending_blank_delay_ms > 0) {
        clear_word_display();
        if (timer) {
            lv_timer_set_period(timer, pending_blank_delay_ms);
        }
        pending_blank_delay_ms = 0;
        return;
    }

    for (;;) {
        if (!next_chunk(&chunk_start, &chunk_len)) {
            if (timer) {
                lv_timer_pause(timer);
            }
            reader_paused = true;
            if (speed_label) {
                lv_label_set_text_fmt(speed_label, "%u WPM  END", reading_speed_wpm);
            }
            return;
        }

        memcpy(word_buf, chunk_start, chunk_len);
        word_buf[chunk_len] = '\0';
        delay_ms = get_word_delay_ms(word_buf, &pending_blank_delay_ms);

        if (is_paragraph_break_token(word_buf)) {
            clear_word_display();
            if (timer) {
                lv_timer_set_period(timer, delay_ms);
            }
            return;
        }

        format_word(word_buf);
        if (timer) {
            lv_timer_set_period(timer, delay_ms);
        }
        return;
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

static lv_obj_t *create_text(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color_hex)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color_hex), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    return label;
}

static lv_obj_t *create_jump_box(
    lv_obj_t *parent,
    const char *text,
    const lv_font_t *font,
    bool selected,
    lv_coord_t x,
    lv_coord_t y)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_t *label;

    lv_obj_set_size(box, 28, 36);
    lv_obj_set_style_bg_color(box, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(
        box,
        lv_color_hex(selected ? MENU_HIGHLIGHT_COLOR : MENU_SUBTEXT_COLOR),
        0);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, x, y);

    label = lv_label_create(box);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(selected ? MENU_HIGHLIGHT_COLOR : MENU_TEXT_COLOR), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_center(label);
    return box;
}

static void render_list_items(
    lv_obj_t *screen,
    const lv_font_t *font,
    const char *const *items,
    size_t item_count,
    size_t selected_index,
    lv_coord_t start_y,
    size_t max_visible)
{
    size_t visible_count;
    size_t start_index;

    if (item_count == 0) {
        return;
    }

    visible_count = item_count < max_visible ? item_count : max_visible;
    start_index = 0;

    if (selected_index >= visible_count) {
        start_index = selected_index - visible_count + 1;
    }
    if (start_index + visible_count > item_count) {
        start_index = item_count - visible_count;
    }

    for (size_t i = 0; i < visible_count; i++) {
        size_t item_index = start_index + i;
        uint32_t color = (item_index == selected_index) ? MENU_HIGHLIGHT_COLOR : MENU_TEXT_COLOR;
        lv_obj_t *label = create_text(screen, items[item_index], font, color);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, MENU_LIST_X, start_y + (lv_coord_t)(i * MENU_LINE_STEP));
    }
}

static void format_capacity_text(char *buf, size_t buf_size, size_t bytes)
{
    if (bytes >= (1024U * 1024U)) {
        snprintf(buf, buf_size, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
        return;
    }

    snprintf(buf, buf_size, "%.0f KB", (double)bytes / 1024.0);
}

static uint8_t get_brightness_menu_index(void)
{
    uint8_t level = BK_GetLight();

    if (level <= 5) {
        return 0;
    }
    if (level <= 10) {
        return 1;
    }
    if (level <= 25) {
        return 2;
    }
    if (level <= 50) {
        return 3;
    }
    if (level <= 75) {
        return 4;
    }
    return 5;
}

static uint8_t get_led_menu_index(void)
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    if (!RGB_IsEnabled()) {
        return 0;
    }

    RGB_GetColor(&red, &green, &blue);
    for (size_t i = 1; i < (sizeof(led_colors) / sizeof(led_colors[0])); i++) {
        if (red == led_colors[i].red &&
            green == led_colors[i].green &&
            blue == led_colors[i].blue) {
            return (uint8_t)i;
        }
    }
    return 1;
}

static void apply_led_menu_selection(uint8_t selection)
{
    if (selection >= (sizeof(led_colors) / sizeof(led_colors[0]))) {
        return;
    }

    if (selection == 0) {
        RGB_SetEnabled(false);
        return;
    }

    RGB_SetColor(
        led_colors[selection].red,
        led_colors[selection].green,
        led_colors[selection].blue);
    RGB_SetEnabled(true);
}

static void stop_reader_timer(void)
{
    if (reader_timer) {
        lv_timer_del(reader_timer);
        reader_timer = NULL;
    }
}

static void toast_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (toast_label) {
        lv_obj_del(toast_label);
        toast_label = NULL;
    }
    if (toast_timer) {
        lv_timer_del(toast_timer);
        toast_timer = NULL;
    }
}

static void show_toast(const char *message)
{
    lv_obj_t *screen;

    if (current_screen != UI_SCREEN_READER) {
        return;
    }

    screen = lv_scr_act();
    if (toast_label) {
        lv_obj_del(toast_label);
        toast_label = NULL;
    }
    if (toast_timer) {
        lv_timer_del(toast_timer);
        toast_timer = NULL;
    }

    toast_label = lv_label_create(screen);
    lv_label_set_text(toast_label, message);
    lv_obj_set_style_text_font(toast_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(toast_label, lv_color_hex(MENU_TEXT_COLOR), 0);
    lv_obj_set_style_bg_color(toast_label, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(toast_label, LV_OPA_80, 0);
    lv_obj_set_style_pad_left(toast_label, 10, 0);
    lv_obj_set_style_pad_right(toast_label, 10, 0);
    lv_obj_set_style_pad_top(toast_label, 6, 0);
    lv_obj_set_style_pad_bottom(toast_label, 6, 0);
    lv_obj_set_style_radius(toast_label, 8, 0);
    lv_obj_align(toast_label, LV_ALIGN_BOTTOM_MID, 0, -24);

    toast_timer = lv_timer_create(toast_timer_cb, 900, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

static void update_reader_status(void)
{
    if (!speed_label) {
        return;
    }

    if (reader_paused) {
        lv_label_set_text_fmt(speed_label, "%u WPM  PAUSED", reading_speed_wpm);
    } else {
        lv_label_set_text_fmt(speed_label, "%u WPM", reading_speed_wpm);
    }
}

static void set_reader_paused(bool paused)
{
    reader_paused = paused;
    if (reader_paused) {
        display_save_position();
    }

    if (reader_timer) {
        if (reader_paused) {
            lv_timer_pause(reader_timer);
        } else {
            lv_timer_resume(reader_timer);
            lv_timer_set_period(reader_timer, get_base_word_delay_ms());
        }
    }

    update_reader_status();
}

static void reset_reader_objects(void)
{
    left_label = NULL;
    left_bold_label = NULL;
    anchor_label = NULL;
    anchor_bold_label = NULL;
    right_label = NULL;
    right_bold_label = NULL;
    speed_label = NULL;
    toast_label = NULL;
}

static void render_root_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    render_list_items(
        screen,
        menu_font,
        menu_items,
        sizeof(menu_items) / sizeof(menu_items[0]),
        root_menu_index,
        ROOT_LIST_Y,
        5
    );
}

static void render_books_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    lv_obj_t *title = create_text(screen, "BOOKS", menu_font, MENU_TEXT_COLOR);
    size_t book_count = display_get_book_count();
    static const char *empty_books[] = { "NO BOOKS" };
    const char *book_names[DISPLAY_MAX_BOOKS];

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);

    if (book_count == 0) {
        render_list_items(screen, menu_font, empty_books, 1, 0, SUBMENU_LIST_Y, 4);
        return;
    }

    for (size_t i = 0; i < book_count && i < DISPLAY_MAX_BOOKS; i++) {
        const char *name = display_get_book_name(i);
        book_names[i] = name ? name : "UNKNOWN";
    }

    render_list_items(screen, menu_font, book_names, book_count, book_menu_index, SUBMENU_LIST_Y, 4);
}

static void render_book_action_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    const char *book_name = display_get_book_name(book_menu_index);
    char status_line[48];
    lv_obj_t *title;
    lv_obj_t *status;

    if (!book_name) {
        book_name = "UNKNOWN";
    }

    snprintf(status_line, sizeof(status_line), "LAST POSITION: %u%%", display_get_book_progress_percent(book_menu_index));

    title = create_text(screen, book_name, &lv_font_montserrat_12, MENU_TEXT_COLOR);
    status = create_text(screen, status_line, &lv_font_montserrat_12, MENU_SUBTEXT_COLOR);

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, 8);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, MENU_LIST_X, 26);
    render_list_items(
        screen,
        menu_font,
        book_action_items,
        sizeof(book_action_items) / sizeof(book_action_items[0]),
        book_action_index,
        62,
        4
    );
}

static void render_speed_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    lv_obj_t *title = create_text(screen, "SET READING SPEED", menu_font, MENU_TEXT_COLOR);
    char speed_rows[4][24];
    const char *items[4];

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);

    for (size_t i = 0; i < 4; i++) {
        uint16_t value = (uint16_t)(reading_speed_wpm + (i * WPM_STEP));
        if (value > WPM_MAX) {
            value = (uint16_t)(WPM_MIN + ((value - WPM_MIN) % (WPM_MAX - WPM_MIN + WPM_STEP)));
        }
        snprintf(speed_rows[i], sizeof(speed_rows[i]), "%u WPM", value);
        items[i] = speed_rows[i];
    }

    render_list_items(screen, menu_font, items, 4, 0, SUBMENU_LIST_Y, 4);
}

static void render_bookmarks_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    lv_obj_t *title = create_text(screen, "BOOKMARKS", menu_font, MENU_TEXT_COLOR);
    size_t bookmark_count = display_get_bookmark_count();
    static const char *empty_items[] = { "NO BOOKMARKS" };
    static char bookmark_rows[DISPLAY_MAX_BOOKS][DISPLAY_BOOK_NAME_MAX + 8];
    const char *items[DISPLAY_MAX_BOOKS];

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);

    if (bookmark_count == 0) {
        render_list_items(screen, menu_font, empty_items, 1, 0, SUBMENU_LIST_Y, 4);
        return;
    }

    if (bookmark_menu_index >= bookmark_count) {
        bookmark_menu_index = 0;
    }

    for (size_t i = 0; i < bookmark_count; i++) {
        const char *name = display_get_bookmark_name(i);
        uint8_t percent = display_get_bookmark_progress_percent(i);
        snprintf(bookmark_rows[i], sizeof(bookmark_rows[i]), "%s %u%%", name ? name : "UNKNOWN", percent);
        items[i] = bookmark_rows[i];
    }

    render_list_items(screen, menu_font, items, bookmark_count, bookmark_menu_index, SUBMENU_LIST_Y, 4);
}

static void render_storage_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    char free_text[32];
    char total_text[32];
    char free_line[64];
    char total_line[64];
    static const char *items[2];

    format_capacity_text(free_text, sizeof(free_text), display_get_free_bytes());
    format_capacity_text(total_text, sizeof(total_text), display_get_total_bytes());
    snprintf(free_line, sizeof(free_line), "STORAGE FREE %s", free_text);
    snprintf(total_line, sizeof(total_line), "TOTAL %s", total_text);
    items[0] = free_line;
    items[1] = total_line;

    render_list_items(screen, menu_font, items, 2, 0, 32, 4);
}

static void render_brightness_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    lv_obj_t *title = create_text(screen, "SCREEN BRIGHTNESS", menu_font, MENU_TEXT_COLOR);

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);
    render_list_items(
        screen,
        menu_font,
        brightness_items,
        sizeof(brightness_items) / sizeof(brightness_items[0]),
        brightness_menu_index,
        SUBMENU_LIST_Y,
        4
    );
}

static void render_led_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    lv_obj_t *title = create_text(screen, "LED LIGHT", menu_font, MENU_TEXT_COLOR);

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);
    render_list_items(
        screen,
        menu_font,
        led_items,
        sizeof(led_items) / sizeof(led_items[0]),
        led_menu_index,
        SUBMENU_LIST_Y,
        4
    );
}

static void render_jump_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    lv_obj_t *title = create_text(screen, "JUMP TO", menu_font, MENU_TEXT_COLOR);

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);
    render_list_items(
        screen,
        menu_font,
        jump_items,
        sizeof(jump_items) / sizeof(jump_items[0]),
        jump_menu_index,
        SUBMENU_LIST_Y,
        4
    );
}

static void render_jump_custom_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    char tens_text[2] = { (char)('0' + jump_digit_tens), '\0' };
    char ones_text[2] = { (char)('0' + jump_digit_ones), '\0' };
    lv_obj_t *title = create_text(screen, "CUSTOM JUMP", menu_font, MENU_TEXT_COLOR);
    lv_obj_t *percent = create_text(screen, "%", menu_font, MENU_TEXT_COLOR);
    lv_obj_t *confirm = create_text(
        screen,
        "CONFIRM",
        menu_font,
        jump_digit_stage == 2 ? MENU_HIGHLIGHT_COLOR : MENU_TEXT_COLOR
    );

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);
    create_jump_box(screen, tens_text, menu_font, jump_digit_stage == 0, MENU_LIST_X, 68);
    create_jump_box(screen, ones_text, menu_font, jump_digit_stage == 1, MENU_LIST_X + 40, 68);
    lv_obj_align(percent, LV_ALIGN_TOP_LEFT, MENU_LIST_X + 78, 72);
    lv_obj_align(confirm, LV_ALIGN_TOP_LEFT, MENU_LIST_X, 128);
}

static void render_current_screen(void);

static void record_user_activity(void)
{
    last_activity_tick = lv_tick_get();
}

static bool ui_is_idle_for_auto_sleep(void)
{
    if (current_screen == UI_SCREEN_READER && !reader_paused) {
        return false;
    }

    return true;
}

static void enter_device_sleep(void)
{
    display_save_position();
    RGB_SaveState();
    RGB_SetEnabled(false);
    LCD_EnterSleep();
    esp_deep_sleep_start();
}

static void render_reader_screen(lv_obj_t *screen)
{
    const lv_font_t *word_font = get_word_font();
    lv_coord_t screen_w = lv_obj_get_width(screen);
    lv_obj_t *top_guide;
    lv_obj_t *bottom_guide;
    lv_obj_t *top_tick;
    lv_obj_t *bottom_tick;

    word_max_width = screen_w - (WORD_SIDE_MARGIN * 2);
    prepare_reader_buffers();

    top_guide = create_guide(screen, screen_w - (GUIDE_MARGIN_X * 2), GUIDE_THICKNESS);
    lv_obj_align(top_guide, LV_ALIGN_TOP_MID, 0, GUIDE_TOP_Y);

    bottom_guide = create_guide(screen, screen_w - (GUIDE_MARGIN_X * 2), GUIDE_THICKNESS);
    lv_obj_align(bottom_guide, LV_ALIGN_BOTTOM_MID, 0, GUIDE_BOTTOM_Y);

    top_tick = create_guide(screen, GUIDE_THICKNESS, GUIDE_TICK_LEN);
    lv_obj_align_to(top_tick, top_guide, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    bottom_tick = create_guide(screen, GUIDE_THICKNESS, GUIDE_TICK_LEN);
    lv_obj_align_to(bottom_tick, bottom_guide, LV_ALIGN_OUT_TOP_MID, 0, 0);

    speed_label = lv_label_create(screen);
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
    reader_timer = lv_timer_create(next_word_cb, get_base_word_delay_ms(), NULL);
    set_reader_paused(true);
}

static void splash_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (splash_timer) {
        lv_timer_del(splash_timer);
        splash_timer = NULL;
    }

    current_screen = UI_SCREEN_MENU;
    render_current_screen();
}

static void render_splash_screen(lv_obj_t *screen)
{
    lv_obj_t *title = create_text(screen, "PICO", &lv_font_montserrat_40, MENU_TEXT_COLOR);
    lv_obj_t *subtitle = create_text(screen, "pico read", &lv_font_montserrat_12, MENU_HIGHLIGHT_COLOR);

    lv_obj_align(title, LV_ALIGN_CENTER, 0, -10);
    lv_obj_align(subtitle, LV_ALIGN_BOTTOM_MID, 0, -14);

    if (!splash_timer) {
        splash_timer = lv_timer_create(splash_timer_cb, 1400, NULL);
        lv_timer_set_repeat_count(splash_timer, 1);
    }
}

static void render_current_screen(void)
{
    lv_obj_t *screen = lv_scr_act();

    stop_reader_timer();
    reset_reader_objects();
    lv_obj_clean(screen);
    if (toast_timer) {
        lv_timer_del(toast_timer);
        toast_timer = NULL;
    }
    if (current_screen != UI_SCREEN_SPLASH && splash_timer) {
        lv_timer_del(splash_timer);
        splash_timer = NULL;
    }
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    switch (current_screen) {
    case UI_SCREEN_SPLASH:
        render_splash_screen(screen);
        break;
    case UI_SCREEN_MENU:
        render_root_menu(screen);
        break;
    case UI_SCREEN_BOOKS:
        render_books_menu(screen);
        break;
    case UI_SCREEN_BOOK_ACTION:
        render_book_action_menu(screen);
        break;
    case UI_SCREEN_SPEED:
        render_speed_menu(screen);
        break;
    case UI_SCREEN_BOOKMARKS:
        render_bookmarks_menu(screen);
        break;
    case UI_SCREEN_STORAGE:
        render_storage_menu(screen);
        break;
    case UI_SCREEN_BRIGHTNESS:
        render_brightness_menu(screen);
        break;
    case UI_SCREEN_LED:
        render_led_menu(screen);
        break;
    case UI_SCREEN_JUMP_MENU:
        render_jump_menu(screen);
        break;
    case UI_SCREEN_JUMP_CUSTOM:
        render_jump_custom_menu(screen);
        break;
    case UI_SCREEN_READER:
        render_reader_screen(screen);
        break;
    }
}

static void handle_single_press(void)
{
    switch (current_screen) {
    case UI_SCREEN_SPLASH:
        break;
    case UI_SCREEN_MENU:
        root_menu_index = (root_menu_index + 1) % (sizeof(menu_items) / sizeof(menu_items[0]));
        render_current_screen();
        break;
    case UI_SCREEN_BOOKS:
        if (display_get_book_count() > 0) {
            book_menu_index = (book_menu_index + 1) % display_get_book_count();
            render_current_screen();
        }
        break;
    case UI_SCREEN_BOOK_ACTION:
        book_action_index = (book_action_index + 1) % (sizeof(book_action_items) / sizeof(book_action_items[0]));
        render_current_screen();
        break;
    case UI_SCREEN_SPEED:
        reading_speed_wpm += WPM_STEP;
        if (reading_speed_wpm > WPM_MAX) {
            reading_speed_wpm = WPM_MIN;
        }
        render_current_screen();
        break;
    case UI_SCREEN_BOOKMARKS:
        if (display_get_bookmark_count() > 0) {
            bookmark_menu_index = (bookmark_menu_index + 1) % display_get_bookmark_count();
            render_current_screen();
        }
        break;
    case UI_SCREEN_STORAGE:
        break;
    case UI_SCREEN_BRIGHTNESS:
        brightness_menu_index = (brightness_menu_index + 1) % (sizeof(brightness_items) / sizeof(brightness_items[0]));
        {
            static const uint8_t brightness_values[] = { 5, 10, 25, 50, 75, 100 };
            BK_Enable(true);
            BK_Light(brightness_values[brightness_menu_index]);
        }
        render_current_screen();
        break;
    case UI_SCREEN_LED:
        led_menu_index = (led_menu_index + 1) % (sizeof(led_items) / sizeof(led_items[0]));
        apply_led_menu_selection(led_menu_index);
        RGB_SaveState();
        render_current_screen();
        break;
    case UI_SCREEN_JUMP_MENU:
        jump_menu_index = (jump_menu_index + 1) % (sizeof(jump_items) / sizeof(jump_items[0]));
        render_current_screen();
        break;
    case UI_SCREEN_JUMP_CUSTOM:
        if (jump_digit_stage == 0) {
            jump_digit_tens = (uint8_t)((jump_digit_tens + 1) % 10);
            render_current_screen();
        } else if (jump_digit_stage == 1) {
            jump_digit_ones = (uint8_t)((jump_digit_ones + 1) % 10);
            render_current_screen();
        }
        break;
    case UI_SCREEN_READER:
        set_reader_paused(!reader_paused);
        break;
    }
}

static void handle_long_press(void)
{
    switch (current_screen) {
    case UI_SCREEN_SPLASH:
        break;
    case UI_SCREEN_MENU:
        switch (root_menu_index) {
        case 0:
            push_screen(UI_SCREEN_BOOKS);
            break;
        case 1:
            push_screen(UI_SCREEN_SPEED);
            break;
        case 2:
            push_screen(UI_SCREEN_BOOKMARKS);
            break;
        case 3:
            push_screen(UI_SCREEN_STORAGE);
            break;
        case 4:
            brightness_menu_index = get_brightness_menu_index();
            push_screen(UI_SCREEN_BRIGHTNESS);
            break;
        case 5:
            led_menu_index = get_led_menu_index();
            push_screen(UI_SCREEN_LED);
            break;
        case 6:
            enter_device_sleep();
            return;
        default:
            current_screen = UI_SCREEN_MENU;
            break;
        }
        render_current_screen();
        break;
    case UI_SCREEN_BOOKS:
        if (display_get_book_count() > 0) {
            book_action_index = 0;
            push_screen(UI_SCREEN_BOOK_ACTION);
            render_current_screen();
        }
        break;
    case UI_SCREEN_BOOK_ACTION:
        if (book_action_index == 0) {
            if (display_select_book(book_menu_index)) {
                push_screen(UI_SCREEN_READER);
                render_current_screen();
            }
        } else if (book_action_index == 1) {
            if (display_select_book_from_start(book_menu_index)) {
                push_screen(UI_SCREEN_READER);
                render_current_screen();
            }
        } else if (book_action_index == 2) {
            jump_menu_index = 0;
            push_screen(UI_SCREEN_JUMP_MENU);
            render_current_screen();
        }
        break;
    case UI_SCREEN_SPEED:
        pop_screen();
        render_current_screen();
        break;
    case UI_SCREEN_BOOKMARKS:
        if (display_get_bookmark_count() > 0 && display_select_bookmark(bookmark_menu_index)) {
            push_screen(UI_SCREEN_READER);
            render_current_screen();
        }
        break;
    case UI_SCREEN_STORAGE:
        break;
    case UI_SCREEN_READER:
        if (reader_paused && display_save_bookmark_for_current_book()) {
            show_toast("BOOKMARK SAVED");
        }
        break;
    case UI_SCREEN_BRIGHTNESS:
    case UI_SCREEN_LED:
        break;
    case UI_SCREEN_JUMP_CUSTOM:
        if (jump_digit_stage < 2) {
            jump_digit_stage++;
            render_current_screen();
        } else {
            uint8_t percent = (uint8_t)((jump_digit_tens * 10) + jump_digit_ones);
            if (display_select_book_at_percent(book_menu_index, percent)) {
                push_screen(UI_SCREEN_READER);
                render_current_screen();
            }
        }
        break;
    case UI_SCREEN_JUMP_MENU:
        if (jump_menu_index < 5) {
            static const uint8_t jump_percents[] = { 10, 25, 50, 75, 90 };
            if (display_select_book_at_percent(book_menu_index, jump_percents[jump_menu_index])) {
                push_screen(UI_SCREEN_READER);
                render_current_screen();
            }
        } else {
            jump_digit_tens = 0;
            jump_digit_ones = 0;
            jump_digit_stage = 0;
            push_screen(UI_SCREEN_JUMP_CUSTOM);
            render_current_screen();
        }
        break;
    }
}

static void handle_double_click(void)
{
    switch (current_screen) {
    case UI_SCREEN_SPLASH:
        break;
    case UI_SCREEN_MENU:
        break;
    case UI_SCREEN_BOOKS:
    case UI_SCREEN_SPEED:
    case UI_SCREEN_BOOKMARKS:
    case UI_SCREEN_STORAGE:
    case UI_SCREEN_BRIGHTNESS:
    case UI_SCREEN_LED:
    case UI_SCREEN_BOOK_ACTION:
    case UI_SCREEN_JUMP_MENU:
    case UI_SCREEN_JUMP_CUSTOM:
    case UI_SCREEN_READER:
        if (current_screen == UI_SCREEN_READER) {
            display_save_position();
        }
        pop_screen();
        render_current_screen();
        break;
    }
}

static void button_poll_cb(lv_timer_t *timer)
{
    bool raw_pressed;
    uint32_t now = lv_tick_get();

    (void)timer;
    raw_pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == BOOT_BUTTON_ACTIVE_LEVEL);

    if (raw_pressed != button_raw_pressed) {
        button_raw_pressed = raw_pressed;
        button_change_tick = now;
    }

    if ((now - button_change_tick) < BUTTON_DEBOUNCE_MS) {
        goto finalize_clicks;
    }

    if (button_stable_pressed != button_raw_pressed) {
        button_stable_pressed = button_raw_pressed;
        record_user_activity();

        if (button_stable_pressed) {
            button_press_tick = now;
            button_long_fired = false;
        } else if (!button_long_fired) {
            if (pending_clicks == 1 && (now - last_click_tick) <= BUTTON_DOUBLE_CLICK_MS) {
                pending_clicks = 0;
                handle_double_click();
            } else {
                pending_clicks = 1;
                last_click_tick = now;
            }
        }
    }

    if (button_stable_pressed && !button_long_fired && (now - button_press_tick) >= BUTTON_LONG_PRESS_MS) {
        button_long_fired = true;
        pending_clicks = 0;
        handle_long_press();
    }

finalize_clicks:
    if (pending_clicks == 1 && (now - last_click_tick) > BUTTON_DOUBLE_CLICK_MS) {
        pending_clicks = 0;
        handle_single_press();
    }

    if (inactivity_sleep_ms > 0 &&
        ui_is_idle_for_auto_sleep() &&
        (now - last_activity_tick) >= inactivity_sleep_ms) {
        enter_device_sleep();
    }
}

static void init_boot_button(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };

    gpio_config(&io_conf);
}

void display_words_start(void)
{
    init_boot_button();

    if (!button_timer) {
        button_timer = lv_timer_create(button_poll_cb, BUTTON_POLL_MS, NULL);
    }

    current_screen = UI_SCREEN_SPLASH;
    screen_stack_len = 0;
    root_menu_index = 0;
    book_menu_index = 0;
    bookmark_menu_index = 0;
    book_action_index = 0;
    jump_menu_index = 0;
    jump_digit_tens = 0;
    jump_digit_ones = 0;
    jump_digit_stage = 0;
    brightness_menu_index = get_brightness_menu_index();
    led_menu_index = get_led_menu_index();
    record_user_activity();
    render_current_screen();
}

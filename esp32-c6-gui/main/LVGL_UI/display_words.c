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

#define DEFAULT_WPM 300
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
/* Adjust this manually to add a pause after punctuation. Units: milliseconds */
static uint16_t punctuation_pause_ms = 500;
/* Adjust this manually to tune word opacity. Range: 0..100 percent */
static uint8_t word_opacity_percent = 100;
/* Adjust this manually to tune guideline opacity. Range: 0..100 percent */
static uint8_t guide_opacity_percent = 50;
/* Adjust this manually to set the default reading speed. Units: words per minute */
static uint16_t reading_speed_wpm = DEFAULT_WPM;

typedef enum {
    UI_SCREEN_MENU = 0,
    UI_SCREEN_BOOKS,
    UI_SCREEN_SPEED,
    UI_SCREEN_BOOKMARKS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_LED,
    UI_SCREEN_READER,
} ui_screen_t;

static const char *menu_items[] = {
    "BOOKS",
    "SET READING SPEED",
    "BOOKMARKS",
    "STORAGE",
    "LED LIGHT",
    "SHUT DOWN",
};

static ui_screen_t current_screen = UI_SCREEN_MENU;
static ui_screen_t screen_stack[4];
static uint8_t screen_stack_len;
static uint8_t root_menu_index;
static uint8_t book_menu_index;

static lv_obj_t *left_label;
static lv_obj_t *left_bold_label;
static lv_obj_t *anchor_label;
static lv_obj_t *anchor_bold_label;
static lv_obj_t *right_label;
static lv_obj_t *right_bold_label;
static lv_obj_t *speed_label;

static lv_timer_t *reader_timer;
static lv_timer_t *button_timer;
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

static bool button_raw_pressed;
static bool button_stable_pressed;
static bool button_long_fired;
static uint8_t pending_clicks;
static uint32_t button_change_tick;
static uint32_t button_press_tick;
static uint32_t last_click_tick;

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

static uint32_t get_word_delay_ms(const char *word)
{
    size_t len = strlen(word);
    if (len == 0) {
        return get_base_word_delay_ms();
    }

    for (size_t i = len; i > 0; i--) {
        char ch = word[i - 1];
        if (ch == '.' || ch == ',' || ch == ';' || ch == ':' || ch == '!' || ch == '?') {
            return get_base_word_delay_ms() + punctuation_pause_ms;
        }
        if (isalnum((unsigned char)ch)) {
            break;
        }
    }

    return get_base_word_delay_ms();
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
    return next_chunk(chunk_start, chunk_len);
}

static void prepare_reader_buffers(void)
{
    pending_token_len = 0;
    pending_token_offset = 0;

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

static lv_obj_t *create_text(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color_hex)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color_hex), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    return label;
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

static void stop_reader_timer(void)
{
    if (reader_timer) {
        lv_timer_del(reader_timer);
        reader_timer = NULL;
    }
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
    static const char *items[] = { "EMPTY FOR NOW" };

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);
    render_list_items(screen, menu_font, items, 1, 0, SUBMENU_LIST_Y, 4);
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

static void render_led_menu(lv_obj_t *screen)
{
    const lv_font_t *menu_font = get_menu_font();
    static const char *items[] = { "ON", "OFF" };
    size_t selected = RGB_IsEnabled() ? 0 : 1;
    lv_obj_t *title = create_text(screen, "LED LIGHT", menu_font, MENU_TEXT_COLOR);

    lv_obj_align(title, LV_ALIGN_TOP_LEFT, MENU_LIST_X, SUBMENU_TITLE_Y);
    render_list_items(screen, menu_font, items, 2, selected, SUBMENU_LIST_Y, 4);
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

static void render_current_screen(void)
{
    lv_obj_t *screen = lv_scr_act();

    stop_reader_timer();
    reset_reader_objects();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    switch (current_screen) {
    case UI_SCREEN_MENU:
        render_root_menu(screen);
        break;
    case UI_SCREEN_BOOKS:
        render_books_menu(screen);
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
    case UI_SCREEN_LED:
        render_led_menu(screen);
        break;
    case UI_SCREEN_READER:
        render_reader_screen(screen);
        break;
    }
}

static void handle_single_press(void)
{
    switch (current_screen) {
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
    case UI_SCREEN_SPEED:
        reading_speed_wpm += WPM_STEP;
        if (reading_speed_wpm > WPM_MAX) {
            reading_speed_wpm = WPM_MIN;
        }
        render_current_screen();
        break;
    case UI_SCREEN_BOOKMARKS:
    case UI_SCREEN_STORAGE:
        break;
    case UI_SCREEN_LED:
        RGB_SetEnabled(!RGB_IsEnabled());
        render_current_screen();
        break;
    case UI_SCREEN_READER:
        set_reader_paused(!reader_paused);
        break;
    }
}

static void handle_long_press(void)
{
    switch (current_screen) {
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
            push_screen(UI_SCREEN_LED);
            break;
        case 5:
            display_save_position();
            RGB_SetEnabled(false);
            LCD_EnterSleep();
            esp_deep_sleep_start();
            return;
        default:
            current_screen = UI_SCREEN_MENU;
            break;
        }
        render_current_screen();
        break;
    case UI_SCREEN_BOOKS:
        if (display_get_book_count() > 0 && display_select_book(book_menu_index)) {
            push_screen(UI_SCREEN_READER);
            render_current_screen();
        }
        break;
    case UI_SCREEN_SPEED:
        pop_screen();
        render_current_screen();
        break;
    case UI_SCREEN_BOOKMARKS:
    case UI_SCREEN_STORAGE:
    case UI_SCREEN_LED:
    case UI_SCREEN_READER:
        break;
    }
}

static void handle_double_click(void)
{
    switch (current_screen) {
    case UI_SCREEN_MENU:
        break;
    case UI_SCREEN_BOOKS:
    case UI_SCREEN_SPEED:
    case UI_SCREEN_BOOKMARKS:
    case UI_SCREEN_STORAGE:
    case UI_SCREEN_LED:
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

    current_screen = UI_SCREEN_MENU;
    screen_stack_len = 0;
    root_menu_index = 0;
    book_menu_index = 0;
    render_current_screen();
}

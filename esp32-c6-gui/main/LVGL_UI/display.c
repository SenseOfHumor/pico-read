#include "display.h"

#include "esp_log.h"
#include "esp_spiffs.h"

#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOK_STORAGE_BASE_PATH "/spiflash"
#define BOOK_STORAGE_PARTITION_LABEL "flash_test"

static const char *TAG = "BOOK_STORAGE";
static char s_fallback_text[96] = "BOOK ERR";

static bool s_mounted;
static FILE *s_book_file;
static const char *s_fallback_cursor;
static char s_book_names[DISPLAY_MAX_BOOKS][DISPLAY_BOOK_NAME_MAX];
static size_t s_book_count;
static size_t s_selected_book_index;
static size_t s_total_bytes;
static size_t s_used_bytes;

static void set_fallback_text(const char *message)
{
    snprintf(s_fallback_text, sizeof(s_fallback_text), "%s", message);
}

static void update_storage_info(void)
{
    size_t total = 0;
    size_t used = 0;

    if (esp_spiffs_info(BOOK_STORAGE_PARTITION_LABEL, &total, &used) == ESP_OK) {
        s_total_bytes = total;
        s_used_bytes = used;
    } else {
        s_total_bytes = 0;
        s_used_bytes = 0;
    }
}

static void close_current_book(void)
{
    if (s_book_file) {
        fclose(s_book_file);
        s_book_file = NULL;
    }
}

static bool is_supported_book_name(const char *name)
{
    const char *ext;
    size_t len;

    if (!name || name[0] == '.') {
        return false;
    }

    len = strlen(name);
    if (len == 0 || len >= DISPLAY_BOOK_NAME_MAX) {
        return false;
    }

    ext = strrchr(name, '.');
    if (!ext) {
        return false;
    }

    return strcmp(ext, ".txt") == 0;
}

static void scan_books(void)
{
    DIR *dir = opendir(BOOK_STORAGE_BASE_PATH);
    struct dirent *entry;

    s_book_count = 0;

    if (!dir) {
        ESP_LOGW(TAG, "Could not open directory %s", BOOK_STORAGE_BASE_PATH);
        set_fallback_text("BOOK DIR FAIL");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!is_supported_book_name(entry->d_name)) {
            continue;
        }
        if (s_book_count >= DISPLAY_MAX_BOOKS) {
            break;
        }

        strncpy(s_book_names[s_book_count], entry->d_name, DISPLAY_BOOK_NAME_MAX - 1);
        s_book_names[s_book_count][DISPLAY_BOOK_NAME_MAX - 1] = '\0';
        s_book_count++;
    }

    closedir(dir);

    for (size_t i = 0; i < s_book_count; i++) {
        for (size_t j = i + 1; j < s_book_count; j++) {
            if (strcmp(s_book_names[i], s_book_names[j]) > 0) {
                char tmp[DISPLAY_BOOK_NAME_MAX];
                memcpy(tmp, s_book_names[i], sizeof(tmp));
                memcpy(s_book_names[i], s_book_names[j], DISPLAY_BOOK_NAME_MAX);
                memcpy(s_book_names[j], tmp, sizeof(tmp));
            }
        }
    }

    if (s_book_count == 0) {
        set_fallback_text("NO BOOKS FOUND");
        return;
    }

    s_selected_book_index = 0;
}

static bool open_book_by_index(size_t index)
{
    char path[DISPLAY_BOOK_NAME_MAX + 32];

    if (index >= s_book_count) {
        set_fallback_text("BOOK INDEX FAIL");
        return false;
    }

    close_current_book();
    snprintf(path, sizeof(path), "%s/%s", BOOK_STORAGE_BASE_PATH, s_book_names[index]);
    s_book_file = fopen(path, "rb");
    if (!s_book_file) {
        ESP_LOGW(TAG, "Could not open %s", path);
        set_fallback_text("BOOK OPEN FAIL");
        return false;
    }

    s_selected_book_index = index;
    ESP_LOGI(TAG, "Opened %s for streaming", path);
    return true;
}

bool display_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BOOK_STORAGE_BASE_PATH,
        .partition_label = BOOK_STORAGE_PARTITION_LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t err;

    if (s_mounted) {
        return true;
    }

    err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS storage: %s", esp_err_to_name(err));
        set_fallback_text("BOOK MOUNT FAIL");
        return false;
    }
    s_mounted = true;

    update_storage_info();
    scan_books();

    if (s_book_count == 0) {
        s_fallback_cursor = s_fallback_text;
        return false;
    }

    if (!open_book_by_index(0)) {
        s_fallback_cursor = s_fallback_text;
        return false;
    }

    s_fallback_cursor = NULL;
    return true;
}

void display_reset(void)
{
    if (s_book_file) {
        rewind(s_book_file);
    }
    s_fallback_cursor = s_fallback_text;
}

static bool next_token_from_fallback(char *buf, size_t buf_size)
{
    size_t len = 0;

    if (!s_fallback_cursor || !buf || buf_size < 2) {
        return false;
    }

    while (*s_fallback_cursor && isspace((unsigned char)*s_fallback_cursor)) {
        s_fallback_cursor++;
    }
    if (!*s_fallback_cursor) {
        s_fallback_cursor = s_fallback_text;
        while (*s_fallback_cursor && isspace((unsigned char)*s_fallback_cursor)) {
            s_fallback_cursor++;
        }
    }
    if (!*s_fallback_cursor) {
        return false;
    }

    while (*s_fallback_cursor && !isspace((unsigned char)*s_fallback_cursor)) {
        if (len + 1 < buf_size) {
            buf[len++] = *s_fallback_cursor;
        }
        s_fallback_cursor++;
    }

    buf[len] = '\0';
    return len > 0;
}

static size_t normalize_utf8_symbol(FILE *file, int first_byte, char *buf, size_t buf_size, size_t len)
{
    int b2;
    int b3;

    if (first_byte != 0xE2 || (len + 1 >= buf_size)) {
        return len;
    }

    b2 = fgetc(file);
    if (b2 == EOF) {
        return len;
    }

    b3 = fgetc(file);
    if (b3 == EOF) {
        return len;
    }

    if (b2 == 0x80 && (b3 == 0x9C || b3 == 0x9D)) {
        buf[len++] = '"';
        return len;
    }

    if (b2 == 0x80 && (b3 == 0x98 || b3 == 0x99)) {
        buf[len++] = '\'';
        return len;
    }

    if (b2 == 0x80 && (b3 == 0x93 || b3 == 0x94)) {
        buf[len++] = '-';
        return len;
    }

    return len;
}

bool display_next_token(char *buf, size_t buf_size)
{
    int ch;
    size_t len = 0;

    if (!buf || buf_size < 2) {
        return false;
    }

    if (!s_book_file) {
        return next_token_from_fallback(buf, buf_size);
    }

    for (;;) {
        ch = fgetc(s_book_file);
        if (ch == EOF) {
            clearerr(s_book_file);
            rewind(s_book_file);
            if (len > 0) {
                break;
            }
            continue;
        }

        if (isspace((unsigned char)ch)) {
            if (len > 0) {
                break;
            }
            continue;
        }

        if ((unsigned char)ch >= 0x80) {
            len = normalize_utf8_symbol(s_book_file, ch, buf, buf_size, len);
            continue;
        }

        if (len + 1 < buf_size) {
            buf[len++] = (char)ch;
        }
    }

    buf[len] = '\0';
    return len > 0;
}

size_t display_get_total_bytes(void)
{
    update_storage_info();
    return s_total_bytes;
}

size_t display_get_used_bytes(void)
{
    update_storage_info();
    return s_used_bytes;
}

size_t display_get_free_bytes(void)
{
    update_storage_info();
    if (s_total_bytes < s_used_bytes) {
        return 0;
    }
    return s_total_bytes - s_used_bytes;
}

size_t display_get_book_count(void)
{
    return s_book_count;
}

const char *display_get_book_name(size_t index)
{
    if (index >= s_book_count) {
        return NULL;
    }
    return s_book_names[index];
}

bool display_select_book(size_t index)
{
    if (!open_book_by_index(index)) {
        s_fallback_cursor = s_fallback_text;
        return false;
    }

    display_reset();
    s_fallback_cursor = NULL;
    return true;
}

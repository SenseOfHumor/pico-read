#include "display.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <dirent.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOK_STORAGE_BASE_PATH "/spiflash"
#define BOOK_STORAGE_PARTITION_LABEL "flash_test"

static const char *TAG = "BOOK_STORAGE";
static char s_fallback_text[96] = "BOOK ERR";
static const char *BOOKMARK_NS = "reader";
static const char *RESUME_KEY = "resume";
static const char *BOOKMARK_KEY = "marks";

static bool s_mounted;
static FILE *s_book_file;
static const char *s_fallback_cursor;
static char s_book_names[DISPLAY_MAX_BOOKS][DISPLAY_BOOK_NAME_MAX];
static size_t s_book_count;
static size_t s_selected_book_index;
static size_t s_total_bytes;
static size_t s_used_bytes;
static nvs_handle_t s_nvs_handle;
static bool s_nvs_ready;

typedef struct {
    char book_name[DISPLAY_BOOK_NAME_MAX];
    uint32_t offset;
} bookmark_entry_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    bookmark_entry_t entries[DISPLAY_MAX_BOOKS];
} bookmark_blob_t;

static bookmark_blob_t s_bookmarks;
static bookmark_blob_t s_manual_bookmarks;

static void set_fallback_text(const char *message)
{
    snprintf(s_fallback_text, sizeof(s_fallback_text), "%s", message);
}

static void init_bookmarks(void)
{
    esp_err_t err;
    size_t required_size = sizeof(s_bookmarks);

    memset(&s_bookmarks, 0, sizeof(s_bookmarks));
    s_bookmarks.version = 1;
    memset(&s_manual_bookmarks, 0, sizeof(s_manual_bookmarks));
    s_manual_bookmarks.version = 1;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_open(BOOKMARK_NS, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    s_nvs_ready = true;

    err = nvs_get_blob(s_nvs_handle, RESUME_KEY, &s_bookmarks, &required_size);
    if (err != ESP_OK || required_size != sizeof(s_bookmarks) || s_bookmarks.version != 1) {
        memset(&s_bookmarks, 0, sizeof(s_bookmarks));
        s_bookmarks.version = 1;
    }

    required_size = sizeof(s_manual_bookmarks);
    err = nvs_get_blob(s_nvs_handle, BOOKMARK_KEY, &s_manual_bookmarks, &required_size);
    if (err != ESP_OK || required_size != sizeof(s_manual_bookmarks) || s_manual_bookmarks.version != 1) {
        memset(&s_manual_bookmarks, 0, sizeof(s_manual_bookmarks));
        s_manual_bookmarks.version = 1;
    }
}

static int find_bookmark_index(const char *book_name)
{
    for (uint32_t i = 0; i < s_bookmarks.count && i < DISPLAY_MAX_BOOKS; i++) {
        if (strcmp(s_bookmarks.entries[i].book_name, book_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_manual_bookmark_index(const char *book_name)
{
    for (uint32_t i = 0; i < s_manual_bookmarks.count && i < DISPLAY_MAX_BOOKS; i++) {
        if (strcmp(s_manual_bookmarks.entries[i].book_name, book_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t get_saved_offset_for_book(const char *book_name)
{
    int index = find_bookmark_index(book_name);
    if (index < 0) {
        return 0;
    }
    return s_bookmarks.entries[index].offset;
}

static void persist_bookmarks(void)
{
    if (!s_nvs_ready) {
        return;
    }
    nvs_set_blob(s_nvs_handle, RESUME_KEY, &s_bookmarks, sizeof(s_bookmarks));
    nvs_commit(s_nvs_handle);
}

static void persist_manual_bookmarks(void)
{
    if (!s_nvs_ready) {
        return;
    }
    nvs_set_blob(s_nvs_handle, BOOKMARK_KEY, &s_manual_bookmarks, sizeof(s_manual_bookmarks));
    nvs_commit(s_nvs_handle);
}

static void set_saved_offset_for_book(const char *book_name, uint32_t offset)
{
    int index;

    if (!book_name || !s_nvs_ready) {
        return;
    }

    index = find_bookmark_index(book_name);
    if (index < 0) {
        if (s_bookmarks.count >= DISPLAY_MAX_BOOKS) {
            return;
        }
        index = (int)s_bookmarks.count++;
        strncpy(s_bookmarks.entries[index].book_name, book_name, DISPLAY_BOOK_NAME_MAX - 1);
        s_bookmarks.entries[index].book_name[DISPLAY_BOOK_NAME_MAX - 1] = '\0';
    }

    s_bookmarks.entries[index].offset = offset;
    persist_bookmarks();
}

static void set_manual_bookmark_for_book(const char *book_name, uint32_t offset)
{
    int index;

    if (!book_name || !s_nvs_ready) {
        return;
    }

    index = find_manual_bookmark_index(book_name);
    if (index < 0) {
        if (s_manual_bookmarks.count >= DISPLAY_MAX_BOOKS) {
            return;
        }
        index = (int)s_manual_bookmarks.count++;
        strncpy(s_manual_bookmarks.entries[index].book_name, book_name, DISPLAY_BOOK_NAME_MAX - 1);
        s_manual_bookmarks.entries[index].book_name[DISPLAY_BOOK_NAME_MAX - 1] = '\0';
    }

    s_manual_bookmarks.entries[index].offset = offset;
    persist_manual_bookmarks();
}

static uint32_t get_manual_bookmark_offset_for_book(const char *book_name)
{
    int index = find_manual_bookmark_index(book_name);
    if (index < 0) {
        return 0;
    }
    return s_manual_bookmarks.entries[index].offset;
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

static long get_book_size_by_name(const char *book_name)
{
    char path[DISPLAY_BOOK_NAME_MAX + 32];
    FILE *file;
    long size;

    if (!book_name) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/%s", BOOK_STORAGE_BASE_PATH, book_name);
    file = fopen(path, "rb");
    if (!file) {
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }

    size = ftell(file);
    fclose(file);
    return size;
}

static void align_stream_to_token_boundary(FILE *file, uint32_t offset)
{
    int ch;

    if (!file || offset == 0) {
        return;
    }

    while ((ch = fgetc(file)) != EOF) {
        if (isspace((unsigned char)ch)) {
            break;
        }
    }
}

static bool seek_current_book_offset(uint32_t offset)
{
    long file_size;

    if (!s_book_file) {
        return false;
    }

    if (fseek(s_book_file, 0, SEEK_END) != 0) {
        rewind(s_book_file);
        return false;
    }

    file_size = ftell(s_book_file);
    if (file_size < 0) {
        rewind(s_book_file);
        return false;
    }

    if (offset >= (uint32_t)file_size) {
        offset = 0;
    }

    if (fseek(s_book_file, (long)offset, SEEK_SET) != 0) {
        rewind(s_book_file);
        return false;
    }

    align_stream_to_token_boundary(s_book_file, offset);
    return true;
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
    uint32_t saved_offset;

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
    saved_offset = get_saved_offset_for_book(s_book_names[index]);
    seek_current_book_offset(saved_offset);
    ESP_LOGI(TAG, "Opened %s for streaming", path);
    return true;
}

static void set_current_book_offset(uint32_t offset, bool persist)
{
    if (s_selected_book_index >= s_book_count) {
        return;
    }

    seek_current_book_offset(offset);
    if (persist) {
        set_saved_offset_for_book(s_book_names[s_selected_book_index], offset);
    }
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
    init_bookmarks();

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
        uint32_t saved_offset = get_saved_offset_for_book(s_book_names[s_selected_book_index]);
        set_current_book_offset(saved_offset, false);
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

void display_save_position(void)
{
    long offset;

    if (!s_book_file || s_selected_book_index >= s_book_count) {
        return;
    }

    offset = ftell(s_book_file);
    if (offset < 0) {
        return;
    }

    set_saved_offset_for_book(s_book_names[s_selected_book_index], (uint32_t)offset);
}

const char *display_get_current_book_name(void)
{
    if (s_selected_book_index >= s_book_count) {
        return NULL;
    }
    return s_book_names[s_selected_book_index];
}

uint8_t display_get_book_progress_percent(size_t index)
{
    long file_size;
    uint32_t offset;

    if (index >= s_book_count) {
        return 0;
    }

    file_size = get_book_size_by_name(s_book_names[index]);
    if (file_size <= 0) {
        return 0;
    }

    offset = get_saved_offset_for_book(s_book_names[index]);
    if (offset >= (uint32_t)file_size) {
        offset = 0;
    }

    return (uint8_t)((offset * 100U) / (uint32_t)file_size);
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
    display_save_position();
    if (!open_book_by_index(index)) {
        s_fallback_cursor = s_fallback_text;
        return false;
    }

    display_reset();
    s_fallback_cursor = NULL;
    return true;
}

bool display_select_book_from_start(size_t index)
{
    display_save_position();
    if (!open_book_by_index(index)) {
        s_fallback_cursor = s_fallback_text;
        return false;
    }

    set_current_book_offset(0, true);
    s_fallback_cursor = NULL;
    return true;
}

bool display_select_book_at_percent(size_t index, uint8_t percent)
{
    long file_size;
    uint32_t offset;

    if (percent > 99) {
        percent = 99;
    }

    display_save_position();
    if (!open_book_by_index(index)) {
        s_fallback_cursor = s_fallback_text;
        return false;
    }

    file_size = get_book_size_by_name(s_book_names[index]);
    if (file_size <= 0) {
        set_current_book_offset(0, true);
        s_fallback_cursor = NULL;
        return true;
    }

    offset = ((uint32_t)file_size * percent) / 100U;
    set_current_book_offset(offset, true);
    s_fallback_cursor = NULL;
    return true;
}

bool display_save_bookmark_for_current_book(void)
{
    long offset;

    if (!s_book_file || s_selected_book_index >= s_book_count) {
        return false;
    }

    offset = ftell(s_book_file);
    if (offset < 0) {
        return false;
    }

    set_manual_bookmark_for_book(s_book_names[s_selected_book_index], (uint32_t)offset);
    return true;
}

static bool book_has_manual_bookmark(const char *book_name)
{
    return find_manual_bookmark_index(book_name) >= 0;
}

static int bookmark_index_to_book_index(size_t bookmark_index)
{
    size_t current = 0;

    for (size_t i = 0; i < s_book_count; i++) {
        if (!book_has_manual_bookmark(s_book_names[i])) {
            continue;
        }
        if (current == bookmark_index) {
            return (int)i;
        }
        current++;
    }

    return -1;
}

size_t display_get_bookmark_count(void)
{
    size_t count = 0;

    for (size_t i = 0; i < s_book_count; i++) {
        if (book_has_manual_bookmark(s_book_names[i])) {
            count++;
        }
    }

    return count;
}

const char *display_get_bookmark_name(size_t index)
{
    int book_index = bookmark_index_to_book_index(index);
    if (book_index < 0) {
        return NULL;
    }
    return s_book_names[book_index];
}

uint8_t display_get_bookmark_progress_percent(size_t index)
{
    int book_index = bookmark_index_to_book_index(index);
    long file_size;
    uint32_t offset;

    if (book_index < 0) {
        return 0;
    }

    file_size = get_book_size_by_name(s_book_names[book_index]);
    if (file_size <= 0) {
        return 0;
    }

    offset = get_manual_bookmark_offset_for_book(s_book_names[book_index]);
    if (offset >= (uint32_t)file_size) {
        offset = 0;
    }

    return (uint8_t)((offset * 100U) / (uint32_t)file_size);
}

bool display_select_bookmark(size_t index)
{
    int book_index = bookmark_index_to_book_index(index);
    uint32_t offset;

    if (book_index < 0) {
        return false;
    }

    display_save_position();
    if (!open_book_by_index((size_t)book_index)) {
        s_fallback_cursor = s_fallback_text;
        return false;
    }

    offset = get_manual_bookmark_offset_for_book(s_book_names[book_index]);
    set_current_book_offset(offset, true);
    s_fallback_cursor = NULL;
    return true;
}

#include "display.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"

#include <dirent.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOK_STORAGE_BASE_PATH "/spiflash"
#define BOOK_STORAGE_PARTITION_LABEL "flash_test"
#define BOOK_STORAGE_FILE_PATH BOOK_STORAGE_BASE_PATH "/paragraph.txt"

static const char *TAG = "BOOK_STORAGE";
static char s_fallback_text[96] = "BOOK ERR";

static bool s_mounted;
static FILE *s_book_file;
static const char *s_fallback_cursor;

static void set_fallback_text(const char *message)
{
    snprintf(s_fallback_text, sizeof(s_fallback_text), "%s", message);
}

static void log_storage_dir(void)
{
    DIR *dir = opendir(BOOK_STORAGE_BASE_PATH);
    struct dirent *entry;

    if (!dir) {
        ESP_LOGW(TAG, "Could not open directory %s", BOOK_STORAGE_BASE_PATH);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "Dir entry: %s", entry->d_name);
    }

    closedir(dir);
}

static bool open_book_from_storage(void)
{
    s_book_file = fopen(BOOK_STORAGE_FILE_PATH, "rb");
    if (!s_book_file) {
        ESP_LOGW(TAG, "Could not open %s", BOOK_STORAGE_FILE_PATH);
        set_fallback_text("BOOK OPEN FAIL");
        return false;
    }
    ESP_LOGI(TAG, "Opened %s for streaming", BOOK_STORAGE_FILE_PATH);
    return true;
}

bool display_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BOOK_STORAGE_BASE_PATH,
        .partition_label = BOOK_STORAGE_PARTITION_LABEL,
        .max_files = 4,
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

    log_storage_dir();

    if (!open_book_from_storage()) {
        ESP_LOGW(TAG, "Falling back because %s could not be loaded", BOOK_STORAGE_FILE_PATH);
        s_fallback_cursor = s_fallback_text;
        return false;
    }

    s_fallback_cursor = NULL;
    ESP_LOGI(TAG, "Streaming book text from %s", BOOK_STORAGE_FILE_PATH);
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

        if (len + 1 < buf_size) {
            buf[len++] = (char)ch;
        }
    }

    buf[len] = '\0';
    return len > 0;
}

size_t display_get_capacity_bytes(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        BOOK_STORAGE_PARTITION_LABEL
    );

    if (!partition) {
        return 0;
    }

    return partition->size;
}

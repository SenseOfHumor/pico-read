#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_TOKEN_MAX_LEN 256
#define DISPLAY_MAX_BOOKS 32
#define DISPLAY_BOOK_NAME_MAX 96

bool display_init(void);
bool display_next_token(char *buf, size_t buf_size);
void display_reset(void);
void display_save_position(void);
const char *display_get_current_book_name(void);
uint8_t display_get_book_progress_percent(size_t index);
size_t display_get_total_bytes(void);
size_t display_get_used_bytes(void);
size_t display_get_free_bytes(void);
size_t display_get_book_count(void);
const char *display_get_book_name(size_t index);
bool display_select_book(size_t index);
bool display_select_book_from_start(size_t index);
bool display_select_book_at_percent(size_t index, uint8_t percent);
bool display_save_bookmark_for_current_book(void);
size_t display_get_bookmark_count(void);
const char *display_get_bookmark_name(size_t index);
uint8_t display_get_bookmark_progress_percent(size_t index);
bool display_select_bookmark(size_t index);

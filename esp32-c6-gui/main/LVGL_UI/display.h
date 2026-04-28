#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DISPLAY_TOKEN_MAX_LEN 256
#define DISPLAY_MAX_BOOKS 32
#define DISPLAY_BOOK_NAME_MAX 96

bool display_init(void);
bool display_next_token(char *buf, size_t buf_size);
void display_reset(void);
void display_save_position(void);
const char *display_get_current_book_name(void);
size_t display_get_total_bytes(void);
size_t display_get_used_bytes(void);
size_t display_get_free_bytes(void);
size_t display_get_book_count(void);
const char *display_get_book_name(size_t index);
bool display_select_book(size_t index);

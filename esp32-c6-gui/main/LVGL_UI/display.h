#pragma once

#include <stdbool.h>
#include <stddef.h>

#define DISPLAY_TOKEN_MAX_LEN 256

bool display_init(void);
bool display_next_token(char *buf, size_t buf_size);
void display_reset(void);
size_t display_get_capacity_bytes(void);

#pragma once

#include "driver/gpio.h"
#include "led_strip.h"

#define BLINK_GPIO 8

void RGB_Init(void);
void Set_RGB( uint8_t red_val, uint8_t green_val, uint8_t blue_val);
void RGB_SetColor(uint8_t red_val, uint8_t green_val, uint8_t blue_val);
void RGB_GetColor(uint8_t *red_val, uint8_t *green_val, uint8_t *blue_val);
void RGB_SetEnabled(bool enabled);
bool RGB_IsEnabled(void);
void RGB_SaveState(void);
void RGB_Example(void);

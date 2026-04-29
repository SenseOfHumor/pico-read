/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "ST7789.h"
#include "display.h"
#include "display_words.h"
#include "RGB.h"

void app_main(void)
{
    LCD_Init();
    BK_Enable(false);
    RGB_Init();
    display_init();
    LVGL_Init();

/********************* Demo *********************/
    display_words_start();
    lv_timer_handler();
    BK_Enable(true);
    BK_Light(80);

    // lv_demo_widgets();
    // lv_demo_keypad_encoder();
    // lv_demo_benchmark();
    // lv_demo_stress();
    // lv_demo_music();

    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
    }
}

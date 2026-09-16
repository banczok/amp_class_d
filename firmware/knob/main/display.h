#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

esp_err_t display_init(void);      /* ST7701S + RGB panel + LVGL + touch */
lv_disp_t *display_lv(void);

void display_backlight(uint8_t percent);   /* fades, 0 = off             */
void display_sleep(void);                  /* backlight off, panel off   */

bool display_lock(uint32_t timeout_ms);    /* take the LVGL mutex        */
void display_unlock(void);
#endif

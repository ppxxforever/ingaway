#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Single UI task owns all drawing operations. */
esp_err_t lcd_init(void);
void lcd_clear(uint16_t color);
void lcd_rect(int x, int y, int w, int h, uint16_t color);
void lcd_text(int x, int y, const char *text, uint16_t color, int scale);
esp_err_t lcd_present(void);
esp_err_t lcd_sleep(bool sleep);

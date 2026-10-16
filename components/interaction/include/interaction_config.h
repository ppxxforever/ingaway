#pragma once

/* ST7789 240x320 portrait. Check the actual board wiring before powering on. */
#define LCD_WIDTH 240
#define LCD_HEIGHT 320
#define LCD_SPI_HOST SPI3_HOST
#define LCD_PIN_SCK 14
#define LCD_PIN_MOSI 15
#define LCD_PIN_CS 16
#define LCD_PIN_DC 17
#define LCD_PIN_RST 18
/* Logic-level backlight enable only; use -1 for externally powered backlight. */
#define LCD_PIN_BL 21
#define LCD_BL_ACTIVE_LEVEL 1
#define LCD_SPI_HZ (20 * 1000 * 1000)
#define LCD_BGR 0
#define LCD_INVERT_COLOR 1
#define LCD_MIRROR_X 0
#define LCD_MIRROR_Y 0
#define LCD_X_GAP 0
#define LCD_Y_GAP 0
#define LCD_TEST_ENABLED 1
/* Blank the LCD/backlight after this much UI inactivity. Input sampling keeps
 * running; the first touch/button/encoder event only wakes the display. */
#define LCD_IDLE_SLEEP_MS 30000

#define ENCODER_PIN_A 4
#define ENCODER_PIN_B 5
#define ENCODER_PIN_SW 6
/* SW active level: most encoder modules short SW to GND when pressed (0).
 * If your module drives SW high when pressed, set this to 1; the input pull
 * direction in interaction.c follows this value automatically. */
#define ENCODER_SW_ACTIVE_LEVEL 0
#define ENCODER_TRANSITIONS_PER_STEP 4
#define ENCODER_REVERSE 0
#define ENCODER_DEBOUNCE_MS 20
#define ENCODER_LONG_PRESS_MS 800

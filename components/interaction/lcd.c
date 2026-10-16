#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "interaction_config.h"
#include "lcd.h"

static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t done;
static uint16_t *frame;
static uint8_t *dma;

static bool color_done(esp_lcd_panel_io_handle_t handle,
                       esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

esp_err_t lcd_init(void)
{
    esp_err_t err = ESP_ERR_NO_MEM;
    bool bus_ready = false;
    done = xSemaphoreCreateBinary();
    /* Keep the 150 KiB framebuffer out of internal RAM; if PSRAM is not
     * available, fail the optional UI instead of starving network/OTA tasks. */
    frame = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(*frame), MALLOC_CAP_SPIRAM);
    dma = heap_caps_malloc(LCD_WIDTH * 16 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!done || !frame || !dma) goto fail;
#define TRY(call) do { err = (call); if (err != ESP_OK) goto fail; } while (0)
    if (LCD_PIN_BL >= 0) {
        TRY(gpio_set_direction(LCD_PIN_BL, GPIO_MODE_OUTPUT));
        TRY(gpio_set_level(LCD_PIN_BL, !LCD_BL_ACTIVE_LEVEL));
    }
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI, .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 16 * 2,
    };
    TRY(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    bus_ready = true;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS, .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0, .pclk_hz = LCD_SPI_HZ, .trans_queue_depth = 1,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = color_done, .user_ctx = done,
    };
    TRY(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &io));
    esp_lcd_panel_dev_config_t config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    TRY(esp_lcd_new_panel_st7789(io, &config, &panel));
    TRY(esp_lcd_panel_reset(panel));
    TRY(esp_lcd_panel_init(panel));
    TRY(esp_lcd_panel_swap_xy(panel, false));
    TRY(esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    TRY(esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP));
    TRY(esp_lcd_panel_invert_color(panel, LCD_INVERT_COLOR));
    lcd_clear(0);
    TRY(lcd_present());
    TRY(esp_lcd_panel_disp_on_off(panel, true));
    if (LCD_PIN_BL >= 0) TRY(gpio_set_level(LCD_PIN_BL, LCD_BL_ACTIVE_LEVEL));
    return ESP_OK;
fail:
    if (panel) { esp_lcd_panel_del(panel); panel = NULL; }
    if (io) { esp_lcd_panel_io_del(io); io = NULL; }
    if (bus_ready) spi_bus_free(LCD_SPI_HOST);
    if (done) { vSemaphoreDelete(done); done = NULL; }
    free(frame); frame = NULL;
    free(dma); dma = NULL;
    return err;
#undef TRY
}

void lcd_clear(uint16_t color)
{
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i) frame[i] = color;
}

void lcd_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int py = y; py < y + h; ++py) {
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int px = x; px < x + w; ++px) {
            if (px >= 0 && px < LCD_WIDTH) frame[py * LCD_WIDTH + px] = color;
        }
    }
}

/* 5x7 column font: digits followed by uppercase ASCII letters. */
static const uint8_t font[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},
    {0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,1},
    {0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e},{0x7f,9,9,9,6},
    {0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},
    {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},
};

void lcd_text(int x, int y, const char *text, uint16_t color, int scale)
{
    for (; *text; ++text, x += 6 * scale) {
        int index = *text >= '0' && *text <= '9' ? *text - '0' :
                    *text >= 'A' && *text <= 'Z' ? *text - 'A' + 10 : -1;
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = index >= 0 ? font[index][col] :
                           *text == '-' ? 8 : *text == ':' && col == 2 ? 0x14 : 0;
            for (int row = 0; row < 7; ++row)
                if (bits & (1 << row)) lcd_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

esp_err_t lcd_sleep(bool sleep)
{
    if (!panel) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_lcd_panel_disp_on_off(panel, !sleep);
    if (err != ESP_OK) return err;
    if (LCD_PIN_BL >= 0) {
        err = gpio_set_level(LCD_PIN_BL, sleep ? !LCD_BL_ACTIVE_LEVEL : LCD_BL_ACTIVE_LEVEL);
    }
    return err;
}

esp_err_t lcd_present(void)
{
    for (int y = 0; y < LCD_HEIGHT; y += 16) {
        int rows = LCD_HEIGHT - y < 16 ? LCD_HEIGHT - y : 16;
        for (int i = 0; i < LCD_WIDTH * rows; ++i) {
            uint16_t pixel = frame[y * LCD_WIDTH + i];
            dma[2 * i] = pixel >> 8;
            dma[2 * i + 1] = pixel & 0xff;
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_WIDTH, y + rows, dma);
        if (err != ESP_OK) return err;
        /* Never reuse/free DMA memory before completion. SPI driver owns it. */
        xSemaphoreTake(done, portMAX_DELAY);
    }
    return ESP_OK;
}

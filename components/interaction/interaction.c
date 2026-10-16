#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "interaction.h"
#include "interaction_config.h"
#include "wifi.h"
#include "lcd.h"
#include "ui_logic.h"

#define WHITE 0xffff
#define GREEN 0x07e0
#define YELLOW 0xffe0
static const char *TAG = "interaction";
static QueueHandle_t events;
static atomic_bool input_overflow;
#define UI_EVENT_STATUS_WAKE (-1000)

static uint8_t read_ab(void)
{
    return (gpio_get_level(ENCODER_PIN_A) << 1) | gpio_get_level(ENCODER_PIN_B);
}

static bool read_sw_pressed(void)
{
    return gpio_get_level(ENCODER_PIN_SW) == ENCODER_SW_ACTIVE_LEVEL;
}

/* Runs in esp_timer task, not ISR. No drawing or blocking in this callback. */
static input_state_t input;
static esp_timer_handle_t input_timer;
static void encoder_sample(void *arg)
{
    int rotate = input_rotate(&input, read_ab(), ENCODER_TRANSITIONS_PER_STEP, ENCODER_REVERSE);
    int button = input_button(&input, read_sw_pressed(),
                             (uint32_t)(esp_timer_get_time() / 1000),
                             ENCODER_DEBOUNCE_MS, ENCODER_LONG_PRESS_MS);
    if (rotate && xQueueSend(events, &rotate, 0) != pdTRUE) atomic_store(&input_overflow, true);
    if (button && xQueueSend(events, &button, 0) != pdTRUE) atomic_store(&input_overflow, true);
}

static esp_err_t draw_ui(const ui_state_t *state)
{
    lcd_clear(0);
    lcd_text(12, 14, "INGAWAY", WHITE, 3);
    lcd_rect(12, 45, 216, 2, GREEN);
    if (state->page == UI_MENU) {
        lcd_text(12, 72, "MENU", GREEN, 2);
        lcd_rect(8, state->selected == 0 ? 105 : 151, 224, 34, 0x2104);
        lcd_text(16, 115, "ENCODER TEST", WHITE, 2);
        lcd_text(16, 161, "WIFI SETUP", WHITE, 2);
        lcd_text(12, 245, "ROTATE TO SELECT", WHITE, 2);
        lcd_text(12, 273, "CLICK TO ENTER", WHITE, 2);
    } else if (state->page == UI_INPUT_TEST) {
        char count[24];
        snprintf(count, sizeof(count), "%d", state->count);
        lcd_text(12, 72, "ENCODER TEST", GREEN, 2);
        lcd_text(12, 115, "COUNT", WHITE, 2);
        lcd_text(12, 155, count, YELLOW, 4);
        lcd_text(12, 245, "CLICK TO RESET", WHITE, 2);
        lcd_text(12, 273, "HOLD TO BACK", WHITE, 2);
    } else {
        lcd_text(12, 72, "RESTART WIFI", YELLOW, 2);
        lcd_text(12, 108, "REBOOT TO BLUFI", WHITE, 2);
        lcd_text(12, 136, "NETWORK WILL STOP", WHITE, 2);
        lcd_rect(state->confirm_yes ? 126 : 8, 186, 102, 38, 0x2104);
        lcd_text(20, 198, "CANCEL", WHITE, 2);
        lcd_text(146, 198, "YES", YELLOW, 2);
        lcd_text(12, 245, "CLICK TO CONFIRM", WHITE, 2);
        lcd_text(12, 273, "HOLD TO CANCEL", WHITE, 2);
    }
    return lcd_present();
}

static esp_err_t display_test(void)
{
#if LCD_TEST_ENABLED
    const uint16_t colors[] = {0xf800, GREEN, 0x001f, WHITE, 0};
    for (unsigned i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        lcd_clear(colors[i]);
        esp_err_t err = lcd_present();
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    lcd_rect(0, 0, 240, 1, WHITE); lcd_rect(0, 319, 240, 1, WHITE);
    lcd_rect(0, 0, 1, 320, WHITE); lcd_rect(239, 0, 1, 320, WHITE);
    lcd_text(4, 4, "TL", WHITE, 2); lcd_text(212, 4, "TR", WHITE, 2);
    lcd_text(4, 300, "BL", WHITE, 2); lcd_text(212, 300, "BR", WHITE, 2);
    esp_err_t err = lcd_present();
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(2000));
#endif
    return ESP_OK;
}

static void ui_task(void *arg)
{
    esp_err_t err = lcd_init();
    if (err != ESP_OK) goto fail;
    err = display_test();
    if (err != ESP_OK) goto fail;
    const gpio_config_t ab_pins = {
        .pin_bit_mask = (1ULL << ENCODER_PIN_A) | (1ULL << ENCODER_PIN_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&ab_pins);
    if (err != ESP_OK) goto fail;
    const gpio_config_t sw_pin = {
        .pin_bit_mask = (1ULL << ENCODER_PIN_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = ENCODER_SW_ACTIVE_LEVEL ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = ENCODER_SW_ACTIVE_LEVEL ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&sw_pin);
    if (err != ESP_OK) goto fail;
    input.ab = read_ab();
    ESP_LOGI(TAG, "Encoder pins A=%d B=%d SW=%d active=%d levels: A=%d B=%d SW=%d",
             ENCODER_PIN_A, ENCODER_PIN_B, ENCODER_PIN_SW, ENCODER_SW_ACTIVE_LEVEL,
             gpio_get_level(ENCODER_PIN_A), gpio_get_level(ENCODER_PIN_B),
             gpio_get_level(ENCODER_PIN_SW));
    const esp_timer_create_args_t timer_config = {
        .callback = encoder_sample, .name = "encoder", .skip_unhandled_events = true,
    };
    err = esp_timer_create(&timer_config, &input_timer);
    if (err != ESP_OK) goto fail;
    err = esp_timer_start_periodic(input_timer, 1000);
    if (err != ESP_OK) goto fail;
    ESP_LOGI(TAG, "ST7789 initialized; encoder ready (1 ms sampling)");
    ui_state_t state = {0};
    bool screen_on = true;
    bool dirty = true;
    int64_t last_activity_ms = esp_timer_get_time() / 1000;
    for (;;) {
        if (screen_on && dirty) {
            err = draw_ui(&state);
            if (err != ESP_OK) goto fail;
            dirty = false;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t remain_ms = LCD_IDLE_SLEEP_MS - (now_ms - last_activity_ms);
        TickType_t wait_ticks = screen_on
            ? (remain_ms > 0 ? pdMS_TO_TICKS(remain_ms) : 0)
            : portMAX_DELAY;
        int event;
        if (xQueueReceive(events, &event, wait_ticks) != pdTRUE) {
            if (screen_on) {
                err = lcd_sleep(true);
                if (err != ESP_OK) goto fail;
                screen_on = false;
                dirty = true;
                ESP_LOGI(TAG, "LCD blanked after idle timeout");
            }
            continue;
        }

        if (atomic_exchange(&input_overflow, false)) {
            /* Lost ordering must never turn into an unintended confirmation. */
            xQueueReset(events);
            state = (ui_state_t){0};
            last_activity_ms = esp_timer_get_time() / 1000;
            if (!screen_on) {
                err = lcd_sleep(false);
                if (err != ESP_OK) goto fail;
                screen_on = true;
            }
            dirty = true;
            ESP_LOGW(TAG, "Input queue overflow; returned to menu");
            continue;
        }

        last_activity_ms = esp_timer_get_time() / 1000;
        if (!screen_on) {
            err = lcd_sleep(false);
            if (err != ESP_OK) goto fail;
            screen_on = true;
            dirty = true;
            ESP_LOGI(TAG, "LCD woke by %s", event == UI_EVENT_STATUS_WAKE ? "status" : "touch/input");
            /* First touch/button/encoder event only wakes the screen, avoiding
             * unintended menu actions or confirmations while the LCD is off. */
            if (event != UI_EVENT_STATUS_WAKE) continue;
        }
        if (event == UI_EVENT_STATUS_WAKE) {
            dirty = true;
            continue;
        }

        ESP_LOGI(TAG, "Input %d: page=%d selection=%d count=%d",
                 event, state.page, state.selected, state.count);
        if (ui_event(&state, event)) {
            lcd_clear(0);
            lcd_text(12, 100, "RESTARTING", YELLOW, 3);
            lcd_text(12, 150, "OPEN BLUFI APP", WHITE, 2);
            lcd_present();
            vTaskDelay(pdMS_TO_TICKS(300));
            wifi_reprovision();
            /* Success restarts the chip. Returning means the persistent
             * request could not be written; do not leave RESTARTING on screen. */
            ESP_LOGE(TAG, "Reprovision request failed; returning to menu");
            lcd_clear(0);
            lcd_text(12, 100, "SETUP FAILED", YELLOW, 2);
            lcd_text(12, 140, "NVS WRITE ERROR", WHITE, 2);
            lcd_text(12, 180, "PLEASE RETRY", WHITE, 2);
            err = lcd_present();
            if (err != ESP_OK) goto fail;
            vTaskDelay(pdMS_TO_TICKS(2000));
            xQueueReset(events);
            state = (ui_state_t){0};
        }
        dirty = true;
    }
fail:
    if (input_timer) {
        esp_timer_stop(input_timer);
        esp_timer_delete(input_timer);
        input_timer = NULL;
    }
    ESP_LOGE(TAG, "LCD/UI failed: %s; Wi-Fi continues", esp_err_to_name(err));
    vTaskDelete(NULL);
}

esp_err_t interaction_start(void)
{
    if (events) return ESP_ERR_INVALID_STATE;
    events = xQueueCreate(16, sizeof(int));
    if (!events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(ui_task, "lcd_ui", 4096, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(events);
        events = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t interaction_notify_status_change(void)
{
    if (!events) return ESP_ERR_INVALID_STATE;
    int event = UI_EVENT_STATUS_WAKE;
    return xQueueSend(events, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

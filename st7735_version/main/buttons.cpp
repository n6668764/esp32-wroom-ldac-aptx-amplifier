#include "buttons.h"

#include <stddef.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr char TAG[] = "BUTTONS";
static constexpr TickType_t POLL_TICKS = pdMS_TO_TICKS(10);
static constexpr uint16_t DEBOUNCE_POLLS = 3;
static constexpr uint16_t LONG_PRESS_POLLS = 60;
static constexpr uint16_t REPEAT_POLLS = 16;

struct button_t {
    gpio_num_t gpio;
    button_action_t short_action;
    button_action_t long_action;
    bool has_long_action;
    bool stable_pressed;
    bool sampled_pressed;
    uint16_t debounce_count;
    uint16_t held_polls;
};

static button_t s_buttons[] = {
    {BOARD_BUTTON_PREV, button_action_t::previous, button_action_t::volume_down,
     true, false, false, 0, 0},
    {BOARD_BUTTON_PLAY, button_action_t::play_pause, button_action_t::play_pause,
     false, false, false, 0, 0},
    {BOARD_BUTTON_NEXT, button_action_t::next, button_action_t::volume_up,
     true, false, false, 0, 0},
    {BOARD_BUTTON_REVERB, button_action_t::reverb_next, button_action_t::reverb_next,
     false, false, false, 0, 0},
};
static button_action_callback_t s_callback;

static void button_task(void *)
{
    for (;;) {
        for (button_t &button : s_buttons) {
            const bool pressed = gpio_get_level(button.gpio) == 0;
            if (pressed != button.sampled_pressed) {
                button.sampled_pressed = pressed;
                button.debounce_count = 0;
            } else if (button.debounce_count < DEBOUNCE_POLLS) {
                ++button.debounce_count;
            }

            if (button.debounce_count == DEBOUNCE_POLLS &&
                button.stable_pressed != button.sampled_pressed) {
                const uint16_t held_polls = button.held_polls;
                button.stable_pressed = button.sampled_pressed;
                button.held_polls = 0;
                if (!button.stable_pressed && !button.has_long_action && s_callback != nullptr) {
                    s_callback(button.short_action);
                } else if (!button.stable_pressed && button.has_long_action &&
                           held_polls < LONG_PRESS_POLLS && s_callback != nullptr) {
                    s_callback(button.short_action);
                }
            }

            if (button.stable_pressed && button.has_long_action) {
                ++button.held_polls;
                if (button.held_polls == LONG_PRESS_POLLS ||
                    (button.held_polls > LONG_PRESS_POLLS &&
                     (button.held_polls - LONG_PRESS_POLLS) % REPEAT_POLLS == 0)) {
                    if (s_callback != nullptr) s_callback(button.long_action);
                }
            }
        }
        vTaskDelay(POLL_TICKS);
    }
}

esp_err_t buttons_init(button_action_callback_t callback)
{
    s_callback = callback;
    uint64_t pin_mask = 0;
    for (const button_t &button : s_buttons) pin_mask |= 1ULL << button.gpio;
    gpio_config_t config = {};
    config.pin_bit_mask = pin_mask;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) return result;

    if (xTaskCreatePinnedToCore(button_task, "buttons", 3072, nullptr,
                                tskIDLE_PRIORITY + 2, nullptr, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready: PREV=%d PLAY=%d NEXT=%d REVERB=%d", BOARD_BUTTON_PREV,
             BOARD_BUTTON_PLAY, BOARD_BUTTON_NEXT, BOARD_BUTTON_REVERB);
    return ESP_OK;
}

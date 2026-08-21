#pragma once

#include "esp_err.h"

enum class button_action_t {
    previous,
    play_pause,
    next,
    reverb_next,
    volume_down,
    volume_up,
};

using button_action_callback_t = void (*)(button_action_t action);

esp_err_t buttons_init(button_action_callback_t callback);

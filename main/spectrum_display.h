#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t spectrum_display_init(void);
void spectrum_display_submit_i2s(const int32_t *stereo, size_t frames, uint32_t sample_rate);
void spectrum_display_set_active(bool active);

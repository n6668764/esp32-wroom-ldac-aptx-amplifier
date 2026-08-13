#include "spectrum_display.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static constexpr char TAG[] = "SPECTRUM";
static constexpr gpio_num_t OLED_SDA = GPIO_NUM_21;
static constexpr gpio_num_t OLED_SCL = GPIO_NUM_22;
static constexpr uint8_t OLED_ADDRESS = 0x3C;
static constexpr size_t FFT_SIZE = 256;
static constexpr size_t BAR_COUNT = 27;
static constexpr float PI_F = 3.14159265358979323846f;

struct fft_block_t {
    float samples[FFT_SIZE];
};

static QueueHandle_t s_fft_queue;
static i2c_master_dev_handle_t s_oled;
static volatile bool s_active;
static float s_capture[FFT_SIZE];
static size_t s_capture_pos;

static esp_err_t oled_command(const uint8_t *commands, size_t length)
{
    uint8_t buffer[32];
    if (length + 1 > sizeof(buffer)) return ESP_ERR_INVALID_SIZE;
    buffer[0] = 0x00;
    memcpy(buffer + 1, commands, length);
    return i2c_master_transmit(s_oled, buffer, length + 1, 20);
}

static esp_err_t oled_draw(const uint8_t *framebuffer)
{
    static constexpr uint8_t address[] = {0x21, 0, 127, 0x22, 0, 7};
    esp_err_t result = oled_command(address, sizeof(address));
    if (result != ESP_OK) return result;

    uint8_t line[129];
    line[0] = 0x40;
    for (size_t offset = 0; offset < 1024; offset += 128) {
        memcpy(line + 1, framebuffer + offset, 128);
        result = i2c_master_transmit(s_oled, line, sizeof(line), 20);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

static void set_pixel(uint8_t *framebuffer, int x, int y)
{
    if ((unsigned)x < 128 && (unsigned)y < 64) {
        framebuffer[x + (y >> 3) * 128] |= (uint8_t)(1U << (y & 7));
    }
}

static uint16_t glyph3x5(char c)
{
    switch (c) {
    case 'A': return 0b010101111101101;
    case 'C': return 0b111100100100111;
    case 'F': return 0b111100110100100;
    case 'G': return 0b111100101101111;
    case 'L': return 0b100100100100111;
    case 'O': return 0b111101101101111;
    case 'V': return 0b101101101101010;
    default:  return 0;
    }
}

static void draw_text3x5(uint8_t *framebuffer, int x, int y, const char *text)
{
    while (*text) {
        const uint16_t glyph = glyph3x5(*text++);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                if (glyph & (1U << (14 - row * 3 - col))) {
                    set_pixel(framebuffer, x + col, y + row);
                }
            }
        }
        x += 4;
    }
}

static void fft_task(void *)
{
    float window[FFT_SIZE];
    float twiddle_re[FFT_SIZE / 2];
    float twiddle_im[FFT_SIZE / 2];
    float re[FFT_SIZE];
    float im[FFT_SIZE];
    float displayed[BAR_COUNT] = {};
    uint8_t framebuffer[1024];

    for (size_t i = 0; i < FFT_SIZE; ++i) {
        window[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * i / (FFT_SIZE - 1));
    }
    for (size_t i = 0; i < FFT_SIZE / 2; ++i) {
        const float angle = -2.0f * PI_F * i / FFT_SIZE;
        twiddle_re[i] = cosf(angle);
        twiddle_im[i] = sinf(angle);
    }

    bool was_active = true;
    int64_t last_draw = 0;
    for (;;) {
        fft_block_t block;
        if (xQueueReceive(s_fft_queue, &block, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (!s_active && was_active) {
                memset(framebuffer, 0, sizeof(framebuffer));
                oled_draw(framebuffer);
                memset(displayed, 0, sizeof(displayed));
                was_active = false;
            }
            continue;
        }
        if (!s_active) continue;
        was_active = true;

        for (size_t i = 0; i < FFT_SIZE; ++i) {
            size_t reversed = 0;
            size_t value = i;
            for (unsigned bit = 0; bit < 8; ++bit) {
                reversed = (reversed << 1) | (value & 1U);
                value >>= 1;
            }
            re[reversed] = block.samples[i] * window[i];
            im[reversed] = 0.0f;
        }
        for (size_t length = 2; length <= FFT_SIZE; length <<= 1) {
            const size_t half = length >> 1;
            const size_t step = FFT_SIZE / length;
            for (size_t base = 0; base < FFT_SIZE; base += length) {
                for (size_t j = 0; j < half; ++j) {
                    const size_t k = j * step;
                    const float tr = re[base + j + half] * twiddle_re[k] -
                                     im[base + j + half] * twiddle_im[k];
                    const float ti = re[base + j + half] * twiddle_im[k] +
                                     im[base + j + half] * twiddle_re[k];
                    const float ur = re[base + j];
                    const float ui = im[base + j];
                    re[base + j] = ur + tr;
                    im[base + j] = ui + ti;
                    re[base + j + half] = ur - tr;
                    im[base + j + half] = ui - ti;
                }
            }
        }

        float levels[BAR_COUNT];
        float peak_db = 1.0f;
        float sample_peak = 0.0f;
        for (size_t i = 0; i < FFT_SIZE; ++i) {
            const float amplitude = fabsf(block.samples[i]);
            if (amplitude > sample_peak) sample_peak = amplitude;
        }
        for (size_t bar = 0; bar < BAR_COUNT; ++bar) {
            float power = 1.0f;
            const size_t first_bin = 1 + bar * 2;
            for (size_t bin = first_bin; bin < first_bin + 2; ++bin) {
                power += re[bin] * re[bin] + im[bin] * im[bin];
            }
            levels[bar] = 10.0f * log10f(power);
            if (levels[bar] > peak_db) peak_db = levels[bar];
        }

        // Right-side status panel: instantaneous volume meter and truthful AGC state.
        for (int y = 0; y < 64; ++y) set_pixel(framebuffer, 108, y);
        draw_text3x5(framebuffer, 112, 1, "VOL");
        float volume_db = 20.0f * log10f(sample_peak / 32768.0f + 0.000001f);
        int meter_height = (int)((volume_db + 48.0f) * (29.0f / 48.0f));
        if (meter_height < 0) meter_height = 0;
        if (meter_height > 29) meter_height = 29;
        for (int x = 112; x <= 124; ++x) {
            set_pixel(framebuffer, x, 9);
            set_pixel(framebuffer, x, 39);
        }
        for (int y = 9; y <= 39; ++y) {
            set_pixel(framebuffer, 112, y);
            set_pixel(framebuffer, 124, y);
        }
        for (int x = 114; x <= 122; ++x) {
            for (int y = 38; y > 38 - meter_height; --y) set_pixel(framebuffer, x, y);
        }
        draw_text3x5(framebuffer, 112, 44, "AGC");
        draw_text3x5(framebuffer, 112, 53, "OFF");

        memset(framebuffer, 0, sizeof(framebuffer));
        for (size_t bar = 0; bar < BAR_COUNT; ++bar) {
            float height = (levels[bar] - (peak_db - 42.0f)) * (63.0f / 42.0f);
            if (height < 0.0f) height = 0.0f;
            if (height > 63.0f) height = 63.0f;
            displayed[bar] = height > displayed[bar] ? height : displayed[bar] * 0.78f;
            const int h = (int)displayed[bar];
            for (int x = (int)bar * 4; x < (int)bar * 4 + 3; ++x) {
                for (int y = 63; y >= 64 - h; --y) set_pixel(framebuffer, x, y);
            }
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_draw >= 50000) {
            const esp_err_t result = oled_draw(framebuffer);
            if (result != ESP_OK) ESP_LOGW(TAG, "OLED update failed: %s", esp_err_to_name(result));
            last_draw = now;
        }
    }
}

esp_err_t spectrum_display_init(void)
{
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = OLED_SDA;
    bus.scl_io_num = OLED_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus_handle = nullptr;
    esp_err_t result = i2c_new_master_bus(&bus, &bus_handle);
    if (result != ESP_OK) return result;

    i2c_device_config_t device = {};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = OLED_ADDRESS;
    device.scl_speed_hz = 400000;
    result = i2c_master_bus_add_device(bus_handle, &device, &s_oled);
    if (result != ESP_OK) return result;

    static constexpr uint8_t init[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0x2E, 0xAF,
    };
    result = oled_command(init, sizeof(init));
    if (result != ESP_OK) return result;

    s_fft_queue = xQueueCreate(1, sizeof(fft_block_t));
    if (s_fft_queue == nullptr) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(fft_task, "fft_oled", 8192, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "SSD1306 128x64 ready: SDA=21 SCL=22 address=0x3c");
    return ESP_OK;
}

void spectrum_display_submit_i2s(const int32_t *stereo, size_t frames)
{
    if (!s_active || s_fft_queue == nullptr) return;
    for (size_t i = 0; i < frames; ++i) {
        s_capture[s_capture_pos++] = (float)(stereo[i * 2] >> 16);
        if (s_capture_pos == FFT_SIZE) {
            fft_block_t block;
            memcpy(block.samples, s_capture, sizeof(s_capture));
            xQueueOverwrite(s_fft_queue, &block);
            s_capture_pos = 0;
        }
    }
}

void spectrum_display_set_active(bool active)
{
    s_active = active;
    if (!active && s_fft_queue != nullptr) xQueueReset(s_fft_queue);
}

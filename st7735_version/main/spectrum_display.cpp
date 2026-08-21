#include "spectrum_display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static constexpr char TAG[] = "ST7735";
static constexpr size_t FFT_SIZE = 256;
static constexpr size_t BAR_COUNT = 32;
static constexpr float PI_F = 3.14159265358979323846f;

struct fft_block_t { float samples[FFT_SIZE]; };
enum class codec_t : uint8_t { none, ldac, aptx_hd, aptx, aac, sbc };

static QueueHandle_t s_fft_queue;
static spi_device_handle_t s_tft;
static volatile bool s_active;
static volatile bool s_connected;
static volatile uint8_t s_absolute_volume = 127;
static volatile uint8_t s_reverb_level = 2;
static volatile codec_t s_codec = codec_t::none;
static volatile uint32_t s_sample_rate;
static volatile uint32_t s_state_revision;
static float s_capture[FFT_SIZE];
static size_t s_capture_pos;

static void state_changed(void)
{
    const uint32_t revision = s_state_revision;
    s_state_revision = revision + 1;
}

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t native = (uint16_t)(((r & 0xf8U) << 8) | ((g & 0xfcU) << 3) | (b >> 3));
    return (uint16_t)((native << 8) | (native >> 8));
}

static constexpr uint16_t WHITE = rgb565(255, 255, 255);
static constexpr uint16_t GRAY = rgb565(90, 100, 110);
static constexpr uint16_t GREEN = rgb565(20, 230, 100);
static constexpr uint16_t CYAN = rgb565(30, 205, 255);
static constexpr uint16_t YELLOW = rgb565(255, 205, 30);
static constexpr uint16_t RED = rgb565(255, 55, 55);
static constexpr uint16_t BLUE = rgb565(30, 90, 230);

static esp_err_t tft_write(bool data, const void *buffer, size_t length)
{
    if (length == 0) return ESP_OK;
    gpio_set_level(BOARD_TFT_DC, data ? 1 : 0);
    spi_transaction_t transaction = {};
    transaction.length = length * 8;
    transaction.tx_buffer = buffer;
    return spi_device_polling_transmit(s_tft, &transaction);
}

static esp_err_t tft_command(uint8_t command, const uint8_t *data = nullptr, size_t length = 0)
{
    esp_err_t result = tft_write(false, &command, 1);
    return result == ESP_OK ? tft_write(true, data, length) : result;
}

static esp_err_t tft_draw(const uint16_t *framebuffer)
{
    const uint16_t x0 = BOARD_TFT_X_OFFSET;
    const uint16_t y0 = BOARD_TFT_Y_OFFSET;
    const uint16_t x1 = x0 + BOARD_TFT_WIDTH - 1;
    const uint16_t y1 = y0 + BOARD_TFT_HEIGHT - 1;
    const uint8_t columns[] = {(uint8_t)(x0 >> 8), (uint8_t)x0,
                               (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t rows[] = {(uint8_t)(y0 >> 8), (uint8_t)y0,
                            (uint8_t)(y1 >> 8), (uint8_t)y1};
    esp_err_t result = tft_command(0x2A, columns, sizeof(columns));
    if (result == ESP_OK) result = tft_command(0x2B, rows, sizeof(rows));
    if (result == ESP_OK) result = tft_command(0x2C);
    if (result == ESP_OK) {
        result = tft_write(true, framebuffer,
                           BOARD_TFT_WIDTH * BOARD_TFT_HEIGHT * sizeof(uint16_t));
    }
    return result;
}

static void pixel(uint16_t *fb, int x, int y, uint16_t color)
{
    if ((unsigned)x < BOARD_TFT_WIDTH && (unsigned)y < BOARD_TFT_HEIGHT) {
        fb[y * BOARD_TFT_WIDTH + x] = color;
    }
}

static void fill_rect(uint16_t *fb, int x, int y, int width, int height, uint16_t color)
{
    for (int py = y; py < y + height; ++py) {
        for (int px = x; px < x + width; ++px) pixel(fb, px, py, color);
    }
}

// Five column, seven row glyphs: 0-9 followed by A-Z.
static const uint8_t FONT[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};

static void draw_char(uint16_t *fb, int x, int y, char c, uint16_t color, int scale = 1)
{
    const uint8_t *glyph = nullptr;
    uint8_t special[5] = {};
    if (c >= '0' && c <= '9') glyph = FONT[c - '0'];
    else if (c >= 'A' && c <= 'Z') glyph = FONT[10 + c - 'A'];
    else if (c == '-') { special[0]=special[1]=special[2]=special[3]=special[4]=0x08; glyph=special; }
    else if (c == '/') { special[0]=0x60;special[1]=0x18;special[2]=0x06;glyph=special; }
    else if (c == ':') { special[2]=0x14;glyph=special; }
    else if (c == '%') { special[0]=0x63;special[1]=0x13;special[2]=0x08;special[3]=0x64;special[4]=0x63;glyph=special; }
    if (glyph == nullptr) return;
    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            if (glyph[column] & (1U << row)) {
                fill_rect(fb, x + column * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(uint16_t *fb, int x, int y, const char *text, uint16_t color, int scale = 1)
{
    while (*text != '\0') {
        draw_char(fb, x, y, *text++, color, scale);
        x += 6 * scale;
    }
}

static const char *codec_name(codec_t codec)
{
    switch (codec) {
        case codec_t::ldac: return "LDAC";
        case codec_t::aptx_hd: return "APTX-HD";
        case codec_t::aptx: return "APTX";
        case codec_t::aac: return "AAC";
        case codec_t::sbc: return "SBC";
        default: return "---";
    }
}

static void draw_ui(uint16_t *fb, const float *bars)
{
    memset(fb, 0, BOARD_TFT_WIDTH * BOARD_TFT_HEIGHT * sizeof(uint16_t));
    const bool connected = s_connected;
    const bool active = s_active;
    draw_text(fb, 3, 3, connected ? "BT LINK" : "BT WAIT", connected ? GREEN : YELLOW);
    draw_text(fb, 105, 3, active ? "PLAY" : "PAUSE", active ? GREEN : GRAY);
    draw_text(fb, 3, 14, codec_name(s_codec), CYAN);
    if (s_sample_rate != 0) {
        char rate[16];
        snprintf(rate, sizeof(rate), "%luHZ", (unsigned long)s_sample_rate);
        draw_text(fb, 93, 14, rate, WHITE);
    }

    for (size_t bar = 0; bar < BAR_COUNT; ++bar) {
        int height = (int)bars[bar];
        if (height < 0) height = 0;
        if (height > 76) height = 76;
        const int x = (int)bar * 5 + 1;
        for (int y = 0; y < height; ++y) {
            const uint8_t red = y > 60 ? 255 : (uint8_t)(25 + y * 2);
            const uint8_t green = y > 60 ? (uint8_t)(220 - (y - 60) * 10) : 220;
            fill_rect(fb, x, 103 - y, 4, 1, rgb565(red, green, 55));
        }
    }

    fill_rect(fb, 2, 109, 156, 8, GRAY);
    const unsigned percent = ((unsigned)s_absolute_volume * 100U + 63U) / 127U;
    const int volume_width = (int)(152U * percent / 100U);
    fill_rect(fb, 4, 111, volume_width, 4, percent == 0 ? RED : BLUE);
    char volume[8];
    snprintf(volume, sizeof(volume), "V%u%%", percent);
    draw_text(fb, 3, 120, volume, WHITE);
    char controls[20];
    snprintf(controls, sizeof(controls), "PRV PLY NXT RV%u", (unsigned)s_reverb_level);
    draw_text(fb, 58, 120, controls, GRAY);
}

static void fft_task(void *)
{
    float window[FFT_SIZE], twiddle_re[FFT_SIZE / 2], twiddle_im[FFT_SIZE / 2];
    float re[FFT_SIZE], im[FFT_SIZE], displayed[BAR_COUNT] = {};
    uint16_t *framebuffer = static_cast<uint16_t *>(heap_caps_malloc(
        BOARD_TFT_WIDTH * BOARD_TFT_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA));
    if (framebuffer == nullptr) {
        ESP_LOGE(TAG, "framebuffer allocation failed");
        vTaskDelete(nullptr);
        return;
    }
    for (size_t i = 0; i < FFT_SIZE; ++i) {
        window[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * i / (FFT_SIZE - 1));
    }
    for (size_t i = 0; i < FFT_SIZE / 2; ++i) {
        const float angle = -2.0f * PI_F * i / FFT_SIZE;
        twiddle_re[i] = cosf(angle); twiddle_im[i] = sinf(angle);
    }

    uint32_t drawn_revision = UINT32_MAX;
    int64_t last_draw = 0;
    for (;;) {
        fft_block_t block;
        const bool got_block = xQueueReceive(s_fft_queue, &block, pdMS_TO_TICKS(100)) == pdTRUE;
        if (got_block && s_active) {
            for (size_t i = 0; i < FFT_SIZE; ++i) {
                size_t reversed = 0, value = i;
                for (unsigned bit = 0; bit < 8; ++bit) {
                    reversed = (reversed << 1) | (value & 1U); value >>= 1;
                }
                re[reversed] = block.samples[i] * window[i]; im[reversed] = 0.0f;
            }
            for (size_t length = 2; length <= FFT_SIZE; length <<= 1) {
                const size_t half = length >> 1, step = FFT_SIZE / length;
                for (size_t base = 0; base < FFT_SIZE; base += length) {
                    for (size_t j = 0; j < half; ++j) {
                        const size_t k = j * step;
                        const float tr = re[base+j+half]*twiddle_re[k] - im[base+j+half]*twiddle_im[k];
                        const float ti = re[base+j+half]*twiddle_im[k] + im[base+j+half]*twiddle_re[k];
                        const float ur = re[base+j], ui = im[base+j];
                        re[base+j] = ur + tr; im[base+j] = ui + ti;
                        re[base+j+half] = ur - tr; im[base+j+half] = ui - ti;
                    }
                }
            }
            float peak_db = 1.0f, levels[BAR_COUNT];
            for (size_t bar = 0; bar < BAR_COUNT; ++bar) {
                float power = 1.0f;
                const size_t first = 1 + bar * 2;
                for (size_t bin = first; bin < first + 2; ++bin) power += re[bin]*re[bin] + im[bin]*im[bin];
                levels[bar] = 10.0f * log10f(power);
                if (levels[bar] > peak_db) peak_db = levels[bar];
            }
            for (size_t bar = 0; bar < BAR_COUNT; ++bar) {
                float height = (levels[bar] - (peak_db - 42.0f)) * (76.0f / 42.0f);
                if (height < 0) height = 0;
                if (height > 76) height = 76;
                displayed[bar] = height > displayed[bar] ? height : displayed[bar] * 0.78f;
            }
        } else if (!s_active) {
            for (float &bar : displayed) bar *= 0.65f;
        }

        const int64_t now = esp_timer_get_time();
        const uint32_t revision = s_state_revision;
        if ((got_block || revision != drawn_revision || !s_active) && now - last_draw >= 50000) {
            draw_ui(framebuffer, displayed);
            const esp_err_t result = tft_draw(framebuffer);
            if (result != ESP_OK) ESP_LOGW(TAG, "update failed: %s", esp_err_to_name(result));
            drawn_revision = revision; last_draw = now;
        }
    }
}

esp_err_t spectrum_display_init(void)
{
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = (1ULL << BOARD_TFT_DC) | (1ULL << BOARD_TFT_RST) | (1ULL << BOARD_TFT_BL);
    outputs.mode = GPIO_MODE_OUTPUT;
    esp_err_t result = gpio_config(&outputs);
    if (result != ESP_OK) return result;
    gpio_set_level(BOARD_TFT_BL, BOARD_TFT_BL_ACTIVE_HIGH ? 0 : 1);
    gpio_set_level(BOARD_TFT_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_TFT_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus = {};
    bus.mosi_io_num = BOARD_TFT_MOSI; bus.miso_io_num = -1; bus.sclk_io_num = BOARD_TFT_SCLK;
    bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
    bus.max_transfer_sz = BOARD_TFT_WIDTH * BOARD_TFT_HEIGHT * sizeof(uint16_t);
    result = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) return result;
    spi_device_interface_config_t device = {};
    device.clock_speed_hz = BOARD_TFT_SPI_HZ; device.mode = 0;
    device.spics_io_num = BOARD_TFT_CS; device.queue_size = 1;
    result = spi_bus_add_device(SPI2_HOST, &device, &s_tft);
    if (result != ESP_OK) return result;

    result = tft_command(0x01); vTaskDelay(pdMS_TO_TICKS(150));
    if (result == ESP_OK) result = tft_command(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t frame[] = {0x01,0x2C,0x2D}, frame3[] = {0x01,0x2C,0x2D,0x01,0x2C,0x2D};
    const uint8_t inv[] = {0x07}, power1[] = {0xA2,0x02,0x84}, power2[] = {0xC5};
    const uint8_t power3[] = {0x0A,0x00}, power4[] = {0x8A,0x2A}, power5[] = {0x8A,0xEE};
    const uint8_t vcom[] = {0x0E}, format[] = {0x05};
    const uint8_t madctl[] = {(uint8_t)(0x60 | (BOARD_TFT_BGR ? 0x08 : 0x00))};
    if (result == ESP_OK) result = tft_command(0xB1, frame, sizeof(frame));
    if (result == ESP_OK) result = tft_command(0xB2, frame, sizeof(frame));
    if (result == ESP_OK) result = tft_command(0xB3, frame3, sizeof(frame3));
    if (result == ESP_OK) result = tft_command(0xB4, inv, sizeof(inv));
    if (result == ESP_OK) result = tft_command(0xC0, power1, sizeof(power1));
    if (result == ESP_OK) result = tft_command(0xC1, power2, sizeof(power2));
    if (result == ESP_OK) result = tft_command(0xC2, power3, sizeof(power3));
    if (result == ESP_OK) result = tft_command(0xC3, power4, sizeof(power4));
    if (result == ESP_OK) result = tft_command(0xC4, power5, sizeof(power5));
    if (result == ESP_OK) result = tft_command(0xC5, vcom, sizeof(vcom));
    if (result == ESP_OK) result = tft_command(0x20);
    if (result == ESP_OK) result = tft_command(0x3A, format, sizeof(format));
    if (result == ESP_OK) result = tft_command(0x36, madctl, sizeof(madctl));
    if (result == ESP_OK) result = tft_command(0x13);
    if (result == ESP_OK) result = tft_command(0x29);
    if (result != ESP_OK) return result;
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_TFT_BL, BOARD_TFT_BL_ACTIVE_HIGH ? 1 : 0);

    s_fft_queue = xQueueCreate(1, sizeof(fft_block_t));
    if (s_fft_queue == nullptr) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(fft_task, "fft_st7735", 8192, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr, 0) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready: %dx%d SCLK=%d MOSI=%d CS=%d DC=%d RST=%d", BOARD_TFT_WIDTH,
             BOARD_TFT_HEIGHT, BOARD_TFT_SCLK, BOARD_TFT_MOSI, BOARD_TFT_CS, BOARD_TFT_DC,
             BOARD_TFT_RST);
    return ESP_OK;
}

void spectrum_display_submit_i2s(const int32_t *stereo, size_t frames)
{
    if (!s_active || s_fft_queue == nullptr) return;
    for (size_t i = 0; i < frames; ++i) {
        s_capture[s_capture_pos++] = (float)(stereo[i * 2] >> 16);
        if (s_capture_pos == FFT_SIZE) {
            fft_block_t block; memcpy(block.samples, s_capture, sizeof(s_capture));
            xQueueOverwrite(s_fft_queue, &block); s_capture_pos = 0;
        }
    }
}

void spectrum_display_set_active(bool active)
{
    s_active = active;
    state_changed();
    if (!active && s_fft_queue != nullptr) xQueueReset(s_fft_queue);
}

void spectrum_display_set_volume(uint8_t volume)
{
    s_absolute_volume = volume > 127 ? 127 : volume;
    state_changed();
}

void spectrum_display_set_reverb(uint8_t level)
{
    s_reverb_level = level > 3 ? 3 : level;
    state_changed();
}

void spectrum_display_set_connected(bool connected)
{
    s_connected = connected;
    state_changed();
}

void spectrum_display_set_codec(const char *codec, uint32_t sample_rate)
{
    if (codec == nullptr) s_codec = codec_t::none;
    else if (strcmp(codec, "LDAC") == 0) s_codec = codec_t::ldac;
    else if (strcmp(codec, "aptX HD") == 0) s_codec = codec_t::aptx_hd;
    else if (strcmp(codec, "aptX Classic") == 0) s_codec = codec_t::aptx;
    else if (strcmp(codec, "AAC") == 0) s_codec = codec_t::aac;
    else if (strcmp(codec, "SBC") == 0) s_codec = codec_t::sbc;
    else s_codec = codec_t::none;
    s_sample_rate = sample_rate;
    state_changed();
}

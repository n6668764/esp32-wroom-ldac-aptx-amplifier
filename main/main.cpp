#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_reg.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_sbc_dec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freeaptx.h"
#include "ldacdec.h"
#include "nvs_flash.h"
#include "spectrum_display.h"

// Pure Bluetooth amplifier build: A2DP -> decoder -> PCM5102.
// Music-fountain FFT, OLED and pump PWM control are intentionally omitted.
static constexpr char TAG[] = "BT_AMP";
static constexpr char DEVICE_NAME[] = "ESP32 aptX Amplifier";
static constexpr char FIRMWARE_REV[] = "ldac-volume-gain-v16";

static constexpr gpio_num_t I2S_BCK = GPIO_NUM_26;
static constexpr gpio_num_t I2S_WS = GPIO_NUM_25;
static constexpr gpio_num_t I2S_DOUT = GPIO_NUM_23;
static constexpr uint32_t I2S_RATE = 44100;
static constexpr uint32_t I2S_RATE_48K = 48000;
static constexpr uint32_t I2S_RATE_88K = 88200;
static constexpr uint32_t I2S_RATE_96K = 96000;
static constexpr bool MONO_OUTPUT = true;

static constexpr uint32_t APTX_VENDOR_ID = 0x0000004FUL;
static constexpr uint16_t APTX_CODEC_ID = 0x0001U;
static constexpr uint32_t APTX_HD_VENDOR_ID = 0x000000D7UL;
static constexpr uint16_t APTX_HD_CODEC_ID = 0x0024U;
static constexpr uint8_t APTX_RATE_44100 = 0x20;
static constexpr uint8_t APTX_RATE_48000 = 0x10;
static constexpr uint8_t APTX_STEREO = 0x02;
static constexpr uint32_t LDAC_VENDOR_ID = 0x0000012DUL;
static constexpr uint16_t LDAC_CODEC_ID = 0x00AAU;
static constexpr uint8_t LDAC_RATE_44100 = 0x20;
static constexpr uint8_t LDAC_RATE_48000 = 0x10;
static constexpr uint8_t LDAC_RATE_88200 = 0x08;
static constexpr uint8_t LDAC_RATE_96000 = 0x04;
static constexpr uint8_t LDAC_STEREO = 0x01;

static constexpr UBaseType_t RAW_QUEUE_DEPTH = 32;
static constexpr uint32_t DECODER_STACK = 8192;
static constexpr UBaseType_t DECODER_PRIORITY = tskIDLE_PRIORITY + 3;
static constexpr BaseType_t DECODER_CORE = 1;

enum decoder_kind_t {
    DECODER_NONE,
    DECODER_ESP_AUDIO,
    DECODER_APTX,
    DECODER_LDAC,
};

static QueueHandle_t s_raw_queue;
static SemaphoreHandle_t s_decoder_mutex;
static SemaphoreHandle_t s_i2s_mutex;
static TaskHandle_t s_decoder_task;
static i2s_chan_handle_t s_i2s_tx;
static esp_audio_dec_handle_t s_esp_decoder;
static aptx_context *s_aptx_decoder;
static ldacdec_t *s_ldac_decoder;
static decoder_kind_t s_decoder_kind = DECODER_NONE;
static volatile bool s_connected;
static bool s_decoder_open;
static bool s_aptx_hd;
static uint32_t s_sample_rate = I2S_RATE;
static uint8_t s_channels = 2;
static uint32_t s_decoder_generation;
static volatile uint8_t s_absolute_volume = 127;
static volatile int32_t s_volume_gain_q15 = 32768;

static esp_err_t set_i2s_rate(uint32_t sample_rate)
{
    if (s_i2s_tx == nullptr || s_sample_rate == sample_rate) {
        s_sample_rate = sample_rate;
        return ESP_OK;
    }

    xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    esp_err_t result = i2s_channel_disable(s_i2s_tx);
    if (result != ESP_OK) {
        xSemaphoreGive(s_i2s_mutex);
        return result;
    }
    i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    result = i2s_channel_reconfig_std_clock(s_i2s_tx, &clock);
    const esp_err_t enable_result = i2s_channel_enable(s_i2s_tx);
    if (result == ESP_OK) {
        result = enable_result;
    }
    if (result == ESP_OK) {
        s_sample_rate = sample_rate;
    }
    xSemaphoreGive(s_i2s_mutex);
    return result;
}

static bool aptx_mcc_variant(const esp_a2d_mcc_t *mcc, bool *hd)
{
    if (mcc == nullptr || mcc->type != ESP_A2D_MCT_NON_A2DP) {
        return false;
    }

    uint8_t cie[11];
    memcpy(cie, &mcc->cie, sizeof(cie));
    const uint32_t vendor = (uint32_t)cie[0] | ((uint32_t)cie[1] << 8) |
                            ((uint32_t)cie[2] << 16) | ((uint32_t)cie[3] << 24);
    const uint16_t codec = (uint16_t)cie[4] | ((uint16_t)cie[5] << 8);
    const bool classic = vendor == APTX_VENDOR_ID && codec == APTX_CODEC_ID;
    const bool high_definition = vendor == APTX_HD_VENDOR_ID && codec == APTX_HD_CODEC_ID;
    if ((!classic && !high_definition) ||
        !(cie[6] & (APTX_RATE_44100 | APTX_RATE_48000)) ||
        !(cie[6] & APTX_STEREO)) {
        return false;
    }
    if (hd != nullptr) {
        *hd = high_definition;
    }
    return true;
}

static bool ldac_mcc(const esp_a2d_mcc_t *mcc, uint32_t *sample_rate)
{
    if (mcc == nullptr || mcc->type != ESP_A2D_MCT_NON_A2DP) return false;
    uint8_t cie[11] = {};
    memcpy(cie, &mcc->cie, sizeof(cie));
    const uint32_t vendor = (uint32_t)cie[0] | ((uint32_t)cie[1] << 8) |
                            ((uint32_t)cie[2] << 16) | ((uint32_t)cie[3] << 24);
    const uint16_t codec = (uint16_t)cie[4] | ((uint16_t)cie[5] << 8);
    if (vendor != LDAC_VENDOR_ID || codec != LDAC_CODEC_ID ||
        !(cie[6] & (LDAC_RATE_44100 | LDAC_RATE_48000 |
                    LDAC_RATE_88200 | LDAC_RATE_96000)) || !(cie[7] & LDAC_STEREO)) {
        return false;
    }
    if (sample_rate != nullptr) {
        *sample_rate = (cie[6] & LDAC_RATE_96000) ? I2S_RATE_96K :
                       (cie[6] & LDAC_RATE_88200) ? I2S_RATE_88K :
                       (cie[6] & LDAC_RATE_48000) ? I2S_RATE_48K : I2S_RATE;
    }
    return true;
}

static void drain_raw_queue(void)
{
    if (s_raw_queue == nullptr) {
        return;
    }
    esp_a2d_audio_buff_t *buffer;
    while (xQueueReceive(s_raw_queue, &buffer, 0) == pdTRUE) {
        esp_a2d_audio_buff_free(buffer);
    }
}

static void decoder_close_locked(void)
{
    if (s_decoder_kind == DECODER_APTX && s_aptx_decoder != nullptr) {
        aptx_finish(s_aptx_decoder);
    } else if (s_decoder_kind == DECODER_LDAC && s_ldac_decoder != nullptr) {
        free(s_ldac_decoder);
    } else if (s_decoder_kind == DECODER_ESP_AUDIO && s_esp_decoder != nullptr) {
        esp_audio_dec_close(s_esp_decoder);
    }
    s_aptx_decoder = nullptr;
    s_ldac_decoder = nullptr;
    s_esp_decoder = nullptr;
    s_decoder_kind = DECODER_NONE;
    s_decoder_open = false;
    s_aptx_hd = false;
    ++s_decoder_generation;
}

static esp_audio_err_t decoder_open(const esp_a2d_mcc_t *mcc)
{
    if (mcc == nullptr) {
        return ESP_AUDIO_ERR_INVALID_PARAMETER;
    }

    drain_raw_queue();
    xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
    decoder_close_locked();

    esp_audio_err_t result = ESP_AUDIO_ERR_FAIL;
    bool aptx_hd = false;
    uint32_t ldac_rate = 0;
    if (ldac_mcc(mcc, &ldac_rate)) {
        s_ldac_decoder = static_cast<ldacdec_t *>(calloc(1, sizeof(ldacdec_t)));
        if (s_ldac_decoder != nullptr && ldacdecInit(s_ldac_decoder) == 0 &&
            set_i2s_rate(ldac_rate) == ESP_OK) {
            s_decoder_kind = DECODER_LDAC;
            s_decoder_open = true;
            s_channels = 2;
            result = ESP_AUDIO_ERR_OK;
            ESP_LOGI(TAG, "codec=LDAC, %u Hz, stereo, PCM=16-bit (experimental)",
                     (unsigned)s_sample_rate);
        } else {
            free(s_ldac_decoder);
            s_ldac_decoder = nullptr;
            result = ESP_AUDIO_ERR_MEM_LACK;
        }
    } else if (aptx_mcc_variant(mcc, &aptx_hd)) {
        uint8_t cie[11];
        memcpy(cie, &mcc->cie, sizeof(cie));
        const uint32_t aptx_rate = (cie[6] & APTX_RATE_48000) ? I2S_RATE_48K : I2S_RATE;
        s_aptx_decoder = aptx_init(aptx_hd ? 1 : 0);
        if (s_aptx_decoder != nullptr && set_i2s_rate(aptx_rate) == ESP_OK) {
            s_decoder_kind = DECODER_APTX;
            s_decoder_open = true;
            s_aptx_hd = aptx_hd;
            s_channels = 2;
            result = ESP_AUDIO_ERR_OK;
            ESP_LOGI(TAG, "codec=%s, %u Hz, stereo, PCM=%u-bit",
                     aptx_hd ? "aptX HD" : "aptX Classic", (unsigned)s_sample_rate,
                     aptx_hd ? 24U : 16U);
        } else {
            if (s_aptx_decoder != nullptr) {
                aptx_finish(s_aptx_decoder);
                s_aptx_decoder = nullptr;
            }
            result = ESP_AUDIO_ERR_MEM_LACK;
        }
    } else if (mcc->type == ESP_A2D_MCT_SBC) {
        esp_sbc_dec_cfg_t config = ESP_SBC_DEC_CONFIG_DEFAULT();
        config.sbc_mode = ESP_SBC_MODE_STD;
        config.ch_num = (mcc->cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) ? 1 : 2;
        config.enable_plc = true;
        esp_sbc_dec_register();
        esp_audio_dec_cfg_t decoder_config = {
            .type = ESP_AUDIO_TYPE_SBC,
            .cfg = &config,
            .cfg_sz = sizeof(config),
        };
        result = esp_audio_dec_open(&decoder_config, &s_esp_decoder);
        if (result == ESP_AUDIO_ERR_OK) {
            if (set_i2s_rate(I2S_RATE) == ESP_OK) {
                s_decoder_kind = DECODER_ESP_AUDIO;
                s_decoder_open = true;
                s_channels = config.ch_num;
                ESP_LOGI(TAG, "codec=SBC, channels=%u", (unsigned)s_channels);
            } else {
                esp_audio_dec_close(s_esp_decoder);
                s_esp_decoder = nullptr;
                result = ESP_AUDIO_ERR_FAIL;
            }
        }
    } else if (mcc->type == ESP_A2D_MCT_M24) {
        const esp_a2d_cie_m24_t *m24 = &mcc->cie.m24_info;
        esp_aac_dec_cfg_t config = ESP_AAC_DEC_CONFIG_DEFAULT();
        config.sample_rate = I2S_RATE;
        config.channel = (m24->ch & ESP_A2D_M24_CIE_CH_1) ? 1 : 2;
        config.bits_per_sample = ESP_AUDIO_BIT16;
        config.no_adts_header = true;
        config.aac_plus_enable =
            (m24->obj_type & (ESP_A2D_M24_CIE_OBJ_TYPE_4_HE_AAC |
                              ESP_A2D_M24_CIE_OBJ_TYPE_4_HE_AAC_V2)) != 0;
        esp_aac_dec_register();
        esp_audio_dec_cfg_t decoder_config = {
            .type = ESP_AUDIO_TYPE_AAC,
            .cfg = &config,
            .cfg_sz = sizeof(config),
        };
        result = esp_audio_dec_open(&decoder_config, &s_esp_decoder);
        if (result == ESP_AUDIO_ERR_OK) {
            if (set_i2s_rate(config.sample_rate) == ESP_OK) {
                s_decoder_kind = DECODER_ESP_AUDIO;
                s_decoder_open = true;
                s_channels = config.channel;
                ESP_LOGI(TAG, "codec=AAC, channels=%u", (unsigned)s_channels);
            } else {
                esp_audio_dec_close(s_esp_decoder);
                s_esp_decoder = nullptr;
                result = ESP_AUDIO_ERR_FAIL;
            }
        }
    }

    if (result != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "failed to open codec type 0x%02x: %d", mcc->type, result);
    }
    xSemaphoreGive(s_decoder_mutex);
    return result;
}

static void decoder_close(void)
{
    drain_raw_queue();
    xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
    decoder_close_locked();
    xSemaphoreGive(s_decoder_mutex);
}

static void silence_i2s(void)
{
    if (s_i2s_tx == nullptr || s_i2s_mutex == nullptr) return;
    static const uint8_t zeros[512] = {};
    xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    if (i2s_channel_disable(s_i2s_tx) == ESP_OK) {
        for (;;) {
            size_t loaded = 0;
            if (i2s_channel_preload_data(s_i2s_tx, zeros, sizeof(zeros), &loaded) != ESP_OK ||
                loaded < sizeof(zeros)) {
                break;
            }
        }
        i2s_channel_enable(s_i2s_tx);
    }
    xSemaphoreGive(s_i2s_mutex);
}

static void write_pcm(uint8_t *data, size_t size, uint8_t channels, uint8_t bits)
{
    if (size == 0) {
        return;
    }

    uint8_t *i2s_data = data;
    size_t i2s_size = size;
    int32_t *converted = nullptr;
    if (bits == 16) {
        const int16_t *input = reinterpret_cast<const int16_t *>(data);
        const size_t samples = size / sizeof(int16_t);
        const size_t frames = samples / channels;
        converted = static_cast<int32_t *>(malloc(frames * 2 * sizeof(int32_t)));
        if (converted == nullptr) return;
        for (size_t i = 0; i < frames; ++i) {
            const int32_t left = (int32_t)input[i * channels] << 16;
            const int32_t right = channels == 1 ? left : (int32_t)input[i * channels + 1] << 16;
            const int32_t sample = MONO_OUTPUT ?
                (int32_t)(((int64_t)left + right) / 2) : left;
            converted[2 * i] = sample;
            converted[2 * i + 1] = MONO_OUTPUT ? sample : right;
        }
        i2s_data = reinterpret_cast<uint8_t *>(converted);
        i2s_size = frames * 2 * sizeof(int32_t);
    } else if (MONO_OUTPUT && channels == 2) {
        int32_t *pcm = reinterpret_cast<int32_t *>(data);
        const size_t frames = size / (2 * sizeof(int32_t));
        for (size_t i = 0; i < frames; ++i) {
            const int32_t mono = (int32_t)(((int64_t)pcm[2 * i] + pcm[2 * i + 1]) / 2);
            pcm[2 * i] = mono;
            pcm[2 * i + 1] = mono;
        }
    }

    // With AVRCP absolute volume the phone sends full-scale PCM and delegates
    // attenuation to the sink. Use a cheap perceptual curve: V0 is mute,
    // V100 is bit-exact, and the lower phone steps remain comfortably quiet.
    int32_t *pcm = reinterpret_cast<int32_t *>(i2s_data);
    const size_t pcm_samples = i2s_size / sizeof(int32_t);
    const int32_t gain_q15 = s_volume_gain_q15;
    if (gain_q15 != 32768) {
        for (size_t i = 0; i < pcm_samples; ++i) {
            pcm[i] = (int32_t)(((int64_t)pcm[i] * gain_q15) >> 15);
        }
    }

    spectrum_display_submit_i2s(reinterpret_cast<const int32_t *>(i2s_data),
                                i2s_size / (2 * sizeof(int32_t)));

    xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    size_t offset = 0;
    while (offset < i2s_size) {
        size_t written = 0;
        const esp_err_t error = i2s_channel_write(s_i2s_tx, i2s_data + offset, i2s_size - offset,
                                                   &written, portMAX_DELAY);
        if (error != ESP_OK || written == 0) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(error));
            break;
        }
        offset += written;
    }
    xSemaphoreGive(s_i2s_mutex);
    free(converted);
}

static void decoder_task(void *)
{
    size_t output_capacity = 4096;
    uint8_t *output = static_cast<uint8_t *>(malloc(output_capacity));
    uint8_t aptx_carry[5] = {};
    size_t aptx_carry_size = 0;
    uint32_t local_generation = 0;
    if (output == nullptr) {
        ESP_LOGE(TAG, "decoder output allocation failed");
        vTaskDelete(nullptr);
    }

    for (;;) {
        esp_a2d_audio_buff_t *packet = nullptr;
        xQueueReceive(s_raw_queue, &packet, portMAX_DELAY);
        if (packet == nullptr) {
            continue;
        }

        xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
        if (!s_connected || !s_decoder_open) {
            xSemaphoreGive(s_decoder_mutex);
            esp_a2d_audio_buff_free(packet);
            continue;
        }

        if (local_generation != s_decoder_generation) {
            aptx_carry_size = 0;
            local_generation = s_decoder_generation;
        }

        if (s_decoder_kind == DECODER_LDAC) {
            size_t offset = 0;
            while (offset < packet->data_len && packet->data[offset] == 0xAA) {
                int bytes_used = 0;
                const int64_t started = esp_timer_get_time();
                const int decode_result = ldacDecode(s_ldac_decoder, packet->data + offset,
                                                      reinterpret_cast<int16_t *>(output),
                                                      &bytes_used);
                const uint32_t decode_us = (uint32_t)(esp_timer_get_time() - started);
                if (decode_result != 0 || bytes_used <= 0 ||
                    offset + (size_t)bytes_used > packet->data_len) {
                    ESP_LOGW(TAG, "LDAC sync loss at %u/%u, result=%d used=%d",
                             (unsigned)offset, (unsigned)packet->data_len,
                             decode_result, bytes_used);
                    break;
                }
                const int decoded_rate = ldacdecGetSampleRate(s_ldac_decoder);
                if ((decoded_rate == 44100 || decoded_rate == 48000 ||
                     decoded_rate == 88200 || decoded_rate == 96000) &&
                    (uint32_t)decoded_rate != s_sample_rate) {
                    const uint32_t negotiated_rate = s_sample_rate;
                    const esp_err_t rate_result = set_i2s_rate((uint32_t)decoded_rate);
                    if (rate_result == ESP_OK) {
                        ESP_LOGW(TAG, "LDAC rate corrected from negotiated %u to frame %u Hz",
                                 (unsigned)negotiated_rate, (unsigned)decoded_rate);
                    } else {
                        ESP_LOGE(TAG, "LDAC I2S rate correction failed: %s",
                                 esp_err_to_name(rate_result));
                    }
                }
                const int channels = ldacdecGetChannelCount(s_ldac_decoder);
                const int samples = s_ldac_decoder->frame.frameSamples;
                const uint32_t audio_us = (uint32_t)((uint64_t)samples * 1000000ULL /
                                                     s_sample_rate);
                static int64_t last_ldac_overrun_log = 0;
                if (decode_us > audio_us && started - last_ldac_overrun_log > 1000000) {
                    ESP_LOGW(TAG, "LDAC overrun decode=%u us audio=%u us queue=%u",
                             (unsigned)decode_us, (unsigned)audio_us,
                             (unsigned)uxQueueMessagesWaiting(s_raw_queue));
                    last_ldac_overrun_log = started;
                }
                offset += (size_t)bytes_used;
                xSemaphoreGive(s_decoder_mutex);
                write_pcm(output, (size_t)samples * channels * sizeof(int16_t),
                          (uint8_t)channels, 16);
                xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
                if (!s_decoder_open || s_decoder_kind != DECODER_LDAC) break;
            }
            xSemaphoreGive(s_decoder_mutex);
        } else if (s_decoder_kind == DECODER_APTX) {
            const bool aptx_hd = s_aptx_hd;
            const size_t codeword_size = aptx_hd ? 6 : 4;
            const size_t combined_size = aptx_carry_size + packet->data_len;
            uint8_t *combined = static_cast<uint8_t *>(malloc(combined_size));
            if (combined != nullptr) {
                memcpy(combined, aptx_carry, aptx_carry_size);
                memcpy(combined + aptx_carry_size, packet->data, packet->data_len);
                const size_t decodable = combined_size - combined_size % codeword_size;
                aptx_carry_size = combined_size - decodable;
                if (aptx_carry_size) {
                    memcpy(aptx_carry, combined + decodable, aptx_carry_size);
                }

                const size_t needed = (decodable / codeword_size) * (aptx_hd ? 32 : 16);
                if (needed > output_capacity) {
                    uint8_t *larger = static_cast<uint8_t *>(realloc(output, needed));
                    if (larger != nullptr) {
                        output = larger;
                        output_capacity = needed;
                    }
                }

                if (decodable && needed <= output_capacity) {
                    const int64_t started = esp_timer_get_time();
                    size_t produced = 0;
                    const size_t consumed = aptx_hd ?
                        aptx_decode32(s_aptx_decoder, combined, decodable, output,
                                      output_capacity, &produced) :
                        aptx_decode16(s_aptx_decoder, combined, decodable, output,
                                      output_capacity, &produced);
                    const uint32_t audio_us = (uint32_t)(((uint64_t)(consumed / codeword_size) * 4 * 1000000ULL) /
                                                         s_sample_rate);
                    const uint32_t decode_us = (uint32_t)(esp_timer_get_time() - started);
                    static int64_t last_overrun_log = 0;
                    if (consumed != decodable) {
                        ESP_LOGW(TAG, "aptX sync loss: consumed=%u/%u", (unsigned)consumed,
                                 (unsigned)decodable);
                        aptx_reset(s_aptx_decoder);
                        aptx_carry_size = 0;
                    }
                    if (audio_us && decode_us > audio_us && started - last_overrun_log > 1000000) {
                        ESP_LOGW(TAG, "aptX overrun decode=%u us audio=%u us queue=%u",
                                 (unsigned)decode_us, (unsigned)audio_us,
                                 (unsigned)uxQueueMessagesWaiting(s_raw_queue));
                        last_overrun_log = started;
                    }
                    xSemaphoreGive(s_decoder_mutex);
                    write_pcm(output, produced, 2, aptx_hd ? 32 : 16);
                } else {
                    xSemaphoreGive(s_decoder_mutex);
                }
                free(combined);
            } else {
                xSemaphoreGive(s_decoder_mutex);
            }
        } else {
            esp_audio_dec_in_raw_t input = {
                .buffer = packet->data,
                .len = packet->data_len,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };
            while (input.len > 0) {
                esp_audio_dec_out_frame_t frame = {
                    .buffer = output,
                    .len = (uint32_t)output_capacity,
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                esp_audio_err_t result = esp_audio_dec_process(s_esp_decoder, &input, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > output_capacity) {
                    uint8_t *larger = static_cast<uint8_t *>(realloc(output, frame.needed_size));
                    if (larger == nullptr) {
                        break;
                    }
                    output = larger;
                    output_capacity = frame.needed_size;
                    continue;
                }
                if (result != ESP_AUDIO_ERR_OK || input.consumed == 0) {
                    break;
                }
                input.buffer += input.consumed;
                input.len -= input.consumed;
                const uint8_t channels = s_channels;
                xSemaphoreGive(s_decoder_mutex);
                write_pcm(output, frame.decoded_size, channels, 16);
                xSemaphoreTake(s_decoder_mutex, portMAX_DELAY);
                if (!s_decoder_open || s_decoder_kind != DECODER_ESP_AUDIO) {
                    break;
                }
            }
            xSemaphoreGive(s_decoder_mutex);
        }

        esp_a2d_audio_buff_free(packet);
    }
}

static void audio_data_callback(esp_a2d_conn_hdl_t, esp_a2d_audio_buff_t *buffer)
{
    if (!s_connected || s_raw_queue == nullptr ||
        xQueueSend(s_raw_queue, &buffer, 0) != pdTRUE) {
        esp_a2d_audio_buff_free(buffer);
    }
}

static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (event == ESP_A2D_PROF_STATE_EVT &&
        param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
        esp_a2d_mcc_t ldac = {};
        ldac.type = ESP_A2D_MCT_NON_A2DP;
        const uint8_t ldac_cie[8] = {0x2D, 0x01, 0, 0, 0xAA, 0,
                                     LDAC_RATE_44100 | LDAC_RATE_48000 |
                                     LDAC_RATE_88200 | LDAC_RATE_96000, LDAC_STEREO};
        memcpy(&ldac.cie, ldac_cie, sizeof(ldac_cie));
        ESP_ERROR_CHECK(esp_a2d_sink_register_stream_endpoint(0, &ldac));

        esp_a2d_mcc_t aptx = {};
        aptx.type = ESP_A2D_MCT_NON_A2DP;
        // aptX HD uses an 11-byte vendor CIE: the Classic fields followed by
        // four mandatory ACL-sprint reserved octets.
        const uint8_t aptx_hd_cie[11] = {0xD7, 0, 0, 0, 0x24, 0,
                                         APTX_RATE_44100 | APTX_RATE_48000 | APTX_STEREO,
                                         0, 0, 0, 0};
        memcpy(&aptx.cie, aptx_hd_cie, sizeof(aptx_hd_cie));
        ESP_ERROR_CHECK(esp_a2d_sink_register_stream_endpoint(1, &aptx));

        const uint8_t aptx_cie[7] = {0x4F, 0, 0, 0, 0x01, 0,
                                     APTX_RATE_44100 | APTX_RATE_48000 | APTX_STEREO};
        memcpy(&aptx.cie, aptx_cie, sizeof(aptx_cie));
        ESP_ERROR_CHECK(esp_a2d_sink_register_stream_endpoint(2, &aptx));

        esp_a2d_mcc_t aac = {};
        aac.type = ESP_A2D_MCT_M24;
        aac.cie.m24_info.drc = ESP_A2D_M24_CIE_DRC_NS;
        aac.cie.m24_info.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC |
                                    ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC;
        aac.cie.m24_info.samp_freq1 = ESP_A2D_M24_CIE_SF1_44K;
        aac.cie.m24_info.ch = ESP_A2D_M24_CIE_CH_2;
        aac.cie.m24_info.br1 = 0x7F;
        aac.cie.m24_info.vbr = ESP_A2D_M24_CIE_VBR_SUPPORT;
        aac.cie.m24_info.br2 = 0xFF;
        aac.cie.m24_info.br3 = 0xFF;
        ESP_ERROR_CHECK(esp_a2d_sink_register_stream_endpoint(3, &aac));

        esp_a2d_mcc_t sbc = {};
        sbc.type = ESP_A2D_MCT_SBC;
        sbc.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_STEREO |
                                   ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
        sbc.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_44K;
        sbc.cie.sbc_info.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR |
                                     ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
        sbc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 |
                                        ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
        sbc.cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_4 |
                                     ESP_A2D_SBC_CIE_BLOCK_LEN_8 |
                                     ESP_A2D_SBC_CIE_BLOCK_LEN_12 |
                                     ESP_A2D_SBC_CIE_BLOCK_LEN_16;
        sbc.cie.sbc_info.min_bitpool = 2;
        sbc.cie.sbc_info.max_bitpool = 250;
        ESP_ERROR_CHECK(esp_a2d_sink_register_stream_endpoint(4, &sbc));
        esp_a2d_sink_get_delay_value();
    } else if (event == ESP_A2D_SEP_REG_STATE_EVT) {
        static const char *const names[] = {"LDAC", "aptX HD", "aptX Classic", "AAC", "SBC"};
        const uint8_t seid = param->a2d_sep_reg_stat.seid;
        ESP_LOGI(TAG, "endpoint seid=%u codec=%s registration=%s (%d)",
                 (unsigned)seid, seid < 5 ? names[seid] : "unknown",
                 param->a2d_sep_reg_stat.reg_state == ESP_A2D_SEP_REG_SUCCESS ? "OK" : "FAILED",
                 (int)param->a2d_sep_reg_stat.reg_state);
    } else if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = true;
            ESP_LOGI(TAG, "Bluetooth connected");
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_connected = false;
            decoder_close();
            spectrum_display_set_active(false);
            silence_i2s();
            ESP_LOGI(TAG, "Bluetooth disconnected");
        }
    } else if (event == ESP_A2D_AUDIO_STATE_EVT) {
        const bool playing = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        spectrum_display_set_active(playing);
        if (!playing) silence_i2s();
    } else if (event == ESP_A2D_AUDIO_CFG_EVT) {
        decoder_open(&param->audio_cfg.mcc);
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_CFM_REQ_EVT) {
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
    } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4,
                             reinterpret_cast<uint8_t *>(const_cast<char *>("1234")));
    } else if (event == ESP_BT_GAP_AUTH_CMPL_EVT) {
        ESP_LOGI(TAG, "authentication status=%d", param->auth_cmpl.stat);
    }
}

static void avrc_target_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    if (event == ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT) {
        const uint8_t volume = param->set_abs_vol.volume & 0x7f;
        s_absolute_volume = volume;
        s_volume_gain_q15 = volume == 127 ? 32768 :
            (int32_t)(((uint32_t)volume * volume * 32768U) / (127U * 127U));
        spectrum_display_set_volume(volume);
        ESP_LOGI(TAG, "absolute volume=%u%% (%u/127)",
                 (unsigned)volume * 100U / 127U, (unsigned)volume);
    } else if (event == ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT &&
               param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        esp_avrc_rn_param_t response = {};
        response.volume = s_absolute_volume;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
                                ESP_AVRC_RN_RSP_INTERIM, &response);
    }
}

static void init_i2s(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 8;
    channel.dma_frame_num = 240;
    channel.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, &s_i2s_tx, nullptr));

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &config));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
}

static void init_bluetooth(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bt_sleep_disable());

    esp_bluedroid_config_t bluedroid = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_callback));

    esp_bt_io_cap_t io = ESP_BT_IO_CAP_IO;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io, sizeof(io)));
    esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(DEVICE_NAME));

    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(avrc_target_callback));
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    esp_avrc_rn_evt_cap_mask_t volume_events = {};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &volume_events,
                                       ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&volume_events));

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_callback));
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    ESP_ERROR_CHECK(esp_a2d_sink_register_audio_data_callback(audio_data_callback));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                              ESP_BT_GENERAL_DISCOVERABLE));
}

extern "C" void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    s_decoder_mutex = xSemaphoreCreateMutex();
    s_i2s_mutex = xSemaphoreCreateMutex();
    s_raw_queue = xQueueCreate(RAW_QUEUE_DEPTH, sizeof(esp_a2d_audio_buff_t *));
    if (s_decoder_mutex == nullptr || s_i2s_mutex == nullptr || s_raw_queue == nullptr) {
        ESP_LOGE(TAG, "queue/mutex allocation failed");
        return;
    }

    init_i2s();
    const esp_err_t display_result = spectrum_display_init();
    if (display_result != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 unavailable: %s; audio will continue", esp_err_to_name(display_result));
    }
    xTaskCreatePinnedToCore(decoder_task, "a2dp_decoder", DECODER_STACK, nullptr,
                            DECODER_PRIORITY, &s_decoder_task, DECODER_CORE);
    init_bluetooth();
    ESP_LOGI(TAG, "firmware=%s", FIRMWARE_REV);
    ESP_LOGI(TAG, "ready: LDAC/aptX HD/Classic/AAC/SBC -> PCM5102 + SSD1306 FFT");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

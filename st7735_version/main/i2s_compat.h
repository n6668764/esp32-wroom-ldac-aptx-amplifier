// Compatibility header: defines legacy i2s_config_t and related types
// for the ESP32-A2DP component to use with newer ESP-IDF versions
// where the legacy I2S driver has been removed.
// These are type definitions only - actual I2S functionality is handled
// by the component's AudioTools mode or the new I2S driver.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- I2S mode flags ----
typedef enum {
    I2S_MODE_MASTER = 1,
    I2S_MODE_SLAVE = 2,
    I2S_MODE_TX = 4,
    I2S_MODE_RX = 8,
    I2S_MODE_DAC_BUILT_IN = 16,
    I2S_MODE_ADC_BUILT_IN = 32,
    I2S_MODE_PDM = 64,
} i2s_mode_t;

// ---- Bits per sample ----
typedef enum {
    I2S_BITS_PER_SAMPLE_8BIT = 8,
    I2S_BITS_PER_SAMPLE_16BIT = 16,
    I2S_BITS_PER_SAMPLE_24BIT = 24,
    I2S_BITS_PER_SAMPLE_32BIT = 32,
} i2s_bits_per_sample_t;

// ---- Channel format ----
typedef enum {
    I2S_CHANNEL_FMT_RIGHT_LEFT = 0,
    I2S_CHANNEL_FMT_ALL_RIGHT,
    I2S_CHANNEL_FMT_ALL_LEFT,
    I2S_CHANNEL_FMT_ONLY_RIGHT,
    I2S_CHANNEL_FMT_ONLY_LEFT,
} i2s_channel_fmt_t;

// ---- Communication format ----
typedef enum {
    I2S_COMM_FORMAT_STAND_I2S = 0,
    I2S_COMM_FORMAT_STAND_MSB = 1,
    I2S_COMM_FORMAT_STAND_PCM_SHORT = 2,
    I2S_COMM_FORMAT_STAND_PCM_LONG = 3,
    I2S_COMM_FORMAT_STAND_MAX,
    I2S_COMM_FORMAT_I2S = I2S_COMM_FORMAT_STAND_I2S,
    I2S_COMM_FORMAT_I2S_MSB = I2S_COMM_FORMAT_STAND_I2S,
    I2S_COMM_FORMAT_I2S_LSB = I2S_COMM_FORMAT_STAND_I2S,
} i2s_comm_format_t;

// ---- I2S port ----
typedef int i2s_port_t;
#define I2S_NUM_0 0
#define I2S_NUM_1 1

// ---- I2S configuration struct (legacy) ----
typedef struct {
    i2s_mode_t mode;
    int sample_rate;
    i2s_bits_per_sample_t bits_per_sample;
    i2s_channel_fmt_t channel_format;
    i2s_comm_format_t communication_format;
    int intr_alloc_flags;
    int dma_buf_count;
    int dma_buf_len;
    bool use_apll;
    bool tx_desc_auto_clear;
    int fixed_mclk;
} i2s_config_t;

// ---- I2S pin configuration (legacy) ----
typedef struct {
    int mck_io_num;
    int bck_io_num;
    int ws_io_num;
    int data_out_num;
    int data_in_num;
} i2s_pin_config_t;

#ifdef __cplusplus
}
#endif

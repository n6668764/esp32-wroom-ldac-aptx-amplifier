#pragma once

#include "driver/gpio.h"

// PCM5102 I2S
static constexpr gpio_num_t BOARD_I2S_BCK = GPIO_NUM_26;
static constexpr gpio_num_t BOARD_I2S_WS = GPIO_NUM_25;
static constexpr gpio_num_t BOARD_I2S_DOUT = GPIO_NUM_23;

// ST7735 SPI (write-only). Change these values to match your module.
static constexpr gpio_num_t BOARD_TFT_SCLK = GPIO_NUM_18;
static constexpr gpio_num_t BOARD_TFT_MOSI = GPIO_NUM_19;
static constexpr gpio_num_t BOARD_TFT_CS = GPIO_NUM_5;
static constexpr gpio_num_t BOARD_TFT_DC = GPIO_NUM_16;
static constexpr gpio_num_t BOARD_TFT_RST = GPIO_NUM_17;
static constexpr gpio_num_t BOARD_TFT_BL = GPIO_NUM_4;
static constexpr bool BOARD_TFT_BL_ACTIVE_HIGH = true;

static constexpr int BOARD_TFT_WIDTH = 160;
static constexpr int BOARD_TFT_HEIGHT = 128;
static constexpr uint8_t BOARD_TFT_X_OFFSET = 0;
static constexpr uint8_t BOARD_TFT_Y_OFFSET = 0;
static constexpr bool BOARD_TFT_BGR = true;
static constexpr uint32_t BOARD_TFT_SPI_HZ = 26000000;

// Buttons are active-low: connect each GPIO to GND when pressed.
static constexpr gpio_num_t BOARD_BUTTON_PREV = GPIO_NUM_32;
static constexpr gpio_num_t BOARD_BUTTON_PLAY = GPIO_NUM_33;
static constexpr gpio_num_t BOARD_BUTTON_NEXT = GPIO_NUM_27;
static constexpr gpio_num_t BOARD_BUTTON_REVERB = GPIO_NUM_14;

idf_component_register(
    SRCS "spectrum_display.cpp" "buttons.cpp" "speaker_spatializer.cpp" "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt esp_audio_codec freeaptx libldacdec freertos esp_timer esp_driver_i2s esp_driver_spi esp_driver_gpio nvs_flash
)

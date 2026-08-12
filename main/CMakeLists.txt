idf_component_register(
    SRCS "main.cpp" "spectrum_display.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt esp_audio_codec freeaptx libldacdec freertos esp_timer esp_driver_i2s esp_driver_i2c nvs_flash
)

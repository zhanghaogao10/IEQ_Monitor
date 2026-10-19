idf_component_register(
    SRCS "sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer io_expander adc_bus esp_driver_uart nvs_flash
)

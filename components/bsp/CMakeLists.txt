idf_component_register(
    SRCS "bsp_lcd.c" "bsp_io.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_spi io_expander
)

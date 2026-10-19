file(GLOB UI_GENERATED_SRCS
    "../UI/generated/*.c"
    "../UI/generated/setup_scr_*.c"
    "../UI/generated/guider_fonts/*.c"
    "../UI/generated/images/*.c"
)

idf_component_register(
    SRCS
        "main.c"
        "app_data.c"
        "app_alarm.c"
        "app_buzzer.c"
        "app_key.c"
        "app_power.c"
        "app_storage.c"
        "app_ui.c"
        "../UI/custom/custom.c"
        ${UI_GENERATED_SRCS}
    INCLUDE_DIRS
        "."
        "../UI/custom"
        "../UI/generated"
        "../UI/generated/guider_fonts"
        "../UI/generated/guider_customer_fonts"
        "../UI/generated/images"
    PRIV_REQUIRES
        spi_flash
        nvs_flash
        driver
        adc_bus
        esp_adc
        esp_timer
        lvgl
        bsp
        sensor
        io_expander
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LV_DCLOCK_TEXT_SELECTION=0
    LV_USE_ANALOGCLOCK=1
)

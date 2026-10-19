# IEQ Monitor

ESP32-C3-MINI-1U based indoor environment monitor with SPI LCD, ADC keys,
I2C IO expander, sensor sampling, alarm threshold storage, and UI modules.

## Project Layout

```text
IEQ_Monitor/
├── CMakeLists.txt
├── sdkconfig
├── main/
│   ├── main.c
│   ├── app_config.h
│   ├── app_data.c
│   ├── app_alarm.c
│   ├── app_key.c
│   ├── app_storage.c
│   └── app_ui.c
└── components/
    ├── bsp/
    ├── sensor/
    └── io_expander/
```

## Hardware Notes

- LCD control: `LCD_CS`, `LCD_SDA`, `LCD_SCL`, `LCD_RS`, `LCD_RESET`, `LED_CTRL`
- LCD reset is driven by the TPT29554A IO expander.
- IO expander uses I2C address `0x20`.
- Alarm output currently uses the buzzer enable net.

## Build

Load the ESP-IDF environment first, then run:

```bash
idf.py set-target esp32c3
idf.py build
```

## Version Control

This workspace provides a read-only `.git` placeholder. Use the project Git
wrapper for version control commands:

```bash
./git-project status
./git-project add main components UI SETTINGS_PAGE_LOGIC.md
./git-project commit -m "Describe the change"
```

The wrapper stores Git metadata in `.git-data/`. Build output and downloaded
managed components are ignored by `.gitignore`.

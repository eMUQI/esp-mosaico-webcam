idf_component_register(SRCS "main.c" "camera_pipeline.c" "preview_ui.c" "webcam_display.c" "ui_assets.c" "ui_font_14.c" "ui_font_20.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_update esp_driver_jpeg esp_driver_ppa esp_mm esp_timer nvs_flash espressif__tinyusb espressif__esp_lcd_touch)

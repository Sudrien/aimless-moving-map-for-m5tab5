idf_component_register(
    SRCS "aimless.c" "gnss.c" "ffread.c" "maptile.c" "mapset.c" "mapview.c"
    INCLUDE_DIRS "."
    REQUIRES mapcore feckless_drivers feckless_graphics_handler feckless_storage_handler
             esp_driver_uart esp_driver_gpio esp_timer fatfs esp_app_format
)

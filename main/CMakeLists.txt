# cJSON, for M5Launcher's config.conf: IDF's own `json` on 5.x, the
# registry's espressif/cjson on 6.x, which moved it out of the tree
# (idf_component.yml, behind an idf_version rule). Defeatist's 6031.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(AIMLESS_JSON cjson)
else()
    set(AIMLESS_JSON json)
endif()

idf_component_register(
    SRCS "aimless.c" "gnss.c" "ffread.c" "maptile.c" "mapset.c" "mapview.c" "tilecache.c" "tilesrc.c" "netremote.c"
         "portal.c" "portalweb.c" "dnsreply.c" "wifijoin.c" "launcher_import.c" "launcherkey.c"
         "sun.c" "uirow.c" "waypoints.c"
    INCLUDE_DIRS "."
    REQUIRES mapcore feckless_drivers feckless_graphics_handler feckless_storage_handler feckless_network_handler
             esp_driver_uart esp_driver_gpio esp_timer fatfs esp_app_format esp_http_client mbedtls nvs_flash
             esp_http_server lwip app_update spi_flash ${AIMLESS_JSON}
)

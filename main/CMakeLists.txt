# cJSON, for M5Launcher's config.conf: IDF's own `json` on 5.x, the
# registry's espressif/cjson on 6.x, which moved it out of the tree
# (idf_component.yml, behind an idf_version rule). Defeatist's 6031.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(AIMLESS_JSON cjson)
else()
    set(AIMLESS_JSON json)
endif()

# The world at boot (0020): tile z0/0/0 of the build the network serves,
# fetched once into the build directory and embedded. Not in the
# repository -- it is OpenStreetMap-derived (ODbL), and CLAUDE.md keeps
# that out. A build without network embeds an empty file and boots to
# black, as before. `idf.py fullclean` (or deleting the file and its
# .tried) fetches it again.
set(AIMLESS_WORLD "${CMAKE_BINARY_DIR}/world_z0.mvt.gz")
if(CMAKE_BUILD_EARLY_EXPANSION)
    # The component list is still being worked out and the config is not
    # loaded; EMBED_FILES only needs the file to exist.
    if(NOT EXISTS "${AIMLESS_WORLD}")
        file(WRITE "${AIMLESS_WORLD}" "")
    endif()
elseif(NOT EXISTS "${AIMLESS_WORLD}.tried")
    idf_build_get_property(_aimless_py PYTHON)
    execute_process(
        COMMAND "${_aimless_py}" -I "${CMAKE_CURRENT_LIST_DIR}/../tools/fetch_worldtile.py"
                "${AIMLESS_WORLD}" --base "${CONFIG_AIMLESS_TILE_BASE}"
                --pinned "${CONFIG_AIMLESS_PINNED_BUILD}"
        OUTPUT_VARIABLE _aimless_out ERROR_VARIABLE _aimless_err)
    message(STATUS "${_aimless_err}${_aimless_out}")
    file(SIZE "${AIMLESS_WORLD}" _aimless_size)
    # src: main/maptile.c TILE_CAP, 192 KB: a larger tile cannot be drawn.
    if(_aimless_size GREATER 196608)
        message(WARNING "worldtile: ${_aimless_size} bytes is more than the render "
                        "scratch's 192 KB; the firmware will log that it would not draw")
    endif()
    file(WRITE "${AIMLESS_WORLD}.tried" "")
endif()

idf_component_register(
    SRCS "aimless.c" "gnss.c" "ffread.c" "maptile.c" "mapset.c" "mapview.c" "tilecache.c" "tilesrc.c" "netremote.c"
         "portal.c" "portalweb.c" "dnsreply.c" "wifijoin.c" "launcher_import.c" "launcherkey.c"
         "sun.c" "uirow.c" "waypoints.c" "worldtile.c" "lastfix.c"
    INCLUDE_DIRS "."
    EMBED_FILES "${AIMLESS_WORLD}"
    REQUIRES mapcore feckless_drivers feckless_graphics_handler feckless_storage_handler feckless_network_handler
             esp_driver_uart esp_driver_gpio esp_timer fatfs esp_app_format esp_http_client mbedtls nvs_flash
             esp_http_server lwip app_update spi_flash ${AIMLESS_JSON}
)

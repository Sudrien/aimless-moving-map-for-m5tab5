# mapcore: the original project's portable C -- PMTiles, gzip, MVT, the
# rasteriser, the Protomaps style, the tile grid -- unchanged from
# original/, which is where to compare it. None of it includes anything
# from ESP-IDF; test/ builds it on a host.
#
# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "src/inflate.c" "src/mvt.c" "src/pmtiles.c" "src/raster.c"
         "src/style.c" "src/tile_grid.c"
    INCLUDE_DIRS "include"
)

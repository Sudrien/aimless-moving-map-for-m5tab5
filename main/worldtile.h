/*
 * worldtile.h -- the whole world, tile z0/0/0, drawn at boot by the map's
 * own renderer (0020).
 *
 * The original had a world backdrop for its boot screen, so the screen
 * was a picture from the first second rather than black until the card,
 * the archive and the network had all come up (original/worldmap.h). Its
 * was a Natural Earth outline compiled in as coordinates and drawn by
 * code of its own. This one is the map's: tile z0/0/0 of the same
 * Protomaps build the network serves, fetched when the firmware is built
 * (tools/fetch_worldtile.py), embedded in the image, and drawn by
 * maprender_payload() in the map's own style -- so the boot screen and
 * the map are one picture at two scales.
 *
 * Not in the repository: the tile is OpenStreetMap-derived, under the
 * ODbL, and CLAUDE.md keeps that out. A build that could not fetch it
 * embeds nothing, and the boot screen is black as before.
 *
 * Free of ESP-IDF; tested on the host against the fixture's tiles.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "maptile.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Draw a gzip MVT tile's `len` bytes as z0/0/0 into `px`, r->size
 * square, in the current style. TILE_NODATA for len 0 (nothing was
 * embedded), TILE_ERROR if it does not fit the scratch or will not draw.
 */
tile_state_t worldtile_draw(maprender_t *r, const uint8_t *gz, size_t len, uint16_t *px);

/*
 * Put the drawn world, `size` square, into a w x h window: scaled to
 * cover, as the original's worldmap_draw() -- a square of side max(w, h)
 * centred, the shorter axis cropped rather than letterboxed. At 1280 x 720
 * from a 1280 tile that is its middle 720 rows, which loses the poles:
 * Mercator stretches them into nothing anybody recognises.
 */
void worldtile_compose(const uint16_t *px, int size, uint16_t *fb, int w, int h, int stride);

#ifdef __cplusplus
}
#endif

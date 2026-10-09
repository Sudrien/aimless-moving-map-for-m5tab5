/*
 * maplabel.h -- names on the map: collected while a tile is drawn, laid
 * out over the whole window when the screen is composed.
 *
 * original/mapengine.cpp's label collector (MapLabel, LabelSet,
 * label_add(), rl_part()) and the layout half of its draw_labels(),
 * with no drawing in it, so the layout runs on the host. aimless.c
 * draws what this places, with feckless-graphics' ark12.
 *
 * Collected, not baked into the tile, for the original's reasons: a
 * name straddling the seam between two tiles would be cut by whichever
 * drew it, collision culling has to see the whole screen rather than one
 * tile, and turning labels off has to be a repaint, not a re-render.
 *
 * A set belongs to a tile's pixel buffer, not to a grid slot (mapview.h):
 * a shift hands buffers between slots, and the names have to go with
 * the pixels they describe.
 *
 * Free of ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/mapengine.cpp LABEL_TEXT_MAX: bytes, with the NUL. */
#define MAPLABEL_TEXT_MAX   (40)
/* src: original/mapengine.cpp LABELS_PER_TILE. A dense city tile has a
 * few hundred POIs; culling drops most, so keeping them buys nothing. */
#define MAPLABEL_PER_TILE   (48)
/* src: original/mapengine.cpp LABEL_MAX_ON_SCREEN. */
#define MAPLABEL_ON_SCREEN  (40)

typedef struct {
    float   fx, fy;                     /* within the tile, 0..1 */
    uint8_t style;                      /* S_POI or an S_PLACE_* rank */
    char    text[MAPLABEL_TEXT_MAX];
} maplabel_t;

typedef struct {
    uint16_t   n;
    maplabel_t v[MAPLABEL_PER_TILE];
} maplabel_set_t;

void maplabel_reset(maplabel_set_t *s);

/*
 * Keep a named point at (fx, fy) in the tile. Nothing for an empty name,
 * a point outside [0, 1) -- it is a neighbouring tile's to label, as
 * rl_part() has it -- or a full set. A name too long is cut at a UTF-8
 * character boundary, where label_add() cut at a byte and could leave
 * half a character for the font to draw as garbage. True if kept.
 */
bool maplabel_add(maplabel_set_t *s, float fx, float fy, uint8_t style,
                  const char *text, uint32_t len);

/* src: original/mapengine.cpp label_rank(): country 0, region 1,
 * locality 2, neighbourhood 3, POI 4. Lower is placed first, so a city
 * wins its pixels over a petrol station. */
#define MAPLABEL_RANKS      (5)
int  maplabel_rank(uint8_t style);

/*
 * The text's integer scale of ark12. The original used FreeSans at
 * 9 pt (neighbourhoods), 12 pt (POIs, bold for localities) and bold
 * 18 pt (regions, countries). ark12 is 12 px and only scales whole:
 * 2 is the smallest that is not below 9 or 12 pt on this panel, and 3
 * stands in for the bold the original used to make towns read over
 * POIs. src: chosen against original/mapengine.cpp label_font().
 */
int  maplabel_scale(uint8_t style);

/* src: original/mapengine.cpp draw_poi_dot()'s outer radius, 6 px: a
 * POI's text sits above its dot, and the dot is part of its box. */
#define MAPLABEL_DOT_R      (6)

/* One tile's labels and where the tile's top-left corner is in the
 * window. */
typedef struct {
    const maplabel_set_t *set;
    int x, y;
} maplabel_src_t;

/* A label that survived: copied, so it can be drawn after the lock that
 * guards the sets is let go. */
typedef struct {
    int     ax, ay;                     /* the point, in the window */
    int     tx, ty;                     /* the text's top-left */
    int     tw;                         /* its width */
    int     bx, by, bw, bh;             /* the box it reserved */
    uint8_t style;
    uint8_t scale;
    char    text[MAPLABEL_TEXT_MAX];
} maplabel_placed_t;

/* Width of `s` in pixels at `scale`: gfx_text_w() on the device. */
typedef int (*maplabel_width_fn)(const char *s, int scale);

/*
 * Lay out the labels of `n` tiles, each `tile_px` square, over a window
 * `w` wide whose map band is rows [top, bot). `glyph_h` is the font's
 * height at scale 1. As draw_labels(): rank by rank, a label whose box
 * hits one already placed is dropped, and so is one whose box is off the
 * window. Unlike it, a box must be wholly inside the band: the original
 * clipped to the band, and gfx has no clip, so half a name under the
 * status bar would be overdrawn there and stand out under the button
 * row's gaps. Returns how many were placed into `out`, at most `max`.
 */
int  maplabel_layout(const maplabel_src_t *src, int n, int tile_px,
                     int w, int top, int bot, int glyph_h,
                     maplabel_width_fn width,
                     maplabel_placed_t *out, int max);

#ifdef __cplusplus
}
#endif

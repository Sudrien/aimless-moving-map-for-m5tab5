/*
 * uirow.h -- where the controls are on the screen, and what a touch hit.
 *
 * The original's footer row, settings panel, pan squares and wake zone
 * (original/tab5_map.cpp buttonRect(), buttonAt(), setPanelRect(),
 * setPanelTouch(), inWakeZone() and handleTouch()'s pan block), as
 * geometry with no drawing in it, so the hit tests run on the host. The
 * screen is passed in (sw x sh, landscape) rather than read from the
 * panel.
 *
 * FIVE BUTTONS. The original's row is home, saved points, area cache,
 * settings and screen off, in that order -- by how often each is reached
 * for while moving, with screen off last because it is the one press
 * that is annoying to make by accident. Saved points arrived in 0017 and
 * the area cache in 0026; until then their slots were left empty rather
 * than the row closing up.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int x, y, w, h; } ui_rect_t;

/* src: original/tab5_map.cpp's enum, in its order. */
enum { UI_BTN_HOME = 0, UI_BTN_PINS, UI_BTN_CACHE, UI_BTN_SET, UI_BTN_SLEEP, UI_BTN_COUNT };

/* src: original/tab5_map.cpp BTN_H, BTN_M: 54 px tall, 12 px apart. */
#define UI_BTN_H        (54)
#define UI_BTN_M        (12)
/* src: original/tab5_map.cpp BTN_PAD_TOP, BTN_PAD_SIDE: the touch target
 * reaches 26 px up into the map and 6 px either side, and down to the
 * screen edge, because 54 px is a small thing to hit on a moving vehicle. */
#define UI_BTN_PAD_TOP  (26)
#define UI_BTN_PAD_SIDE (6)

/* Whether a slot has a button in it yet. */
bool ui_button_present(int i);

/* Button i's drawn rectangle. */
void ui_button_rect(int i, int sw, int sh, ui_rect_t *r);

/* The present button under (px, py), padded as above; -1 for none. */
int  ui_button_at(int px, int py, int sw, int sh);

/* Where the map is not under the row: the top of the row's touch zone. */
int  ui_map_bottom(int sh);

/*
 * The pan squares. The map between `top` (under the status bar) and
 * ui_map_bottom() is three by three; a tap in an outer square steps the
 * view that way, -1, 0 or +1 a side. The middle square is a no-op on
 * purpose: it is where the marker is, and the wake zone. False for a
 * point outside the map band, so the caller tries the buttons next.
 */
bool ui_pan_cell(int px, int py, int sw, int sh, int top, int *dx, int *dy);

/* src: original/tab5_map.cpp inWakeZone(): the middle ninth, so a brush
 * against a bag cannot light the screen. */
bool ui_wake_zone(int px, int py, int sw, int sh);

/* ---- the settings panel ---- */

/* The original's rows, less the compass's two, labels and Wi-Fi location,
 * which come with their features. */
enum { UI_SET_THEME = 0, UI_SET_BRIGHT, UI_SET_WIFI, UI_SET_COUNT };

/* src: original/tab5_map.cpp SP_ROW_H, SP_HEAD_H, SP_FOOT_H. */
#define UI_SP_ROW_H     (62)
#define UI_SP_HEAD_H    (70)
#define UI_SP_FOOT_H    (62)
/* The close button in the footer. src: original drawSetPanel(): 150 x 44
 * at 14 in; its touch reaches 170 across, as setPanelTouch(). */
#define UI_SP_CLOSE_W   (170)

/* src: original setPanelRect(): three quarters of the width, centred,
 * no taller than the screen less 40. */
void ui_panel_rect(int sw, int sh, ui_rect_t *r);

typedef enum {
    UI_PANEL_OUTSIDE = -3,  /* closes it */
    UI_PANEL_CLOSE   = -2,  /* the close button: closes it */
    UI_PANEL_NOTHING = -1,  /* the heading or the footer's empty part */
} ui_panel_hit_t;

/* A row's index, or one of the above. */
int  ui_panel_at(int px, int py, int sw, int sh);

/* ---- the settings themselves ---- */

/* src: original/tab5_map.cpp ThemeMode and BrightMode, and setRowTap()'s
 * cycles: auto, day, night; auto, low, medium, high. Back to auto rather
 * than stopping at the end, because a manual setting corrects for
 * something the sun cannot see, and that something passes. Neither is
 * kept across a restart, as there: an override that survives a reboot is
 * one nobody remembers setting. */
/* ---- the saved points panel (0017) ---- */

/* src: original/tab5_map.cpp PP_ROW_H, PP_HEAD_H, PP_FOOT_H, and
 * pinPanelRect(): two thirds of the screen each way, centred. */
#define UI_PP_ROW_H     (64)
#define UI_PP_HEAD_H    (70)
#define UI_PP_FOOT_H    (62)

void ui_pins_rect(int sw, int sh, ui_rect_t *r);

/* Rows on one page. */
int  ui_pins_rows(int sw, int sh);

typedef enum {
    UI_PINS_NOTHING = 0,
    UI_PINS_OUTSIDE,    /* closes it */
    UI_PINS_CLOSE,
    UI_PINS_SAVE,       /* "save here", the heading's right-hand end */
    UI_PINS_STOP,       /* "stop guiding", in the footer */
    UI_PINS_UP,
    UI_PINS_DOWN,
    UI_PINS_PICK,       /* a row: guide to it, or stop if it is the target */
    UI_PINS_DELETE,     /* a row's right-hand end */
} ui_pins_hit_t;

/*
 * What a tap on the panel hits, as the original's pinPanelTouch(). Rows
 * are page rows; *row is the one hit, for PICK and DELETE. The caller
 * adds its scroll offset and checks the row has a point in it.
 */
ui_pins_hit_t ui_pins_at(int px, int py, int sw, int sh, int *row);

typedef enum { UI_THEME_AUTO = 0, UI_THEME_DAY, UI_THEME_NIGHT, UI_THEME_COUNT } ui_theme_t;
typedef enum { UI_BRIGHT_AUTO = 0, UI_BRIGHT_LOW, UI_BRIGHT_MED, UI_BRIGHT_HIGH, UI_BRIGHT_COUNT } ui_bright_t;

static inline ui_theme_t  ui_theme_next(ui_theme_t t)   { return (ui_theme_t)((t + 1) % UI_THEME_COUNT); }
static inline ui_bright_t ui_bright_next(ui_bright_t b) { return (ui_bright_t)((b + 1) % UI_BRIGHT_COUNT); }

#ifdef __cplusplus
}
#endif

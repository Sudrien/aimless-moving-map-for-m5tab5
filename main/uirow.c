/*
 * uirow.c -- see uirow.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "uirow.h"

bool ui_button_present(int i)
{
    return i == UI_BTN_HOME || i == UI_BTN_PINS || i == UI_BTN_SET || i == UI_BTN_SLEEP;
}

void ui_button_rect(int i, int sw, int sh, ui_rect_t *r)
{
    /* src: original buttonRect(): the whole width in five, 12 px between
     * and at either end. */
    const int total = sw - UI_BTN_M * (UI_BTN_COUNT + 1);
    r->w = total / UI_BTN_COUNT;
    r->h = UI_BTN_H;
    r->x = UI_BTN_M + i * (r->w + UI_BTN_M);
    r->y = sh - UI_BTN_H - UI_BTN_M;
}

int ui_button_at(int px, int py, int sw, int sh)
{
    for (int i = 0; i < UI_BTN_COUNT; i++) {
        if (!ui_button_present(i)) continue;
        ui_rect_t r;
        ui_button_rect(i, sw, sh, &r);
        const int hx = r.x - UI_BTN_PAD_SIDE, hw = r.w + 2 * UI_BTN_PAD_SIDE;
        const int hy = r.y - UI_BTN_PAD_TOP;
        if (px >= hx && px < hx + hw && py >= hy && py < sh) return i;
    }
    return -1;
}

int ui_map_bottom(int sh)
{
    return sh - UI_BTN_H - UI_BTN_M - UI_BTN_PAD_TOP;
}

bool ui_pan_cell(int px, int py, int sw, int sh, int top, int *dx, int *dy)
{
    const int bot = ui_map_bottom(sh);
    if (py < top || py >= bot || bot <= top || sw <= 0) return false;
    int col = (px * 3) / sw;
    int row = ((py - top) * 3) / (bot - top);
    /* src: original handleTouch(): a controller reporting at or past the
     * edge would index a fourth square. */
    if (col < 0) col = 0;
    if (col > 2) col = 2;
    if (row < 0) row = 0;
    if (row > 2) row = 2;
    *dx = col - 1;
    *dy = row - 1;
    return true;
}

bool ui_wake_zone(int px, int py, int sw, int sh)
{
    return px >= sw / 3 && px < 2 * sw / 3 && py >= sh / 3 && py < 2 * sh / 3;
}

void ui_panel_rect(int sw, int sh, ui_rect_t *r)
{
    r->w = sw * 3 / 4;
    r->h = UI_SP_HEAD_H + UI_SP_FOOT_H + UI_SET_COUNT * UI_SP_ROW_H;
    if (r->h > sh - 40) r->h = sh - 40;
    r->x = (sw - r->w) / 2;
    r->y = (sh - r->h) / 2;
}

int ui_panel_at(int px, int py, int sw, int sh)
{
    ui_rect_t r;
    ui_panel_rect(sw, sh, &r);
    if (px < r.x || px >= r.x + r.w || py < r.y || py >= r.y + r.h) return UI_PANEL_OUTSIDE;
    if (py > r.y + r.h - UI_SP_FOOT_H)
        return px < r.x + UI_SP_CLOSE_W ? UI_PANEL_CLOSE : UI_PANEL_NOTHING;
    if (py < r.y + UI_SP_HEAD_H) return UI_PANEL_NOTHING;
    const int row = (py - (r.y + UI_SP_HEAD_H)) / UI_SP_ROW_H;
    return row < UI_SET_COUNT ? row : UI_PANEL_NOTHING;
}

void ui_pins_rect(int sw, int sh, ui_rect_t *r)
{
    r->w = sw * 2 / 3;
    r->h = sh * 2 / 3;
    r->x = (sw - r->w) / 2;
    r->y = (sh - r->h) / 2;
}

int ui_pins_rows(int sw, int sh)
{
    ui_rect_t r;
    ui_pins_rect(sw, sh, &r);
    const int rows = (r.h - UI_PP_HEAD_H - UI_PP_FOOT_H) / UI_PP_ROW_H;
    return rows < 1 ? 1 : rows;
}

ui_pins_hit_t ui_pins_at(int px, int py, int sw, int sh, int *row)
{
    ui_rect_t r;
    ui_pins_rect(sw, sh, &r);
    *row = -1;
    /* A tap anywhere outside closes it: requiring the close button would
     * trap anyone who did not find it, over a map they cannot see. */
    if (px < r.x || px >= r.x + r.w || py < r.y || py >= r.y + r.h) return UI_PINS_OUTSIDE;
    /* src: original pinPanelTouch()'s regions, in its order. */
    if (py < r.y + UI_PP_HEAD_H - 10)
        return px > r.x + r.w - 200 ? UI_PINS_SAVE : UI_PINS_NOTHING;
    if (py > r.y + r.h - UI_PP_FOOT_H) {
        if (px < r.x + 170) return UI_PINS_CLOSE;
        if (px < r.x + 400) return UI_PINS_STOP;
        if (px > r.x + r.w - 154 && px < r.x + r.w - 86) return UI_PINS_UP;
        if (px > r.x + r.w - 86) return UI_PINS_DOWN;
        return UI_PINS_NOTHING;
    }
    if (py < r.y + UI_PP_HEAD_H) return UI_PINS_NOTHING;
    const int k = (py - (r.y + UI_PP_HEAD_H)) / UI_PP_ROW_H;
    if (k < 0 || k >= ui_pins_rows(sw, sh)) return UI_PINS_NOTHING;
    *row = k;
    return px > r.x + r.w - 84 ? UI_PINS_DELETE : UI_PINS_PICK;
}

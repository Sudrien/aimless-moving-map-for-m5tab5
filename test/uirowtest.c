/*
 * uirowtest.c -- main/uirow.c: the button row, the pan squares, the wake
 * zone and the settings panel, as places on a 1280 x 720 screen.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "uirow.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SW 1280
#define SH 720
#define TOP 36      /* aimless.c's STATUS_H */

int main(void)
{
    printf("the row: five slots across, 54 px tall, 12 px from the bottom\n");
    {
        ui_rect_t r0, r4;
        ui_button_rect(0, SW, SH, &r0);
        ui_button_rect(UI_BTN_COUNT - 1, SW, SH, &r4);
        CHECK(r0.x == 12 && r0.y == SH - 54 - 12 && r0.h == 54, "first at %d,%d", r0.x, r0.y);
        CHECK(r0.w == (SW - 12 * 6) / 5, "width %d", r0.w);
        CHECK(r4.x + r4.w <= SW - 12 + 4 && r4.x + r4.w >= SW - 12 - 4, "last ends at %d", r4.x + r4.w);
        for (int i = 1; i < UI_BTN_COUNT; i++) {
            ui_rect_t a, b;
            ui_button_rect(i - 1, SW, SH, &a);
            ui_button_rect(i, SW, SH, &b);
            CHECK(b.x == a.x + a.w + 12, "gap before %d", i);
        }
    }

    printf("a tap finds the button under it, padded, and only present ones\n");
    for (int i = 0; i < UI_BTN_COUNT; i++) {
        ui_rect_t r;
        ui_button_rect(i, SW, SH, &r);
        const int want = ui_button_present(i) ? i : -1;
        CHECK(ui_button_at(r.x + r.w / 2, r.y + r.h / 2, SW, SH) == want, "middle of %d", i);
        CHECK(ui_button_at(r.x + r.w / 2, r.y - 25, SW, SH) == want, "26 px above %d", i);
        CHECK(ui_button_at(r.x + r.w / 2, SH - 1, SW, SH) == want, "the bottom edge under %d", i);
        CHECK(ui_button_at(r.x - 6, r.y + 5, SW, SH) == want, "6 px left of %d", i);
        CHECK(ui_button_at(r.x + r.w / 2, r.y - 27, SW, SH) == -1, "27 px above %d", i);
    }
    CHECK(ui_button_present(UI_BTN_HOME) && ui_button_present(UI_BTN_SET) &&
          ui_button_present(UI_BTN_SLEEP), "the three");
    CHECK(!ui_button_present(UI_BTN_PINS) && !ui_button_present(UI_BTN_CACHE), "the two to come");
    CHECK(ui_button_at(SW / 2, SH / 2, SW, SH) == -1, "the middle of the map");

    printf("the pan squares: three by three over the map band\n");
    {
        int dx = 9, dy = 9;
        const int bot = ui_map_bottom(SH);
        CHECK(bot == SH - 54 - 12 - 26, "map bottom %d", bot);
        CHECK(!ui_pan_cell(SW / 2, TOP - 1, SW, SH, TOP, &dx, &dy), "in the status bar");
        CHECK(!ui_pan_cell(SW / 2, bot, SW, SH, TOP, &dx, &dy), "in the row's zone");
        CHECK(ui_pan_cell(SW / 2, (TOP + bot) / 2, SW, SH, TOP, &dx, &dy) && dx == 0 && dy == 0, "middle");
        CHECK(ui_pan_cell(0, TOP, SW, SH, TOP, &dx, &dy) && dx == -1 && dy == -1, "north-west");
        CHECK(ui_pan_cell(SW - 1, bot - 1, SW, SH, TOP, &dx, &dy) && dx == 1 && dy == 1, "south-east");
        CHECK(ui_pan_cell(SW + 50, TOP + 1, SW, SH, TOP, &dx, &dy) && dx == 1 && dy == -1, "past the edge");
        CHECK(ui_pan_cell(-5, (TOP + bot) / 2, SW, SH, TOP, &dx, &dy) && dx == -1 && dy == 0, "before the edge");
        /* Every pixel lands in exactly one square, each square a third. */
        int n[3][3] = { { 0 } };
        for (int y = TOP; y < bot; y++)
            for (int x = 0; x < SW; x++)
                if (ui_pan_cell(x, y, SW, SH, TOP, &dx, &dy)) n[dy + 1][dx + 1]++;
        const int third = SW * (bot - TOP) / 9;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                CHECK(n[r][c] > third - SW && n[r][c] < third + SW, "square %d,%d has %d", r, c, n[r][c]);
    }

    printf("the wake zone is the middle ninth, and no button is in it\n");
    {
        CHECK(ui_wake_zone(SW / 2, SH / 2, SW, SH), "centre");
        CHECK(!ui_wake_zone(SW / 3 - 1, SH / 2, SW, SH), "left of it");
        CHECK(!ui_wake_zone(SW / 2, 2 * SH / 3, SW, SH), "under it");
        int both = 0;
        for (int y = SH / 3; y < 2 * SH / 3; y++)
            for (int x = SW / 3; x < 2 * SW / 3; x++) both += ui_button_at(x, y, SW, SH) >= 0;
        CHECK(both == 0, "%d wake pixels are buttons", both);
    }

    printf("the settings panel: rows, close, outside\n");
    {
        ui_rect_t p;
        ui_panel_rect(SW, SH, &p);
        CHECK(p.w == SW * 3 / 4 && p.x == (SW - p.w) / 2, "width %d at %d", p.w, p.x);
        CHECK(p.h == 70 + 62 + UI_SET_COUNT * 62 && p.y == (SH - p.h) / 2, "height %d", p.h);
        CHECK(ui_panel_at(p.x - 1, p.y + 80, SW, SH) == UI_PANEL_OUTSIDE, "left of it");
        CHECK(ui_panel_at(p.x + 10, p.y + 10, SW, SH) == UI_PANEL_NOTHING, "the heading");
        for (int i = 0; i < UI_SET_COUNT; i++) {
            const int y = p.y + 70 + i * 62 + 31;
            CHECK(ui_panel_at(p.x + p.w / 2, y, SW, SH) == i, "row %d", i);
        }
        CHECK(ui_panel_at(p.x + 20, p.y + p.h - 20, SW, SH) == UI_PANEL_CLOSE, "close");
        CHECK(ui_panel_at(p.x + p.w - 20, p.y + p.h - 20, SW, SH) == UI_PANEL_NOTHING, "the footer's right");
        /* On a short screen it is held to the screen less 40. */
        ui_panel_rect(SW, 200, &p);
        CHECK(p.h == 160, "short screen %d", p.h);
    }

    printf("the overrides cycle back to automatic\n");
    {
        ui_theme_t t = UI_THEME_AUTO;
        t = ui_theme_next(t); CHECK(t == UI_THEME_DAY, "day");
        t = ui_theme_next(t); CHECK(t == UI_THEME_NIGHT, "night");
        t = ui_theme_next(t); CHECK(t == UI_THEME_AUTO, "auto");
        ui_bright_t b = UI_BRIGHT_AUTO;
        b = ui_bright_next(b); CHECK(b == UI_BRIGHT_LOW, "low");
        b = ui_bright_next(b); CHECK(b == UI_BRIGHT_MED, "med");
        b = ui_bright_next(b); CHECK(b == UI_BRIGHT_HIGH, "high");
        b = ui_bright_next(b); CHECK(b == UI_BRIGHT_AUTO, "auto");
    }

    printf("\nuirowtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

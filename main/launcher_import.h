/*
 * launcher_import.h -- M5Launcher's saved networks, copied into ours.
 *
 * When this program runs under M5Launcher, Launcher's own list of
 * networks is in config.conf on the card, encrypted. This reads it,
 * finds the key in Launcher's partition (launcherkey.h), and saves each
 * network with wifistore_save() -- the same store the portal and
 * defeatist write. It joins nothing and never logs a password.
 *
 * defeatist-music-player-for-m5tab5's main/launcher_import.h at 74535ca,
 * cut down: no exit-to-Launcher, and the status line is plain English
 * rather than an i18n key, since nothing here is translated. The work
 * runs on its own task and the status is copied out, as there.
 *
 * Called at boot when nothing is saved and Launcher is present
 * (aimless.c), so a network typed into Launcher is not asked for again.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LI_IDLE = 0,    /* not run this boot */
    LI_RUNNING,     /* reading the card and the flash */
    LI_DONE,        /* finished; imported and skipped are final */
    LI_FAILED,      /* no config.conf, or no key found */
} li_phase_t;

typedef struct {
    li_phase_t phase;
    int imported;   /* handed to wifistore_save() and stored */
    int skipped;    /* open, unreadable or refused */
    char msg[64];   /* one line for the log and the screen */
} li_status_t;

/* Start an import on its own task and return at once.
 * ESP_ERR_INVALID_STATE if one is running. Needs the card, not the
 * radio. */
esp_err_t launcher_import_request(void);

/* Copy the state out. Safe from any task. */
void launcher_import_status(li_status_t *out);

/* Whether this was started from M5Launcher: a factory app partition
 * exists and is not the one running. One partition-table lookup. */
bool launcher_present(void);

#ifdef __cplusplus
}
#endif

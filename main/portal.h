/*
 * portal.h -- the way a Wi-Fi network gets into this program.
 *
 * There is no keyboard, and a WPA passphrase is up to 63 characters of
 * anything. So: an open access point, a DNS server that answers every
 * name with this device's address, and a form served to whatever phone
 * joins. The phone has the keyboard. The original did the same
 * (original/portal.cpp); this is defeatist-music-player-for-m5tab5's
 * main/portal.c at 74535ca, which is that design rewritten on plain
 * ESP-IDF for this board and this C6, cut down to what a map needs.
 *
 * WHAT IS KEPT FROM DEFEATIST'S, and why -- its portal.h has the long
 * version, with the measurements:
 *
 *   It does not block. The original's portal_run() owned the screen for
 *   five minutes. Here the map keeps drawing and following the fix while
 *   the portal runs on its own task; the screen reads a copied snapshot,
 *   portal_state(), and is never handed a pointer the portal owns.
 *
 *   Tried before it is stored. A submitted network is joined first, and
 *   written only if the join worked, through wifijoin_try().
 *
 *   The PSK before the passphrase, except where the scan shows WPA3,
 *   which refuses a precomputed key (portalweb_join_plan()).
 *
 *   Every SSID is escaped into the page; the password is never logged.
 *
 *   "Aimless-XXXX", hashed from the MAC, so two in a room differ.
 *
 *   Five minutes, then the AP comes down by itself, because the case
 *   that matters is the one where nobody comes back to close it.
 *
 * WHAT IS LEFT OUT: defeatist's station mode (the same page served over
 * a network already joined, for adding radio stations) and its stations
 * section; its translations; and its pause hook, since there is no
 * playback to pause. Every buffer a handler used to keep on the HTTP
 * task's stack is a static here (CLAUDE.md): one server task runs one
 * handler at a time, so a static is the request's own.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 32 bytes plus a terminator, as wifistore. */
#define PORTAL_SSID_MAX     (32)

/* src: defeatist portal.h PORTAL_TIMEOUT_S; the original's
 * portal_run(300000) in tab5_map.cpp. */
#define PORTAL_TIMEOUT_S    (300)

typedef enum {
    PORTAL_OFF = 0,     /* not running                                  */
    PORTAL_STARTING,    /* scanning, AP coming up; not yet joinable     */
    PORTAL_WAITING,     /* AP up, nothing submitted                     */
    PORTAL_TRYING,      /* joining with a submitted credential          */
    PORTAL_SAVED,       /* joined and stored; about to close            */
    PORTAL_FAILED,      /* the credential did not work; still waiting   */
    PORTAL_TIMEDOUT,    /* five minutes with nothing saved; AP down     */
    PORTAL_ERROR,       /* could not start, or the radio went away      */
} portal_status_t;

/* A snapshot, every field a value. */
typedef struct {
    portal_status_t status;
    /* The AP this device is broadcasting, for the screen. */
    char ap_ssid[PORTAL_SSID_MAX + 1];
    /* The network last submitted; never the password. */
    char last_ssid[PORTAL_SSID_MAX + 1];
    /* Phones associated with the AP. Zero tells "nobody has joined yet"
     * from "somebody is filling in the form". */
    uint8_t clients;
    /* Counting down from PORTAL_TIMEOUT_S; 0 when not running. */
    uint16_t seconds_left;
    /* The form's address, for a phone that does not open it by itself. */
    char url_ip[16];
} portal_state_t;

/* Once at boot, before anything else here: the portal's task. */
void portal_init(void);

/*
 * Bring the AP up, and return as soon as that is under way.
 *
 * Needs the radio already up (wifi_up()); ESP_ERR_INVALID_STATE
 * otherwise, rather than starting a transmitter as a side effect. ESP_OK
 * and nothing changed if it is already running.
 */
esp_err_t portal_start(void);

/* Up or coming up. A value, safe anywhere. */
bool portal_running(void);

/* Ask for the portal to close, and return at once. For the screen. */
void portal_request_stop(void);

/* Close it and wait, up to about twenty seconds when a join is under
 * way. For the network's portal_stop hook; not from the draw loop. */
esp_err_t portal_stop(void);

/* Copy the state out. Safe from any task; PORTAL_OFF and empty strings
 * when nothing has run. */
void portal_state(portal_state_t *out);

#ifdef __cplusplus
}
#endif

/*
 * portal.c -- see portal.h.
 *
 * defeatist-music-player-for-m5tab5's main/portal.c at 74535ca, setup
 * mode only. Three pieces on two tasks, as there:
 *
 *   the portal task   owns the lifecycle and the join: scans, raises the
 *                     AP, starts DNS and HTTP, counts the timeout, and
 *                     runs every join attempt.
 *   the DNS task      answers every name with the AP's address, through
 *                     dnsreply_build(), until told to stop.
 *   esp_http_server   its own task. Handlers read state and hand a
 *                     submitted credential to the portal task; none waits
 *                     on the radio.
 *
 * What parses a byte from a phone is portalweb.c and dnsreply.c, tested
 * on the host. This is the plumbing around them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dnsreply.h"
#include "portalweb.h"
#include "wifi.h"
#include "wifijoin.h"
#include "wifistore.h"

static const char *TAG = "portal";

/* src: defeatist portal.c SEEN_MAX. A page lists this many networks;
 * the scan keeps the strongest. */
#define SEEN_MAX            (24)
/* src: defeatist portal.c bring_up(): the scan is asked for this many. */
#define SCAN_MAX            (64)
/* src: defeatist portal.c BODY_MAX. Longest form body accepted. */
#define BODY_MAX            (512)
/* src: defeatist portal.c SAVED_LINGER_MS. How long SAVED stays up for
 * the phone's page before the AP goes down. */
#define SAVED_LINGER_MS     (4000)
/* src: defeatist portal.c STOP_WAIT_MS: a join in progress, plus the
 * teardown. */
#define STOP_WAIT_MS        (20000)
/* src: defeatist portal.c portal_init() and dns_start(). */
#define PORTAL_STACK        (6144)
#define PORTAL_PRIO         (3)
#define DNS_STACK           (4096)
#define DNS_PRIO            (4)
#define HTTP_STACK          (6144)
/* src: defeatist portal.c http_start(). */
#define HTTP_SOCKETS        (4)
/* A message built for the page: the longest is the non-ASCII hint,
 * about 300 bytes with an escaped name in it. src: chosen. */
#define MSG_MAX             (512)

/* ---- state shared across tasks, all under s_mu ------------------------ */

static SemaphoreHandle_t s_mu;
static portal_state_t    s_st;
static esp_err_t         s_last_err;
static bool              s_pending;
static char              s_pend_ssid[WIFISTORE_SSID_MAX + 1];
static char              s_pend_pass[WIFISTORE_SECRET_MAX + 1];

static volatile bool     s_running;
static volatile bool     s_want_start;
static volatile bool     s_want_stop;

/* Written by the portal task before the server starts and not again
 * while it runs, so handlers read them without the lock. */
static wifi_seen_t       s_seen[SEEN_MAX];
static int               s_seen_n;
static int               s_hidden_n;
/* src: the ESP-IDF AP netif's default address; dhcp_offer_dns() reads
 * the real one back. */
static char              s_ap_ip[16] = "192.168.4.1";

static httpd_handle_t    s_httpd;               /* portal task only */
static TickType_t        s_deadline;

static SemaphoreHandle_t s_kick;
static SemaphoreHandle_t s_stopped;
static volatile bool     s_dns_run;
static SemaphoreHandle_t s_dns_done;

/* The HTTP task's working space. One server task runs one handler at a
 * time, so these are each request's own. */
static char s_body[BODY_MAX];
static char s_chosen[129], s_typed[129], s_ssid[129];
static char s_pass[WIFISTORE_SECRET_MAX + 1];
static char s_msg[MSG_MAX];
static char s_esc[WIFISTORE_SSID_MAX * 6 + 1];
static char s_hint[128];
static char s_esc_hint[128 * 6 + 1];
/* The DNS task's. */
static uint8_t s_dq[DNSREPLY_MAX], s_dr[DNSREPLY_MAX];

static void set_status(portal_status_t st, const char *last_ssid)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.status = st;
    if (last_ssid) snprintf(s_st.last_ssid, sizeof(s_st.last_ssid), "%s", last_ssid);
    xSemaphoreGive(s_mu);
}

void portal_state(portal_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_mu) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_mu);
}

bool portal_running(void) { return s_running; }

/* ---- DNS --------------------------------------------------------------- */

static void dns_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: no socket");
        goto out;
    }

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = inet_addr(s_ap_ip);    /* the AP side only */
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns: bind %s:53 failed", s_ap_ip);
        close(sock);
        goto out;
    }
    /* Half a second, so a stop is noticed without a packet arriving. */
    const struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const uint32_t a = ntohl(addr.sin_addr.s_addr);
    const uint8_t ip[4] = { (uint8_t)(a >> 24), (uint8_t)(a >> 16),
                            (uint8_t)(a >> 8),  (uint8_t)a };
    unsigned answered = 0;
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        const int n = recvfrom(sock, s_dq, sizeof(s_dq), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        const size_t m = dnsreply_build(s_dq, (size_t)n, ip, s_dr, sizeof(s_dr));
        if (m) {
            sendto(sock, s_dr, m, 0, (struct sockaddr *)&from, flen);
            answered++;
        }
    }
    close(sock);
    ESP_LOGI(TAG, "dns: stopped after %u answers", answered);
out:
    xSemaphoreGive(s_dns_done);
    vTaskDelete(NULL);
}

static bool dns_start(void)
{
    s_dns_run = true;
    xSemaphoreTake(s_dns_done, 0);                /* drain a stale signal */
    if (xTaskCreate(dns_task, "portal_dns", DNS_STACK, NULL, DNS_PRIO, NULL) != pdPASS) {
        s_dns_run = false;
        return false;
    }
    return true;
}

static void dns_stop(void)
{
    if (!s_dns_run) return;
    s_dns_run = false;
    xSemaphoreTake(s_dns_done, pdMS_TO_TICKS(2000));
}

/* Offer this AP's address as the DNS server in the DHCP lease, so the
 * phone asks dns_task and nothing else. The DHCP server has to be
 * stopped to change its options; it restarts at once. */
static void dhcp_offer_dns(void)
{
    esp_netif_t *netif = wifi_ap_netif();
    if (!netif) return;

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr)
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&info.ip));

    esp_netif_dns_info_t dns = { 0 };
    dns.ip.u_addr.ip4.addr = info.ip.addr;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    uint8_t offer = 1;

    esp_netif_dhcps_stop(netif);
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer, sizeof(offer));
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(netif);
}

/* ---- HTTP -------------------------------------------------------------- */

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>";
static const char PAGE_STYLE[] =
    "<title>Aimless Moving Map setup</title>"
    "<style>"
    "body{font:18px sans-serif;margin:24px;max-width:28em}"
    "input,select,button{font:inherit;width:100%;padding:10px;margin:6px 0;box-sizing:border-box}"
    "p.m{padding:10px;background:#eee}"
    "</style></head><body><h2>Aimless Moving Map</h2>";
static const char PAGE_TAIL[] = "</body></html>";

/* Never an empty chunk: in chunked HTTP a zero-length chunk ends the
 * response (defeatist 6047). */
static esp_err_t chunk(httpd_req_t *req, const char *s)
{
    if (!s || !*s) return ESP_OK;
    return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t chunk_escaped(httpd_req_t *req, const char *s)
{
    if (!portalweb_escape(s, s_esc, sizeof(s_esc))) return ESP_OK;
    return chunk(req, s_esc);
}

static void chunk_p(httpd_req_t *req, const char *text)
{
    chunk(req, "<p class=m>");
    chunk(req, text);
    chunk(req, "</p>");
}

static void send_head(httpd_req_t *req, const char *extra)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    chunk(req, PAGE_HEAD);
    if (extra) chunk(req, extra);
    chunk(req, PAGE_STYLE);
}

static esp_err_t send_form(httpd_req_t *req, const char *message)
{
    send_head(req, NULL);
    if (message) chunk_p(req, message);

    chunk(req, "<p>Choose the Wi-Fi network the map should use to download "
               "tiles, and enter its password. It is tried before it is "
               "saved.</p>"
               "<form method=post action=/join><label>Network<select name=ssid>");
    /* Strongest first, as scanned. */
    for (int i = 0; i < s_seen_n; i++) {
        chunk(req, "<option value=\"");
        chunk_escaped(req, s_seen[i].ssid);
        chunk(req, "\">");
        chunk_escaped(req, s_seen[i].ssid);
        char meta[64];
        snprintf(meta, sizeof(meta), " &mdash; %s, %d dBm, ch %u</option>",
                 wifi_auth_name(s_seen[i].auth), s_seen[i].rssi,
                 (unsigned)s_seen[i].channel);
        chunk(req, meta);
    }
    chunk(req, s_seen_n ? "<option value=\"\">" : "<option value=\"\" selected>");
    chunk(req, "Other or hidden network (type it below)");
    if (s_hidden_n) chunk(req, " &mdash; hidden nearby");
    chunk(req, "</option></select></label>"
               "<label>Or type a network name<input name=ssid_other maxlength=32 "
               "autocomplete=off autocapitalize=none placeholder='only if not in the list'>"
               "</label><label>Password<input name=pass type=password maxlength=64 "
               "autocomplete=off required></label><button>Join</button></form>");
    if (s_hidden_n) {
        snprintf(s_msg, sizeof(s_msg), "<p>%d hidden network%s nearby. A hidden "
                 "network's name has to be typed.</p>",
                 s_hidden_n, s_hidden_n == 1 ? "" : "s");
        chunk(req, s_msg);
    }
    snprintf(s_msg, sizeof(s_msg), "<p>This page closes itself after %d minutes. "
             "The map works without a network; this only adds one.</p>",
             PORTAL_TIMEOUT_S / 60);
    chunk(req, s_msg);
    chunk(req, PAGE_TAIL);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_root(httpd_req_t *req)
{
    return send_form(req, NULL);
}

static esp_err_t h_status(httpd_req_t *req)
{
    portal_state_t st;
    portal_state(&st);
    esp_err_t last;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    last = s_last_err;
    xSemaphoreGive(s_mu);

    const bool trying = (st.status == PORTAL_TRYING);
    send_head(req, trying ? "<meta http-equiv=refresh content=2>" : NULL);

    if (!portalweb_escape(st.last_ssid, s_esc, sizeof(s_esc))) s_esc[0] = '\0';
    switch (st.status) {
    case PORTAL_TRYING:
        snprintf(s_msg, sizeof(s_msg), "Trying %s&hellip; This can take fifteen "
                 "seconds, and this phone may lose the setup network for a "
                 "moment. The map's screen shows the result either way.", s_esc);
        break;
    case PORTAL_SAVED:
        snprintf(s_msg, sizeof(s_msg), "Joined and saved %s. The setup network "
                 "is closing; you can leave it.", s_esc);
        break;
    case PORTAL_FAILED:
        snprintf(s_msg, sizeof(s_msg), "%s",
                 last == ESP_ERR_NOT_FOUND ? "That network was not found. Check the name and try again."
               : last == ESP_ERR_TIMEOUT   ? "No answer from that network. Try again."
                                           : "The password did not work. Try again.");
        break;
    default:
        snprintf(s_msg, sizeof(s_msg), "Waiting for a network.");
        break;
    }
    chunk_p(req, s_msg);
    if (st.status == PORTAL_FAILED || st.status == PORTAL_WAITING)
        chunk(req, "<p><a href=/>Back to the form</a></p>");
    chunk(req, PAGE_TAIL);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_join(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > BODY_MAX)
        return send_form(req, "That form was too large to be a network name and a password.");
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, s_body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        got += (size_t)n;
    }

    /* Name buffers larger than an SSID on purpose: a 33-byte name has to
     * arrive whole so portalweb_check() refuses it by length. */
    if (!portalweb_field(s_body, got, "ssid", s_chosen, sizeof(s_chosen))) s_chosen[0] = '\0';
    if (!portalweb_field(s_body, got, "ssid_other", s_typed, sizeof(s_typed))) s_typed[0] = '\0';
    snprintf(s_ssid, sizeof(s_ssid), "%s", portalweb_pick_ssid(s_chosen, s_typed));
    const bool have_ssid = s_ssid[0] != '\0';
    const bool have_pass = portalweb_field(s_body, got, "pass", s_pass, sizeof(s_pass));
    memset(s_body, 0, sizeof(s_body));

    const portalweb_check_t c = (have_ssid && have_pass)
                              ? portalweb_check(s_ssid, s_pass)
                              : (have_ssid ? PORTALWEB_BAD_SECRET : PORTALWEB_BAD_SSID);

    /* What was submitted, as far as encoding goes, never what was typed
     * (portalweb_describe()). Every submission, the refused ones above
     * all. */
    if (have_ssid && have_pass) {
        portalweb_describe(s_pass, s_msg, sizeof(s_msg));
        ESP_LOGI(TAG, "submitted %.32s: password %s", s_ssid, s_msg);
    }

    if (c == PORTALWEB_NON_ASCII) {
        portalweb_non_ascii_hint(s_pass, s_hint, sizeof(s_hint));
        memset(s_pass, 0, sizeof(s_pass));
        if (!portalweb_escape(s_hint, s_esc_hint, sizeof(s_esc_hint))) s_esc_hint[0] = '\0';
        snprintf(s_msg, sizeof(s_msg), "The password contains %s. A Wi-Fi password "
                 "is plain keyboard characters only; phones substitute these when "
                 "autocorrect or smart punctuation is on. Retype it with that "
                 "turned off.", s_esc_hint);
        return send_form(req, s_msg);
    }
    const char *problem =
        c == PORTALWEB_BAD_SSID   ? "A network name is 1 to 32 characters." :
        c == PORTALWEB_NO_SECRET  ? "Open networks are not supported yet." :
        c == PORTALWEB_BAD_SECRET ? "A Wi-Fi password is 8 to 63 characters, or 64 hex digits." :
                                    NULL;
    if (problem) {
        memset(s_pass, 0, sizeof(s_pass));
        return send_form(req, problem);
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool busy = s_pending || s_st.status == PORTAL_TRYING;
    if (!busy) {
        snprintf(s_pend_ssid, sizeof(s_pend_ssid), "%.32s", s_ssid);
        memcpy(s_pend_pass, s_pass, sizeof(s_pend_pass));
        s_pending = true;
        /* TRYING now, so the page this request returns already says so. */
        s_st.status = PORTAL_TRYING;
        snprintf(s_st.last_ssid, sizeof(s_st.last_ssid), "%.32s", s_pend_ssid);
    }
    xSemaphoreGive(s_mu);
    memset(s_pass, 0, sizeof(s_pass));

    if (!busy) xSemaphoreGive(s_kick);
    return h_status(req);
}

/* Everything else: a phone's connectivity check, most likely. A redirect
 * to the form is what makes it open the sign-in sheet. */
static esp_err_t h_other(httpd_req_t *req)
{
    char loc[32];
    snprintf(loc, sizeof(loc), "http://%s/", s_ap_ip);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

static bool http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = HTTP_SOCKETS;
    cfg.stack_size = HTTP_STACK;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        s_httpd = NULL;
        return false;
    }
    /* Order matters with wildcard matching: the catch-all goes last. */
    static const httpd_uri_t uris[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = h_root   },
        { .uri = "/status", .method = HTTP_GET,  .handler = h_status },
        { .uri = "/join",   .method = HTTP_POST, .handler = h_join   },
        { .uri = "/*",      .method = HTTP_GET,  .handler = h_other  },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_httpd, &uris[i]);
    return true;
}

/* ---- the portal task --------------------------------------------------- */

static void teardown(portal_status_t final)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    dns_stop();
    wifi_ap_end();

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.status = final;
    s_st.clients = 0;
    s_st.seconds_left = 0;
    s_pending = false;
    memset(s_pend_pass, 0, sizeof(s_pend_pass));
    xSemaphoreGive(s_mu);

    s_running = false;
    s_want_start = false;
    ESP_LOGI(TAG, "portal down (%d)", (int)final);
}

static void bring_up(void)
{
    set_status(PORTAL_STARTING, "");

    /* Scan first, as a plain station: the page's list, without an AP
     * sharing the radio while it is gathered. */
    wifi_seen_t *seen = calloc(SCAN_MAX, sizeof(*seen));
    s_seen_n = 0;
    s_hidden_n = 0;
    if (seen) {
        const int n = wifi_scan_list(seen, SCAN_MAX);
        for (int i = 0; i < n && s_seen_n < SEEN_MAX; i++) {
            if (seen[i].hidden) { s_hidden_n++; continue; }
            bool dup = false;
            for (int k = 0; k < s_seen_n; k++)
                if (strcmp(s_seen[k].ssid, seen[i].ssid) == 0) { dup = true; break; }
            if (!dup) s_seen[s_seen_n++] = seen[i];
        }
        free(seen);
    }

    /* A stop that arrived during the scan, which is the long part: do not
     * raise an AP only to take it down. */
    if (s_want_stop) {
        ESP_LOGI(TAG, "stopped during the scan; not raising the AP");
        teardown(PORTAL_OFF);
        return;
    }

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char name[PORTAL_SSID_MAX + 1];
    portalweb_ap_name(mac, name, sizeof(name));

    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_st.ap_ssid, sizeof(s_st.ap_ssid), "%s", name);
    xSemaphoreGive(s_mu);

    if (wifi_ap_begin(name) != ESP_OK) {
        teardown(PORTAL_ERROR);
        return;
    }
    dhcp_offer_dns();
    if (!dns_start() || !http_start()) {
        ESP_LOGE(TAG, "could not start DNS or HTTP");
        teardown(PORTAL_ERROR);
        return;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_st.url_ip, sizeof(s_st.url_ip), "%s", s_ap_ip);
    xSemaphoreGive(s_mu);

    s_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PORTAL_TIMEOUT_S * 1000u);
    set_status(PORTAL_WAITING, NULL);
    ESP_LOGI(TAG, "portal up: join %s and open http://%s/ (%d networks listed)",
             name, s_ap_ip, s_seen_n);
}

/* What the scan said about `ssid`'s security, for the join plan. */
static portalweb_net_t scanned_net(const char *ssid)
{
    for (int i = 0; i < s_seen_n; i++)
        if (strcmp(s_seen[i].ssid, ssid) == 0) return wifijoin_net(s_seen[i].auth);
    return PORTALWEB_NET_UNKNOWN;
}

/* The portal task's own copies of a submission. Statics for CLAUDE.md's
 * stack rule; only this task touches them. */
static char s_try_ssid[WIFISTORE_SSID_MAX + 1], s_try_pass[WIFISTORE_SECRET_MAX + 1];

static bool try_join(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(s_try_ssid, s_pend_ssid, sizeof(s_try_ssid));
    memcpy(s_try_pass, s_pend_pass, sizeof(s_try_pass));
    memset(s_pend_pass, 0, sizeof(s_pend_pass));
    s_pending = false;
    xSemaphoreGive(s_mu);

    const esp_err_t err = wifijoin_try(TAG, s_try_ssid, s_try_pass, scanned_net(s_try_ssid));
    memset(s_try_pass, 0, sizeof(s_try_pass));

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_last_err = err;
    s_st.status = (err == ESP_OK) ? PORTAL_SAVED : PORTAL_FAILED;
    xSemaphoreGive(s_mu);
    return err == ESP_OK;
}

static void portal_task(void *arg)
{
    (void)arg;
    TickType_t saved_at = 0;

    for (;;) {
        xSemaphoreTake(s_kick, pdMS_TO_TICKS(1000));

        if (s_want_stop) {
            s_want_stop = false;
            if (s_running) teardown(PORTAL_OFF);
            xSemaphoreGive(s_stopped);
            continue;
        }
        if (s_want_start && !s_httpd && s_running) {
            s_want_start = false;
            bring_up();
            continue;
        }
        if (!s_running) continue;

        /* The radio went away underneath. */
        if (!wifi_up()) {
            teardown(PORTAL_ERROR);
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        portal_status_t st;
        bool pending;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        st = s_st.status;
        pending = s_pending;
        s_st.clients = (uint8_t)wifi_ap_clients();
        s_st.seconds_left = (now < s_deadline)
                          ? (uint16_t)((s_deadline - now) / configTICK_RATE_HZ) : 0;
        xSemaphoreGive(s_mu);

        if (pending) {
            if (try_join()) saved_at = xTaskGetTickCount();
            continue;
        }
        if (st == PORTAL_SAVED) {
            if (saved_at && (now - saved_at) >= pdMS_TO_TICKS(SAVED_LINGER_MS)) {
                saved_at = 0;
                teardown(PORTAL_SAVED);
            }
            continue;
        }
        if (now >= s_deadline) {
            ESP_LOGI(TAG, "five minutes with nothing saved; closing");
            teardown(PORTAL_TIMEDOUT);
        }
    }
}

void portal_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_kick = xSemaphoreCreateBinary();
    s_stopped = xSemaphoreCreateBinary();
    s_dns_done = xSemaphoreCreateBinary();
    if (!s_mu || !s_kick || !s_stopped || !s_dns_done ||
        xTaskCreate(portal_task, "portal", PORTAL_STACK, NULL, PORTAL_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "portal unavailable this boot");
        s_mu = NULL;
    }
}

esp_err_t portal_start(void)
{
    if (!s_mu) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_OK;
    if (!wifi_up()) {
        ESP_LOGW(TAG, "not starting: the radio is off");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    memset(&s_st, 0, sizeof(s_st));
    s_st.status = PORTAL_STARTING;
    s_st.seconds_left = PORTAL_TIMEOUT_S;
    s_last_err = ESP_OK;
    xSemaphoreGive(s_mu);

    s_want_stop = false;
    s_running = true;
    s_want_start = true;
    xSemaphoreGive(s_kick);
    return ESP_OK;
}

void portal_request_stop(void)
{
    if (!s_mu || !s_running) return;
    s_want_stop = true;
    xSemaphoreGive(s_kick);
}

esp_err_t portal_stop(void)
{
    if (!s_mu || !s_running) return ESP_OK;
    xSemaphoreTake(s_stopped, 0);                 /* drain a stale signal */
    s_want_stop = true;
    xSemaphoreGive(s_kick);
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STOP_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "portal did not stop within %d ms", STOP_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

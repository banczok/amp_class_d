/* link.h - ESP-NOW link to the amplifier's on-board ESP32-C3.
 *
 * Control only.  Everything here is small, urgent and must work before
 * WiFi has associated - volume, power, transport, and the track text.
 * The cover image is not urgent and does not come this way; see art.h.
 */
#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "proto.h"

#define NP_TEXT_MAX 64

typedef struct {
    uint8_t power;        /* 0 standby, 1 starting, 2 on, 3 stopping */
    uint8_t vol_code;     /* PGA code, 0..192                        */
    uint8_t muted;
    uint8_t input;
    uint8_t eq_path;
    uint8_t max_vol;
    bool    valid;
} amp_state_t;

typedef struct {
    bool    playing;
    uint8_t art_id;       /* 0 = no cover                            */
    char    artist[NP_TEXT_MAX];
    char    title[NP_TEXT_MAX];
    char    album[NP_TEXT_MAX];
    bool    valid;
} nowplaying_t;

void link_init(void);
void link_hello(void);                    /* announce on every wake    */

void link_send_key(fn_id_t fn, bool repeat);
void link_send_volume(int detents);       /* signed, coalesced burst   */

amp_state_t  link_amp(void);
nowplaying_t link_nowplaying(void);

/* Bumped whenever either of the above changes, so the UI can redraw on
 * a change instead of polling fields. */
uint32_t link_revision(void);

bool link_amp_reachable(void);   /* heard from it in the last few seconds */
bool link_ever_heard(void);      /* heard from it at all since this wake  */
float link_volume_db(void);
#endif

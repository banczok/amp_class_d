/* state.h - the little the C3 needs to remember.
 *
 * The C3 is a relay, not a controller: it caches the last amp state and
 * the last now-playing blob purely so a knob that has just woken up gets
 * an answer immediately instead of waiting a round trip through the Pico
 * and the Pi.  Nothing here is authoritative.
 */
#ifndef STATE_H
#define STATE_H

#include <stdint.h>
#include <stdbool.h>

#define NOWPLAYING_MAX 200

typedef struct {
    uint8_t power;        /* power_state_t on the Pico          */
    uint8_t vol_code;
    uint8_t muted;
    uint8_t input;
    uint8_t eq_path;
    uint8_t max_vol;
    bool    valid;
} amp_state_t;

void        state_init(void);

void        state_set_amp(const uint8_t *p, int n);
amp_state_t state_amp(void);

/* Milliseconds since the Pico last told us anything.  The knob's
 * keepalive must not wake a sleeping Pico every time. */
int64_t     state_amp_age_ms(void);

void        state_set_nowplaying(const uint8_t *p, int n);
int         state_nowplaying(uint8_t *out, int max);   /* returns length */

#endif /* STATE_H */

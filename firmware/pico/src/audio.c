/* audio.c - the audio state and the two clamps the design doc mandates.
 *
 *   1. The PGA never goes above 0 dB (code 192).  Its positive-gain range
 *      is noisy and the chain does not need it.
 *   2. EQ boost is clamped against volume:  max_boost = min(15, -3 - pga_dB)
 *      At PGA 0 dB the chain already delivers 2.12 V differential into an
 *      amplifier that clips at 3.05 V, so boost there clips the amp.
 *
 * Both are enforced here, and the PGA ceiling is enforced again inside
 * pga_write() so no code path can route around it.
 *
 * Balance is done in the PGA because it has independent L/R registers;
 * the BD37033 front faders stay at unity.  That keeps the tone block
 * symmetric and puts the trim ahead of it.
 */
#include "audio.h"
#include "board.h"
#include "pga2320.h"
#include "bd37033.h"
#include "relays.h"
#include "settings.h"
#include "pico/stdlib.h"
#include <string.h>

static audio_state_t st;

void audio_init(void)
{
    memset(&st, 0, sizeof st);
    st.vol_code    = pga_db_to_code(-40.0f);   /* sane, quiet start */
    st.balance     = 0;
    st.muted       = false;
    st.input       = IN_SEL_DIGITAL;
    st.eq_path     = EQ_PATH_ACTIVE;
    st.tone_f0[0]  = 1; st.tone_q[0] = 1;
    st.tone_f0[1]  = 1; st.tone_q[1] = 1;
    st.tone_f0[2]  = 0; st.tone_q[2] = 0;
    st.loudness    = 0;
    st.loudness_f0 = 0;
}

float audio_boost_headroom(void)
{
    return eq_max_boost_db(pga_code_to_db(st.vol_code));
}

/* Re-clamp every band after a volume change - turning the volume up must
 * pull the EQ down with it, not just refuse the next boost. */
static void reclamp_tone(void)
{
    float lim = audio_boost_headroom();
    for (int b = 0; b < 3; b++)
        if (st.tone_gain[b] > (int)lim) st.tone_gain[b] = (int8_t)lim;
}

void audio_apply(void)
{
    reclamp_tone();

    /* Balance by attenuating the louder side, never by boosting. */
    int l = st.vol_code, r = st.vol_code;
    if (st.balance > 0)      l -= st.balance;
    else if (st.balance < 0) r += st.balance;
    if (l < PGA_CODE_MIN) l = PGA_CODE_MIN;
    if (r < PGA_CODE_MIN) r = PGA_CODE_MIN;

    if (st.muted) { l = PGA_CODE_MUTE; r = PGA_CODE_MUTE; }
    pga_write((uint8_t)l, (uint8_t)r);

    for (int b = 0; b < 3; b++) {
        bd_set_band_shape((bd_band_t)b, st.tone_f0[b], st.tone_q[b]);
        bd_set_tone((bd_band_t)b, st.tone_gain[b]);
    }
    bd_set_loudness(st.loudness, st.loudness_f0, false);

    relay_input(st.input);
    relay_eq_path(st.eq_path);
}

void audio_set_volume(int code)
{
    /* Two ceilings, both enforced here so no caller can route around
     * either: the hardware 0 dB limit, and the user ceiling set in the
     * Levels menu. */
    int ceiling = settings()->max_vol;
    if (ceiling > PGA_CODE_MAX || ceiling < PGA_CODE_MIN) ceiling = PGA_CODE_MAX;
    if (code > ceiling)      code = ceiling;
    if (code < PGA_CODE_MIN) code = PGA_CODE_MIN;
    st.vol_code = (uint8_t)code;
    audio_apply();
}

void audio_volume_step(int steps) { audio_set_volume(st.vol_code + steps); }
int   audio_volume_code(void)     { return st.vol_code; }
float audio_volume_db(void)       { return pga_code_to_db(st.vol_code); }

void audio_set_mute(bool m)
{
    st.muted = m;
    /* Use the PGA's own mute pin as well as the code: it is a zero-cross
     * mute in hardware and does not disturb the gain setting. */
    pga_set_mute_pin(m);
    audio_apply();
}
void audio_toggle_mute(void) { audio_set_mute(!st.muted); }
bool audio_muted(void)       { return st.muted; }

void audio_set_balance(int half_db)
{
    if (half_db >  24) half_db =  24;
    if (half_db < -24) half_db = -24;
    st.balance = (int8_t)half_db;
    audio_apply();
}

void audio_set_input(uint8_t sel)
{
    /* The selector relay is not click-free, and the BD37033's advanced
     * switch does not cover the input path.  Mute across the change. */
    bool was = bd_mute_pin();
    bd_set_mute_pin(true);
    sleep_ms(5);
    st.input = sel;
    relay_input(sel);
    sleep_ms(15);                    /* TQ2SA operate time is 4 ms */
    bd_set_mute_pin(was);
}

void audio_set_eq_path(uint8_t path)
{
    bool was = bd_mute_pin();
    bd_set_mute_pin(true);
    sleep_ms(5);
    st.eq_path = path;
    relay_eq_path(path);
    sleep_ms(15);
    bd_set_mute_pin(was);
}

void audio_set_tone(bd_band_t b, int gain_db)
{
    float lim = audio_boost_headroom();
    if (gain_db >  (int)lim) gain_db = (int)lim;
    if (gain_db < -15)       gain_db = -15;
    st.tone_gain[b] = (int8_t)gain_db;
    bd_set_tone(b, gain_db);
}

void audio_set_band_shape(bd_band_t b, uint8_t f0, uint8_t q)
{
    st.tone_f0[b] = f0;
    st.tone_q[b]  = q;
    bd_set_band_shape(b, f0, q);
}

void audio_set_loudness(uint8_t gain_db, uint8_t f0)
{
    st.loudness    = gain_db;
    st.loudness_f0 = f0;
    bd_set_loudness(gain_db, f0, false);
}

void audio_hard_mute(bool on)
{
    /* BD37033 MUTE pin acts in microseconds, where the PGA's mute is a
     * zero-cross ramp and a relay is milliseconds.  This is the one to
     * use around register writes and power transitions. */
    bd_set_mute_pin(on);
    pga_set_mute_pin(on || st.muted);
}

const audio_state_t *audio_get(void) { return &st; }

void audio_set_all(const audio_state_t *s)
{
    st = *s;
    if (st.vol_code > PGA_CODE_MAX) st.vol_code = PGA_CODE_MAX;
    audio_apply();
}

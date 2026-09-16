#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>
#include <stdbool.h>
#include "bd37033.h"

typedef struct {
    uint8_t vol_code;      /* PGA code 1..192, 0 = mute                  */
    int8_t  balance;       /* half-dB steps, -24..+24, + = right louder  */
    bool    muted;         /* user mute                                  */
    uint8_t input;         /* IN_SEL_*                                   */
    uint8_t eq_path;       /* EQ_PATH_*                                  */
    int8_t  tone_gain[3];  /* bass, mid, treble, dB                      */
    uint8_t tone_f0[3];
    uint8_t tone_q[3];
    uint8_t loudness;      /* 0 = off, else dB                           */
    uint8_t loudness_f0;
} audio_state_t;

void  audio_init(void);
void  audio_apply(void);                 /* push the whole state to hw   */

void  audio_set_volume(int code);
void  audio_volume_step(int steps);
int   audio_volume_code(void);
float audio_volume_db(void);

void  audio_set_mute(bool m);
void  audio_toggle_mute(void);
bool  audio_muted(void);

void  audio_set_balance(int half_db);
void  audio_set_input(uint8_t sel);
void  audio_set_eq_path(uint8_t path);

void  audio_set_tone(bd_band_t b, int gain_db);
void  audio_set_band_shape(bd_band_t b, uint8_t f0, uint8_t q);
void  audio_set_loudness(uint8_t gain_db, uint8_t f0);

/* Headroom currently available for EQ boost, in dB.  The panel greys out
 * anything above this and shows why. */
float audio_boost_headroom(void);

/* Hard mute used around BD37033 register writes and power transitions. */
void  audio_hard_mute(bool on);

const audio_state_t *audio_get(void);
void  audio_set_all(const audio_state_t *s);
#endif

#ifndef BD37033_H
#define BD37033_H
#include <stdint.h>
#include <stdbool.h>

/* Tone band selector */
typedef enum { BD_BASS = 0, BD_MID, BD_TREBLE } bd_band_t;

bool bd_init(void);                             /* reset + safe defaults */
void bd_set_mute_pin(bool muted);               /* hardware pin via Q5   */
bool bd_mute_pin(void);

void bd_set_volume_db(int db);                  /* +15 .. -79, 0 = unity */
void bd_set_fader_db(int left_db, int right_db);/* front outputs         */
void bd_set_tone(bd_band_t b, int gain_db);     /* -15 .. +15            */
void bd_set_band_shape(bd_band_t b, uint8_t f0_idx, uint8_t q_idx);
void bd_set_loudness(uint8_t gain_db, uint8_t f0_idx, bool hi_cut);

/* Labels for the panel UI - indices match the register encodings. */
extern const char *bd_bass_f0[4];
extern const char *bd_mid_f0[4];
extern const char *bd_treble_f0[4];
extern const char *bd_bass_q[4];
extern const char *bd_mid_q[4];
extern const char *bd_treble_q[2];
#endif

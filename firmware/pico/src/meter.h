#ifndef METER_H
#define METER_H
#include <stdint.h>
#include <stdbool.h>

#define METER_BANDS 32

typedef struct {
    uint16_t peak_l, peak_r;        /* 0..2047, relative to the 1.65 V bias */
    uint16_t rms_l,  rms_r;
    /* Per channel, not a mono sum.  Every visualiser on the panel is
     * stereo, and a summed spectrum cannot be un-summed at the far end.
     * The second transform costs about 2% of core 1. */
    uint8_t  band_l[METER_BANDS];   /* log-spaced, 0..255, 3/8 dB a step  */
    uint8_t  band_r[METER_BANDS];
} meter_result_t;

void meter_init(void);
void meter_start(void);
void meter_stop(void);
bool meter_get(meter_result_t *out);   /* true when a new block is ready */
void meter_core1_main(void);           /* entry point for core 1         */

/* Decimated mono audio for the Pi: 48 kHz in, half-band filtered, 24 kHz
 * out, mu-law encoded.  Off by default - it is 26% of the UART and there
 * is no point paying that while the digital input is selected, where the
 * Pi already knows what is playing. */
void meter_audio_enable(bool on);
bool meter_audio_enabled(void);

/* Stereo doubles the rate.  Only the per-channel visualisers need it -
 * a fingerprint is summed to mono before it is looked at. */
void meter_audio_stereo(bool on);
bool meter_audio_is_stereo(void);

/* Drain up to max bytes into out.  Single producer (core 1), single
 * consumer (core 0), no lock. */
uint16_t meter_audio_read(uint8_t *out, uint16_t max);
#endif

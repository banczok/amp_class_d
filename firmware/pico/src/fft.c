/* fft.c - compact Q15 radix-2 decimation-in-time FFT.
 *
 * Fixed point rather than float because this runs continuously on core1
 * alongside the DMA metering; at 48 kHz and 1024 points that is roughly
 * 47 transforms a second and we would rather not spend the cycles.
 *
 * Each stage shifts right by one to prevent overflow, so the output is
 * scaled by 1/N overall.  That is fine for a display: we only care about
 * relative magnitude, and it keeps everything inside int16.
 */
#include "fft.h"
#include <math.h>

static int16_t  tw_re[FFT_N / 2];
static int16_t  tw_im[FFT_N / 2];
static uint16_t rev[FFT_N];

void fft_init(void)
{
    for (int i = 0; i < FFT_N / 2; i++) {
        double a = -2.0 * M_PI * i / FFT_N;
        tw_re[i] = (int16_t)lrint(cos(a) * 32767.0);
        tw_im[i] = (int16_t)lrint(sin(a) * 32767.0);
    }
    for (int i = 0; i < FFT_N; i++) {
        unsigned r = 0, x = (unsigned)i;
        for (int b = 0; b < FFT_LOG2N; b++) { r = (r << 1) | (x & 1); x >>= 1; }
        rev[i] = (uint16_t)r;
    }
}

void fft_run(int16_t *re, int16_t *im)
{
    for (int i = 0; i < FFT_N; i++) {
        int j = rev[i];
        if (j > i) {
            int16_t t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len >> 1;
        int step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len) {
            for (int j = 0, k = 0; j < half; j++, k += step) {
                int32_t wr = tw_re[k], wi = tw_im[k];
                int32_t xr = re[i + j + half], xi = im[i + j + half];
                int32_t tr = (wr * xr - wi * xi) >> 15;
                int32_t ti = (wr * xi + wi * xr) >> 15;
                int32_t ur = re[i + j], ui = im[i + j];
                /* Halve at every stage: overall scaling is 1/N. */
                re[i + j]        = (int16_t)((ur + tr) >> 1);
                im[i + j]        = (int16_t)((ui + ti) >> 1);
                re[i + j + half] = (int16_t)((ur - tr) >> 1);
                im[i + j + half] = (int16_t)((ui - ti) >> 1);
            }
        }
    }
}

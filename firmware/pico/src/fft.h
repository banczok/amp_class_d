#ifndef FFT_H
#define FFT_H
#include <stdint.h>
#define FFT_N     1024
#define FFT_LOG2N 10
void fft_init(void);
/* In-place complex FFT, Q15.  im[] may be pre-zeroed for a real input. */
void fft_run(int16_t *re, int16_t *im);
#endif

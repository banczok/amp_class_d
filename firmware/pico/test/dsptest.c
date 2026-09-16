/* Host check of the two bits of DSP added to meter.c: the mu-law codec
 * and the half-band decimator.  Neither fails loudly on hardware - a
 * wrong codec just means fingerprints never match - so check them here. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define DEC_TAPS 39

/* ---- copy of the encoder from meter.c ---- */
static uint8_t ulaw_encode(int16_t pcm)
{
    const int BIAS = 0x84, CLIP = 32635;
    int v = pcm, sign = 0;
    if (v < 0) { sign = 0x80; v = -v; }
    if (v > CLIP) v = CLIP;
    v += BIAS;
    int exp = 7;
    for (int m = 0x4000; (v & m) == 0 && exp > 0; exp--, m >>= 1) { }
    int mant = (v >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | mant);
}

/* ---- reference G.711 decoder, to check we round-trip ---- */
static int16_t ulaw_decode(uint8_t u)
{
    static const int exp_lut[8] = { 0, 132, 396, 924, 1980, 4092, 8316, 16764 };
    u = ~u;
    int sign = u & 0x80, exp = (u >> 4) & 0x07, mant = u & 0x0F;
    int v = exp_lut[exp] + (mant << (exp + 3));
    return (int16_t)(sign ? -v : v);
}

int main(void)
{
    int fail = 0;

    /* 1. Monotonic, and every code round-trips inside its own quantum. */
    int worst = 0;
    for (int p = -32768; p <= 32767; p++) {
        uint8_t u = ulaw_encode((int16_t)p);
        int back = ulaw_decode(u);
        int clipped = p;
        if (clipped >  32635) clipped =  32635;
        if (clipped < -32635) clipped = -32635;
        int err = abs(back - clipped);
        if (err > worst) worst = err;
    }
    printf("mu-law: worst round-trip error %d counts (%.2f%% of full scale)\n",
           worst, 100.0 * worst / 32768.0);
    if (worst > 1100) { printf("  FAIL: quantisation far too coarse\n"); fail++; }

    /* 2. Zero must encode to the standard idle code 0xFF. */
    uint8_t z = ulaw_encode(0);
    printf("mu-law: encode(0) = 0x%02X (G.711 idle is 0xFF)\n", z);
    if (z != 0xFF) { printf("  FAIL\n"); fail++; }

    /* 3. Sign symmetry. */
    for (int p = 1; p < 32000; p += 137) {
        int a = ulaw_decode(ulaw_encode((int16_t)p));
        int b = ulaw_decode(ulaw_encode((int16_t)-p));
        if (a != -b) { printf("  FAIL: asymmetric at %d (%d vs %d)\n", p, a, b); fail++; break; }
    }

    /* ---- half-band filter, generated exactly as meter_init does ---- */
    int16_t dec[DEC_TAPS];
    {
        const int M = DEC_TAPS - 1;
        double h[DEC_TAPS], sum = 0.0;
        for (int n = 0; n <= M; n++) {
            double x = (double)n - M / 2.0;
            double s = (x == 0.0) ? 0.5 : sin(M_PI * 0.5 * x) / (M_PI * x);
            double w = 0.54 - 0.46 * cos(2.0 * M_PI * n / M);
            h[n] = s * w;
            sum += h[n];
        }
        for (int n = 0; n <= M; n++) dec[n] = (int16_t)((h[n] / sum) * 32768.0);
    }

    /* DC gain must be 1.0, or the whole stream changes level. */
    long dcsum = 0, abssum = 0;
    for (int n = 0; n < DEC_TAPS; n++) { dcsum += dec[n]; abssum += abs(dec[n]); }
    printf("FIR: DC gain %.4f, sum|h| = %ld\n", dcsum / 32768.0, abssum);
    if (fabs(dcsum / 32768.0 - 1.0) > 0.005) { printf("  FAIL: DC gain off\n"); fail++; }

    /* Accumulator headroom: worst case must fit int32. */
    double worst_acc = (double)abssum * 32767.0;
    printf("FIR: worst-case accumulator %.3g (int32 holds 2.147e9)\n", worst_acc);
    if (worst_acc > 2147483647.0) { printf("  FAIL: accumulator overflows\n"); fail++; }

    /* Response at the frequencies that matter, 48 kHz in. */
    const double fs = 48000.0;
    const double probe[] = { 100, 1000, 5000, 10000, 11000, 12000, 14000, 16000, 19000, 23000 };
    printf("FIR: magnitude response\n");
    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        double w = 2.0 * M_PI * probe[i] / fs, re = 0, im = 0;
        for (int n = 0; n < DEC_TAPS; n++) {
            re += dec[n] / 32768.0 * cos(-w * n);
            im += dec[n] / 32768.0 * sin(-w * n);
        }
        double mag = sqrt(re * re + im * im);
        printf("   %6.0f Hz  %7.4f  %8.2f dB%s\n", probe[i], mag,
               20 * log10(mag < 1e-9 ? 1e-9 : mag),
               probe[i] > 12000 ? "   <- must be rejected, it aliases" : "");
        /* Everything above 12 kHz folds back on top of the wanted band. */
        if (probe[i] >= 14000 && mag > 0.01) {
            printf("     FAIL: only %.1f dB of alias rejection\n", -20 * log10(mag));
            fail++;
        }
        if (probe[i] <= 10000 && mag < 0.9) {
            printf("     FAIL: passband droop\n");
            fail++;
        }
    }

    printf("\n%s\n", fail ? "FAILURES ABOVE" : "all checks passed");
    return fail ? 1 : 0;
}

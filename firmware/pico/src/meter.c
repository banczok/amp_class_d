#include <math.h>
/* meter.c - continuous stereo level and spectrum metering.
 *
 * The front end taps the buffer output, ahead of the volume control, so
 * the reading is volume-independent - that was a deliberate choice in
 * the design doc, because post-volume metering would give about nine ADC
 * counts at -40 dB.
 *
 * The signal arrives biased at 1.65 V through a Sallen-Key anti-alias
 * filter (fc ~ 19.9 kHz, Q 0.707), attenuated 0.69x by the 10k/22k
 * network.  So full scale here is roughly 1.5 V peak at the buffer.
 *
 * ADC runs round-robin over both channels at 96 kSPS total, 48 kHz per
 * channel, straight into a ping-pong DMA buffer.  Core 1 does the DSP so
 * the control loop on core 0 never has to wait for it.
 */
#include "meter.h"
#include "fft.h"
#include "board.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include <string.h>

#define BLOCK_PAIRS  FFT_N          /* samples per channel per block */
#define BUF_WORDS    (BLOCK_PAIRS * 2)

static uint16_t s_buf[2][BUF_WORDS];
static int      s_dma_a, s_dma_b;
static volatile int  s_ready = -1;     /* buffer index awaiting the DSP */
static volatile bool s_run   = false;

static meter_result_t s_out;
static volatile bool  s_out_new = false;

static int16_t s_re[FFT_N], s_im[FFT_N];
static int16_t s_win[FFT_N];

/* ------------------------------------------------ audio to the Pi ---
 *
 * The Pi has no converter on it and the amplifier does not route audio
 * its way, so without this the analogue input has no title, no artist
 * and no cover - and the waveform visualisers have nothing to draw.
 *
 * 48 kHz in, half-band filtered, decimated by two, mu-law encoded, out
 * at 24 kHz.  The filter is the part that is not optional: the board's
 * anti-alias filter sits at ~19.9 kHz because the ADC runs at 48 kHz.
 * Simply sampling slower would fold 12-19.9 kHz back into the audible
 * band, and invented spectral energy is exactly what stops a fingerprint
 * from matching.
 */
/* 39 taps, Hamming.  Measured on the host: flat to 10 kHz, -44 dB at
 * 14 kHz, -90 dB at 16 kHz.  23 taps managed only -20 dB at 14 kHz,
 * which folds back to 10 kHz as a phantom peak on the spectrum and as
 * noise a fingerprinter has to work through.  Worst-case accumulator is
 * 1.72e9, inside int32. */
#define DEC_TAPS      39
#define AUDIO_RING  4096            /* 170 ms of slack at 24 kHz */

static int16_t s_dec[DEC_TAPS];     /* Q15 half-band coefficients */
static int16_t s_fir_zl[DEC_TAPS], s_fir_zr[DEC_TAPS];
static int     s_fir_i, s_fir_phase;

static uint8_t          s_a_buf[AUDIO_RING];
static volatile uint16_t s_a_head, s_a_tail;
static volatile bool     s_audio_on;
static volatile bool     s_audio_stereo;

/* G.711 mu-law.  Eight bits per sample instead of sixteen, for a
 * difference no fingerprinter can tell. */
static uint8_t ulaw_encode(int16_t pcm)
{
    const int BIAS = 0x84, CLIP = 32635;
    /* Widen before negating.  -32768 has no positive int16, so negating
     * in place leaves it negative and every branch below then reads a
     * sign bit that should not be there. */
    int v = pcm, sign = 0;
    if (v < 0) { sign = 0x80; v = -v; }
    if (v > CLIP) v = CLIP;
    v += BIAS;
    int exp = 7;
    for (int m = 0x4000; (v & m) == 0 && exp > 0; exp--, m >>= 1) { }
    int mant = (v >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | mant);
}

static void ring_put(uint8_t b)
{
    uint16_t nh = (uint16_t)((s_a_head + 1) & (AUDIO_RING - 1));
    if (nh == s_a_tail) return;    /* Pi is not draining; drop, do not stall */
    s_a_buf[s_a_head] = b;
    s_a_head = nh;
}

static int32_t fir(const int16_t *z, int oldest)
{
    int32_t acc = 0;
    int k = oldest;
    for (int n = 0; n < DEC_TAPS; n++) {
        acc += (int32_t)z[k] * s_dec[n];
        if (++k == DEC_TAPS) k = 0;
    }
    int32_t v = acc >> 15;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return v;
}

/* Both channels always go through the filter, even in mono.  Filtering
 * then averaging and averaging then filtering are the same thing for a
 * linear filter, and one code path is worth more than the 900 kMAC/s. */
static void audio_push(int32_t l, int32_t r)
{
    if (!s_audio_on) return;

    s_fir_zl[s_fir_i] = (int16_t)(l << 4);        /* 12-bit -> 16-bit */
    s_fir_zr[s_fir_i] = (int16_t)(r << 4);
    if (++s_fir_i == DEC_TAPS) s_fir_i = 0;

    if (++s_fir_phase < 2) return;                /* decimate by two */
    s_fir_phase = 0;

    int32_t vl = fir(s_fir_zl, s_fir_i);          /* s_fir_i is the oldest */
    int32_t vr = fir(s_fir_zr, s_fir_i);

    if (s_audio_stereo) {
        ring_put(ulaw_encode((int16_t)vl));
        ring_put(ulaw_encode((int16_t)vr));
    } else {
        ring_put(ulaw_encode((int16_t)((vl + vr) >> 1)));
    }
}

void meter_audio_enable(bool on)
{
    if (on == s_audio_on) return;
    if (on) {
        memset(s_fir_zl, 0, sizeof s_fir_zl);
        memset(s_fir_zr, 0, sizeof s_fir_zr);
        s_fir_i = s_fir_phase = 0;
        s_a_head = s_a_tail = 0;
    }
    s_audio_on = on;
}

bool meter_audio_enabled(void) { return s_audio_on; }

void meter_audio_stereo(bool on)
{
    if (on == s_audio_stereo) return;

    /* Changing width mid-stream leaves the old interleaving in the ring,
     * and the Pi would hear the channels swap.  Stop, change, restart:
     * audio_push() checks the enable flag on entry, so core 1 stops
     * feeding before the buffers are cleared. */
    bool was = s_audio_on;
    if (was) meter_audio_enable(false);
    s_audio_stereo = on;
    if (was) meter_audio_enable(true);
}

bool meter_audio_is_stereo(void) { return s_audio_stereo; }

uint16_t meter_audio_read(uint8_t *out, uint16_t max)
{
    uint16_t head = s_a_head;                    /* one snapshot; core 1
                                                  * may advance it, which
                                                  * only ever makes this
                                                  * read smaller */
    uint16_t avail = (uint16_t)((head - s_a_tail) & (AUDIO_RING - 1));
    uint16_t n = avail < max ? avail : max;

    /* Never split an L,R pair.  Rounding down here rather than in the
     * caller matters: the caller has already consumed whatever it was
     * given, so trimming there would silently drop a byte and swap the
     * channels for every frame afterwards. */
    if (s_audio_stereo) n &= (uint16_t)~1u;

    for (uint16_t i = 0; i < n; i++) {
        out[i] = s_a_buf[s_a_tail];
        s_a_tail = (uint16_t)((s_a_tail + 1) & (AUDIO_RING - 1));
    }
    return n;
}

/* Band edges: 32 log-spaced bands from ~40 Hz to ~18 kHz.
 * Bin width at 48 kHz / 1024 points is 46.9 Hz. */
static uint16_t s_edge[METER_BANDS + 1];

static void dma_isr(void)
{
    if (dma_channel_get_irq0_status(s_dma_a)) {
        dma_channel_acknowledge_irq0(s_dma_a);
        s_ready = 0;
    } else if (dma_channel_get_irq0_status(s_dma_b)) {
        dma_channel_acknowledge_irq0(s_dma_b);
        s_ready = 1;
    }
}

void meter_init(void)
{
    fft_init();

    for (int i = 0; i < FFT_N; i++) {
        /* Hann window in Q15 */
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (FFT_N - 1));
        s_win[i] = (int16_t)(w * 32767.0);
    }
    /* Windowed-sinc half-band, cutoff at fs/4 = 12 kHz.  Generated here
     * rather than pasted as a table so the tap count is one #define. */
    {
        const int M = DEC_TAPS - 1;
        double h[DEC_TAPS], sum = 0.0;
        for (int n = 0; n <= M; n++) {
            double x = (double)n - M / 2.0;
            double s = (x == 0.0) ? 0.5 : sin(M_PI * 0.5 * x) / (M_PI * x);
            double w = 0.54 - 0.46 * cos(2.0 * M_PI * n / M);   /* Hamming */
            h[n] = s * w;
            sum += h[n];
        }
        for (int n = 0; n <= M; n++)
            s_dec[n] = (int16_t)((h[n] / sum) * 32768.0);      /* unity at DC */
    }

    for (int b = 0; b <= METER_BANDS; b++) {
        double f = 40.0 * pow(18000.0 / 40.0, (double)b / METER_BANDS);
        uint16_t bin = (uint16_t)(f / (48000.0 / FFT_N));
        if (bin < 1) bin = 1;
        if (bin > FFT_N / 2 - 1) bin = FFT_N / 2 - 1;
        s_edge[b] = bin;
    }

    adc_init();
    adc_gpio_init(PIN_ADC_R);
    adc_gpio_init(PIN_ADC_L);
    adc_set_round_robin((1u << ADC_CH_R) | (1u << ADC_CH_L));
    adc_select_input(ADC_CH_R);
    adc_fifo_setup(true, true, 1, false, false);
    /* 48 MHz / (clkdiv + 1) = 96 kSPS total => 48 kHz per channel. */
    adc_set_clkdiv(499.0f);

    s_dma_a = dma_claim_unused_channel(true);
    s_dma_b = dma_claim_unused_channel(true);

    dma_channel_config ca = dma_channel_get_default_config(s_dma_a);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_16);
    channel_config_set_read_increment(&ca, false);
    channel_config_set_write_increment(&ca, true);
    channel_config_set_dreq(&ca, DREQ_ADC);
    channel_config_set_chain_to(&ca, s_dma_b);
    dma_channel_configure(s_dma_a, &ca, s_buf[0], &adc_hw->fifo, BUF_WORDS, false);

    dma_channel_config cb = dma_channel_get_default_config(s_dma_b);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_16);
    channel_config_set_read_increment(&cb, false);
    channel_config_set_write_increment(&cb, true);
    channel_config_set_dreq(&cb, DREQ_ADC);
    channel_config_set_chain_to(&cb, s_dma_a);
    dma_channel_configure(s_dma_b, &cb, s_buf[1], &adc_hw->fifo, BUF_WORDS, false);

    dma_channel_set_irq0_enabled(s_dma_a, true);
    dma_channel_set_irq0_enabled(s_dma_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_isr);
    irq_set_enabled(DMA_IRQ_0, true);
}

void meter_start(void)
{
    if (s_run) return;
    adc_fifo_drain();
    dma_channel_start(s_dma_a);
    adc_run(true);
    s_run = true;
}

void meter_stop(void)
{
    if (!s_run) return;
    adc_run(false);
    dma_channel_abort(s_dma_a);
    dma_channel_abort(s_dma_b);
    adc_fifo_drain();
    s_run = false;
    s_ready = -1;
    meter_audio_enable(false);      /* no rails, no audio */
}

/* Turn the transform in s_re/s_im into 32 log-spaced band levels.
 *
 * The level is a log of the band's peak power.  The old version counted
 * leading bits and multiplied by eight, which quantised the whole display
 * to 3 dB steps - visible as bars that jump rather than move.  Taking
 * three more bits below the leading one puts it at 3/8 dB, over a 96 dB
 * range, in the same byte. */
static void bands_from_fft(uint8_t *out)
{
    for (int b = 0; b < METER_BANDS; b++) {
        uint32_t acc = 0;
        int lo = s_edge[b], hi = s_edge[b + 1];
        if (hi <= lo) hi = lo + 1;
        for (int k = lo; k < hi; k++) {
            int32_t re = s_re[k], im = s_im[k];
            uint32_t mag = (uint32_t)(re * re + im * im);
            if (mag > acc) acc = mag;      /* peak-hold within the band */
        }
        int val = 0;
        if (acc) {
            int e = 31;
            while (!(acc & (1u << e))) e--;               /* MSB index */
            uint32_t mant = (e >= 3) ? ((acc >> (e - 3)) & 7u)
                                     : ((acc << (3 - e)) & 7u);
            val = (e << 3) | (int)mant;
        }
        out[b] = (uint8_t)val;
    }
}

/* Window one channel into the transform buffers.  Round-robin fills the
 * FIFO R,L,R,L... because ADC_CH_R < ADC_CH_L and the scan restarts at
 * the lowest set channel, so off is 0 for right and 1 for left. */
static void load_channel(const uint16_t *buf, int off, int32_t dc)
{
    for (int i = 0; i < BLOCK_PAIRS; i++) {
        int32_t v = (int32_t)buf[2 * i + off] - dc;
        s_re[i] = (int16_t)(((v << 4) * s_win[i]) >> 15);
        s_im[i] = 0;
    }
}

static void process(const uint16_t *buf)
{
    /* Round-robin fills the FIFO R,L,R,L... because ADC_CH_R < ADC_CH_L
     * and the scan restarts at the lowest set channel. */
    int32_t dcl = 0, dcr = 0;
    for (int i = 0; i < BLOCK_PAIRS; i++) {
        dcr += buf[2 * i];
        dcl += buf[2 * i + 1];
    }
    dcr /= BLOCK_PAIRS;
    dcl /= BLOCK_PAIRS;

    uint32_t pkl = 0, pkr = 0;
    uint64_t sql = 0, sqr = 0;
    for (int i = 0; i < BLOCK_PAIRS; i++) {
        int32_t r = (int32_t)buf[2 * i]     - dcr;
        int32_t l = (int32_t)buf[2 * i + 1] - dcl;
        uint32_t ar = (uint32_t)(r < 0 ? -r : r);
        uint32_t al = (uint32_t)(l < 0 ? -l : l);
        if (ar > pkr) pkr = ar;
        if (al > pkl) pkl = al;
        sqr += (uint64_t)r * r;
        sql += (uint64_t)l * l;

        /* The decimator gets the samples unwindowed - a Hann taper is
         * exactly what an audio stream must not have. */
        audio_push(l, r);
    }

    /* Two transforms over the same buffers, one channel at a time, so
     * stereo costs no extra RAM.  buf is still ours until we return. */
    load_channel(buf, 1, dcl);
    fft_run(s_re, s_im);
    bands_from_fft(s_out.band_l);

    load_channel(buf, 0, dcr);
    fft_run(s_re, s_im);
    bands_from_fft(s_out.band_r);

    s_out.peak_l = (uint16_t)(pkl > 2047 ? 2047 : pkl);
    s_out.peak_r = (uint16_t)(pkr > 2047 ? 2047 : pkr);
    uint32_t rl = 0, rr = 0;
    {   /* integer sqrt of the mean square */
        uint64_t ml = sql / BLOCK_PAIRS, mr = sqr / BLOCK_PAIRS;
        while ((uint64_t)(rl + 1) * (rl + 1) <= ml) rl++;
        while ((uint64_t)(rr + 1) * (rr + 1) <= mr) rr++;
    }
    s_out.rms_l = (uint16_t)(rl > 2047 ? 2047 : rl);
    s_out.rms_r = (uint16_t)(rr > 2047 ? 2047 : rr);
    s_out_new = true;
}

void meter_core1_main(void)
{
    for (;;) {
        int idx = s_ready;
        if (idx >= 0) {
            s_ready = -1;
            process(s_buf[idx]);
        } else {
            tight_loop_contents();
        }
    }
}

bool meter_get(meter_result_t *out)
{
    if (!s_out_new) return false;
    uint32_t save = save_and_disable_interrupts();
    *out = s_out;
    s_out_new = false;
    restore_interrupts(save);
    return true;
}

/* bd37033.c - ROHM BD37033FV-M sound processor over I2C.
 *
 * Bus format is the plain register write:  S | 0x40<<1 | reg | data | P
 *
 * Three of this part's power-on defaults are traps, and all three are
 * fixed in bd_init():
 *
 *   reg 0x28/0x29  front fader   default 0xFF -- and 0xFF means MUTE.
 *                                Nothing comes out until these are set.
 *   reg 0x20       volume        default 0x00, which the datasheet lists
 *                                in the "Prohibition" range.
 *   reg 0x03       mixing 1/2ch  default 0 == ON, so the unconnected MIN
 *                                pin gets summed into the output.
 *
 * Volume and fader share the encoding  value = 0x80 - gain_dB, covering
 * +15 dB (0x71) through -79 dB (0xCF), with 0xFF as mute on the faders.
 */
#include "bd37033.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#define BD_ADDR 0x40

/* select addresses */
#define R_INIT      0x01
#define R_LPF       0x02
#define R_MIX       0x03
#define R_INSEL     0x05
#define R_INGAIN    0x06
#define R_VOLUME    0x20
#define R_FADER_1F  0x28   /* 1ch front = left  */
#define R_FADER_2F  0x29   /* 2ch front = right */
#define R_FADER_1R  0x2A
#define R_FADER_2R  0x2B
#define R_FADER_1S  0x2C
#define R_MIXGAIN   0x30
#define R_BASS_SET  0x41
#define R_MID_SET   0x44
#define R_TREB_SET  0x47
#define R_BASS_GAIN 0x51
#define R_MID_GAIN  0x54
#define R_TREB_GAIN 0x57
#define R_LOUDNESS  0x75
#define R_RESET     0xFE

const char *bd_bass_f0[4]   = { "60 Hz",  "80 Hz",  "100 Hz", "120 Hz" };
const char *bd_mid_f0[4]    = { "0.5 kHz","1 kHz",  "1.5 kHz","2.5 kHz" };
const char *bd_treble_f0[4] = { "7.5 kHz","10 kHz", "12.5 kHz","15 kHz" };
const char *bd_bass_q[4]    = { "0.5", "1.0", "1.5", "2.0" };
const char *bd_mid_q[4]     = { "0.75","1.00","1.25","1.50" };
const char *bd_treble_q[2]  = { "0.75","1.25" };

static bool s_mute_pin = true;

static bool wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_write_blocking(BD_I2C, BD_ADDR, b, 2, false) == 2;
}

/* value = 0x80 - dB, clamped to the legal window */
static uint8_t gain_code(int db)
{
    if (db >  15) db =  15;
    if (db < -79) db = -79;
    return (uint8_t)(0x80 - db);
}

void bd_set_mute_pin(bool muted)
{
    /* Q5 is an NPN pulling IC1 pin 15 (active-low MUTE) down.
     * GPIO high -> transistor on -> pin low -> chip muted. */
    s_mute_pin = muted;
    gpio_put(PIN_BD_MUTE, muted ? BD_MUTE_ON : BD_MUTE_OFF);
}
bool bd_mute_pin(void) { return s_mute_pin; }

bool bd_init(void)
{
    gpio_init(PIN_BD_MUTE);
    gpio_set_dir(PIN_BD_MUTE, GPIO_OUT);
    bd_set_mute_pin(true);              /* mute before the rails settle */

    i2c_init(BD_I2C, BD_I2C_BAUD);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    /* Board has 4k7 pull-ups at the Pico end; no internal pulls. */

    if (!wr(R_RESET, 0x81)) return false;
    sleep_ms(5);

    /* Advanced Switch on (click-free level changes), anti-alias filter
     * on, shortest transition times. */
    wr(R_INIT, 0xA0);

    /* Subwoofer/LPF block unused: everything off. */
    wr(R_LPF, 0x00);

    /* Mixing OFF on both channels (bits are inverted: 1 = OFF).  MIN is
     * an unconnected 100k input pin - do not sum it in. */
    wr(R_MIX, 0x03);

    /* Input selector: A_Single = A1/A2, which is where the PGA lands. */
    wr(R_INSEL, 0x00);

    /* Input gain block to 0 dB, its own mute bit off.  The gain range
     * runs to +16 dB and must never be left there: it sits ahead of the
     * volume control and would blow the whole gain plan. */
    wr(R_INGAIN, 0x00);

    /* Internal volume to unity - the PGA2320 owns volume in this design. */
    wr(R_VOLUME, gain_code(0));

    /* Front faders carry the signal; anything else stays muted. */
    wr(R_FADER_1F, gain_code(0));
    wr(R_FADER_2F, gain_code(0));
    wr(R_FADER_1R, 0xFF);
    wr(R_FADER_2R, 0xFF);
    wr(R_FADER_1S, 0xFF);
    wr(R_MIXGAIN,  0xFF);

    /* Flat tone, mid-range shapes. */
    bd_set_band_shape(BD_BASS,   1, 1);   /* 80 Hz,  Q 1.0  */
    bd_set_band_shape(BD_MID,    1, 1);   /* 1 kHz,  Q 1.00 */
    bd_set_band_shape(BD_TREBLE, 0, 0);   /* 7.5 kHz,Q 0.75 */
    bd_set_tone(BD_BASS,   0);
    bd_set_tone(BD_MID,    0);
    bd_set_tone(BD_TREBLE, 0);
    wr(R_LOUDNESS, 0x00);

    return true;
}

void bd_set_volume_db(int db)          { wr(R_VOLUME, gain_code(db)); }

void bd_set_fader_db(int left_db, int right_db)
{
    wr(R_FADER_1F, gain_code(left_db));
    wr(R_FADER_2F, gain_code(right_db));
}

void bd_set_tone(bd_band_t b, int gain_db)
{
    if (gain_db >  15) gain_db =  15;
    if (gain_db < -15) gain_db = -15;
    uint8_t mag = (uint8_t)(gain_db < 0 ? -gain_db : gain_db);
    uint8_t v   = (uint8_t)((gain_db < 0 ? 0x80 : 0x00) | (mag & 0x1F));
    wr(b == BD_BASS ? R_BASS_GAIN : b == BD_MID ? R_MID_GAIN : R_TREB_GAIN, v);
}

void bd_set_band_shape(bd_band_t b, uint8_t f0_idx, uint8_t q_idx)
{
    /* D5:D4 = f0, D1:D0 = Q  (treble Q is a single bit, D0). */
    uint8_t qmask = (b == BD_TREBLE) ? 0x01 : 0x03;
    uint8_t v = (uint8_t)(((f0_idx & 0x03) << 4) | (q_idx & qmask));
    wr(b == BD_BASS ? R_BASS_SET : b == BD_MID ? R_MID_SET : R_TREB_SET, v);
}

void bd_set_loudness(uint8_t gain_db, uint8_t f0_idx, bool hi_cut)
{
    if (gain_db > 15) gain_db = 15;
    wr(R_LOUDNESS, (uint8_t)((hi_cut ? 0x20 : 0x00) | (gain_db & 0x1F)));
    /* Loudness f0 lives in the mixing register, bits D3:D2. */
    wr(R_MIX, (uint8_t)(0x03 | ((f0_idx & 0x03) << 2)));
}

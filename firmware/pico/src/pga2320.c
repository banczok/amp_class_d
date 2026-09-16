/* pga2320.c - PGA2320 stereo volume control.
 *
 * Serial format, straight from the datasheet (SBOS312B, figure 2):
 *
 *   SDI:  R7 R6 R5 R4 R3 R2 R1 R0  L7 L6 L5 L4 L3 L2 L1 L0
 *
 * Right channel byte FIRST, MSB first, latched on the rising edge of
 * SCLK, and the gain registers load on the rising edge of CS.  Power-on
 * state is 0x00 on both channels, i.e. muted, which is what we want.
 *
 * Everything here goes through U3 (74AHCT125) to reach the PGA at 5 V,
 * so the GPIO sense is non-inverting.
 */
#include "pga2320.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

static bool s_mute_pin = true;

void pga_init(void)
{
    /* Mute first, before anything else can make a noise. */
    gpio_init(PIN_PGA_MUTE);
    gpio_set_dir(PIN_PGA_MUTE, GPIO_OUT);
    gpio_put(PIN_PGA_MUTE, 0);          /* ~MUTE low = muted */
    s_mute_pin = true;

    gpio_init(PIN_PGA_CS);
    gpio_set_dir(PIN_PGA_CS, GPIO_OUT);
    gpio_put(PIN_PGA_CS, 1);            /* ~CS idles high */

    spi_init(PGA_SPI, PGA_SPI_BAUD);
    /* Mode 0: clock idles low, data sampled on the rising edge. */
    spi_set_format(PGA_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_PGA_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_PGA_SDI,  GPIO_FUNC_SPI);

    pga_write(PGA_CODE_MUTE, PGA_CODE_MUTE);
}

void pga_write(uint8_t left, uint8_t right)
{
    /* Hardware ceiling.  The doc limits the PGA to 0 dB; its positive
     * gain range is noisy and the chain never needs it.  Enforced here
     * as well as in audio.c so no path can slip past it. */
    if (left  > PGA_CODE_MAX) left  = PGA_CODE_MAX;
    if (right > PGA_CODE_MAX) right = PGA_CODE_MAX;

    const uint8_t frame[2] = { right, left };   /* right goes first */

    gpio_put(PIN_PGA_CS, 0);
    busy_wait_us(1);                            /* tCSCR = 90 ns min */
    spi_write_blocking(PGA_SPI, frame, 2);
    busy_wait_us(1);                            /* tCFCS = 35 ns min */
    gpio_put(PIN_PGA_CS, 1);                    /* rising edge latches */
}

void pga_set_mute_pin(bool muted)
{
    s_mute_pin = muted;
    gpio_put(PIN_PGA_MUTE, muted ? 0 : 1);
}

bool pga_mute_pin(void) { return s_mute_pin; }

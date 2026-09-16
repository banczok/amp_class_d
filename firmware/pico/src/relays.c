/* relays.c - the four relay coils.
 *
 * All four drives are NPN low-side with a 10k base pull-down, so a GPIO
 * that is low - or high-impedance, which is how the RP2350 comes out of
 * reset - leaves the coil de-energised.  That is deliberate: it means
 * mains off, DAC selected and the EQ in circuit are all the power-on
 * state without firmware having to do anything.
 *
 * U14 and U15 coils live in the DGND domain while their contacts sit in
 * the audio path.  The coil is a floating two-terminal load and both its
 * wires come from DGND, so coil current never enters AGND.
 */
#include "relays.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

static bool    s_mains = false, s_secondary = false;
static uint8_t s_input = IN_SEL_DIGITAL, s_eq = EQ_PATH_ACTIVE;

static void out_low(uint pin)
{
    gpio_init(pin);
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);   /* drive low before enabling output */
}

void relays_init(void)
{
    out_low(PIN_RLY_MAINS);
    out_low(PIN_RLY_SECONDARY);
    out_low(PIN_RLY_INPUT);
    out_low(PIN_RLY_EQ_BYPASS);
    s_mains = s_secondary = false;
    s_input = IN_SEL_DIGITAL;
    s_eq    = EQ_PATH_ACTIVE;
}

void relay_mains(bool on)       { s_mains = on;     gpio_put(PIN_RLY_MAINS, on); }
void relay_secondary(bool on)   { s_secondary = on; gpio_put(PIN_RLY_SECONDARY, on); }
void relay_input(uint8_t sel)   { s_input = sel;    gpio_put(PIN_RLY_INPUT, sel == IN_SEL_ANALOG); }
void relay_eq_path(uint8_t p)   { s_eq = p;         gpio_put(PIN_RLY_EQ_BYPASS, p == EQ_PATH_BYPASS); }

bool    relay_mains_state(void)     { return s_mains; }
bool    relay_secondary_state(void) { return s_secondary; }
uint8_t relay_input_state(void)     { return s_input; }
uint8_t relay_eq_path_state(void)   { return s_eq; }

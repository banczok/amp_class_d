/* board.h - pin map and hardware constants for the preamp controller.
 *
 * Target: Raspberry Pi Pico 2 (RP2350A) on amp_ctrl rev A.
 * Every pin below was taken from the KiCad netlist, not from memory.
 */
#ifndef BOARD_H
#define BOARD_H

#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"

/* ---------------------------------------------------------------- pins */

/* Rotary encoder.  All three have a 10k pull-up to +3V3, a 1k series
 * resistor and 100n to ground, so they are active-low and already
 * debounced with tau ~ 1 ms.  No internal pulls needed - and note the
 * RP2350-E9 erratum makes internal pull-downs unreliable anyway. */
#define PIN_ENC_SW        0
#define PIN_ENC_B         1
#define PIN_ENC_A         2

/* IR receiver (TSOP-class, open-collector, 22k pull-up on the board). */
#define PIN_IR            3

/* UART1 -> ESP32-C3 SuperMini.  Pico GP4 drives the ESP's GPIO20/U0RXD,
 * Pico GP5 listens to the ESP's GPIO21/U0TXD. */
#define PIN_ESP_TX        4
#define PIN_ESP_RX        5
#define UART_ESP          uart1
#define UART_ESP_BAUD     115200

/* Relay drives.  All four are NPN low-side (BC847) with a 10k base
 * pull-down, so GPIO low == coil de-energised == the reset state. */
#define PIN_RLY_MAINS     6    /* K3  230 V, 2 pole                       */
#define PIN_RLY_SECONDARY 7    /* K2  T1 2x12 V secondary                 */
#define PIN_RLY_INPUT     8    /* U14 input select                        */
#define PIN_RLY_EQ_BYPASS 22   /* U15 EQ bypass                           */

/* Front-panel power button: 10k pull-up + 100n, active low. */
#define PIN_PWR_BTN       9

/* PGA2320 three-wire port.  These four GPIOs feed the 74AHCT125 level
 * shifter (U3); the AHCT outputs drive the PGA at 5 V. */
#define PIN_PGA_SCLK      10
#define PIN_PGA_SDI       11
#define PIN_PGA_CS        12
#define PIN_PGA_MUTE      13   /* PGA ~MUTE, active LOW  -> low = muted   */
#define PGA_SPI           spi1
#define PGA_SPI_BAUD      1000000

/* Status LEDs, external, 1k series, ~1.3 mA. */
#define PIN_LED_1         14
#define PIN_LED_2         15

/* UART0 -> Raspberry Pi through the ADuM/ISO7742 isolator. */
#define PIN_PI_TX         16
#define PIN_PI_RX         17
#define UART_PI           uart0
#define UART_PI_BAUD      921600

/* Discrete handshake lines to the Pi, also through the isolator. */
#define PIN_SHUTDOWN_REQ  18   /* out: assert to ask the Pi to halt       */
#define PIN_HALTED        19   /* in : Pi asserts when it has halted      */

/* I2C to the BD37033 (and out to header J13). 33R series on both lines,
 * 4k7 pull-ups at the Pico end. */
#define PIN_SDA           20
#define PIN_SCL           21
#define BD_I2C            i2c0
#define BD_I2C_BAUD       100000

/* Metering ADC inputs, fed from the MCP6004 Sallen-Key stage, biased
 * at 1.65 V.  ADC0 = right, ADC1 = left. */
#define PIN_ADC_R         26
#define PIN_ADC_L         27
#define ADC_CH_R          0
#define ADC_CH_L          1

/* BD37033 hardware MUTE via Q5 (NPN, open collector on IC1 pin 15).
 * GPIO HIGH -> Q5 on -> MUTE pin pulled low -> chip MUTED. */
#define PIN_BD_MUTE       28

/* --------------------------------------------------- logical polarity */

/* U14 input select.  De-energised (coil off, GPIO low) selects the DAC. */
#define IN_SEL_DIGITAL    0
#define IN_SEL_ANALOG     1

/* U15 EQ bypass.  De-energised keeps the BD37033 in circuit, so the coil
 * is unpowered during normal listening. */
#define EQ_PATH_ACTIVE    0
#define EQ_PATH_BYPASS    1

/* Q5 sense for the BD37033 mute pin. */
#define BD_MUTE_ON        1
#define BD_MUTE_OFF       0

/* --------------------------------------------------- audio constants  */

/* PGA2320: gain(dB) = 31.5 - 0.5 * (255 - N), N=0 is mute.
 * N = 192 is therefore exactly 0 dB, which is the hardware ceiling the
 * design doc mandates - the positive-gain range is noisy and never
 * needed here. */
#define PGA_CODE_MUTE     0
#define PGA_CODE_MIN      1      /* -95.5 dB */
#define PGA_CODE_0DB      192
#define PGA_CODE_MAX      PGA_CODE_0DB
#define PGA_DB_PER_STEP   0.5f

static inline float pga_code_to_db(int code) {
    return PGA_DB_PER_STEP * (float)(code - PGA_CODE_0DB);
}
static inline int pga_db_to_code(float db) {
    int c = PGA_CODE_0DB + (int)(db / PGA_DB_PER_STEP + (db < 0 ? -0.5f : 0.5f));
    if (c > PGA_CODE_MAX) c = PGA_CODE_MAX;
    if (c < PGA_CODE_MIN) c = PGA_CODE_MIN;
    return c;
}

/* EQ boost headroom.  From the design doc:
 *     max_boost = min(15, -3 - pga_dB)
 * The chain delivers 2.12 V differential at PGA 0 dB against the amp's
 * 3.05 V full output, i.e. 3.15 dB of headroom - so any boost at full
 * volume clips the amplifier, not the preamp.  Floored at 0: we never
 * *force* a cut, we just refuse to boost. */
static inline float eq_max_boost_db(float pga_db) {
    float m = -3.0f - pga_db;
    if (m > 15.0f) m = 15.0f;
    if (m < 0.0f)  m = 0.0f;
    return m;
}

/* Sequencing timings, from the design doc. */
#define T_MAINS_TO_SECONDARY_MS   400
#define T_RAILS_SETTLE_MS         600
#define T_UNMUTE_DELAY_MS        2500
#define T_MUTE_BEFORE_SHUTDOWN_MS 200
#define T_PI_HALT_TIMEOUT_MS    30000
#define T_SECONDARY_TO_MAINS_MS   300

/* Long-press threshold for the encoder button -> settings menu. */
#define T_ENC_LONGPRESS_MS       1200
#define T_PWR_LONGPRESS_MS       1500

#endif /* BOARD_H */

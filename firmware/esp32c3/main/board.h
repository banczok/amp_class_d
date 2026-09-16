/* board.h - pin map for the on-board ESP32-C3 SuperMini.
 *
 * The module sits on +5VA, which is the always-on rail, so this radio is
 * alive whether or not the amplifier is.  That is the whole point of it:
 * it is what lets a battery-powered knob turn the amp on.
 *
 * Wiring, from the amp_ctrl netlist:
 *   Pico GP4 (TX) -> C3 GPIO20 / U0RXD
 *   Pico GP5 (RX) <- C3 GPIO21 / U0TXD
 *
 * UART0 is normally the IDF console on a C3.  sdkconfig.defaults moves
 * the console to the built-in USB Serial/JTAG (GPIO18/19) so UART0 is
 * free for the Pico link and the debug log still works over the USB-C
 * connector.
 */
#ifndef BOARD_H
#define BOARD_H

#include "driver/uart.h"

#define UART_PICO        UART_NUM_0
#define PIN_PICO_RX      20      /* C3 input,  from Pico GP4 */
#define PIN_PICO_TX      21      /* C3 output, to   Pico GP5 */
#define UART_PICO_BAUD   115200

/* The Pico deinits nothing on this link, but in standby it hands its RX
 * pin to the GPIO block and clocks down to 12 MHz, so the byte that wakes
 * it is lost.  Send this many MSG_ESP_WAKE frames first, then wait, then
 * send the real one.  See power.c on the Pico side. */
#define WAKE_PREAMBLE_FRAMES     8
#define WAKE_SETTLE_MS          40

/* If nothing has arrived from the Pico for this long, assume it is
 * asleep and lead with the preamble. */
#define PICO_ASSUMED_ASLEEP_MS 2000

/* Ask the Pico for a state refresh this often when it is awake. */
#define HELLO_INTERVAL_MS      5000

#endif /* BOARD_H */

#ifndef LINKPI_H
#define LINKPI_H
#include <stdint.h>
#include <stdbool.h>
#include "meter.h"
#include "proto.h"

void linkpi_init(void);
void linkpi_poll(void);                 /* drain RX, dispatch commands  */
void linkpi_send_state(void);
void linkpi_send_meter(const meter_result_t *m);
void linkpi_send_menu(void);
void linkpi_send_event(uint8_t kind, uint8_t a, uint8_t b);
void linkpi_log(const char *s);
void linkpi_send_irlearn(uint8_t fn, uint8_t status, uint8_t proto,
                         uint16_t addr, uint16_t cmd);
bool linkpi_pi_alive(void);             /* seen a frame recently?       */

/* Drain the decimated audio ring onto the wire.  Called from the main
 * loop, not from core 1, so a blocking UART write never delays the DSP. */
void linkpi_pump_audio(void);
#endif

#ifndef PICOLINK_H
#define PICOLINK_H
#include <stdint.h>
#include <stdbool.h>

void picolink_init(void);                    /* starts the RX task      */
void picolink_send(uint8_t type, const uint8_t *p, uint8_t n);
bool picolink_pico_awake(void);              /* heard from it recently? */
#endif

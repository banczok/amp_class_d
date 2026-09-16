#ifndef LINKESP_H
#define LINKESP_H
#include <stdint.h>
#include <stdbool.h>
#include "proto.h"

void linkesp_init(void);
void linkesp_poll(void);
void linkesp_send_state(void);
void linkesp_forward_nowplaying(const uint8_t *p, uint8_t n);
bool linkesp_alive(void);
void linkesp_arm_wake(bool en);   /* RX line as a standby wake source */
#endif

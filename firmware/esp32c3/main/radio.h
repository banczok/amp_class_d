#ifndef RADIO_H
#define RADIO_H
#include <stdint.h>
#include <stdbool.h>

void radio_init(void);

/* Push the cached state / now-playing to the knob, if one is paired and
 * we believe it is awake.  Cheap no-ops otherwise. */
void radio_push_state(void);
void radio_push_nowplaying(void);

bool radio_knob_paired(void);
bool radio_wifi_up(void);
void radio_forget_knob(void);
uint8_t radio_channel(void);
#endif

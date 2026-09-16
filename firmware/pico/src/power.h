#ifndef POWER_H
#define POWER_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    POWER_STANDBY = 0,
    POWER_STARTING,
    POWER_ON,
    POWER_STOPPING
} power_state_t;

void          power_init(void);
void          power_tick(void);
void          power_request(power_state_t want);
void          power_toggle(void);
power_state_t power_state(void);
void          power_note_pi_up(void);
bool          power_button_pressed(void);
void          power_standby_sleep(void);
#endif

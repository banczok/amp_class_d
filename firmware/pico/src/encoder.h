#ifndef ENCODER_H
#define ENCODER_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ENC_NONE = 0,
    ENC_CW, ENC_CCW,
    ENC_CLICK,        /* short press released                          */
    ENC_LONG,         /* held past T_ENC_LONGPRESS_MS -> settings menu */
} enc_event_t;

void        encoder_init(void);
enc_event_t encoder_poll(void);     /* call from the main loop          */
int         encoder_take_delta(void);/* accumulated detents since last  */
bool        encoder_button_down(void);
void        encoder_arm_wake(bool en); /* IRQ wake sources for standby  */
/* Any bank-0 edge at all, read-and-clear.  encoder.c owns the single
 * GPIO callback the SDK allows per core, so the standby sleep in
 * power.c asks it whether anything happened rather than installing a
 * second callback and silently unhooking this one. */
bool        encoder_wake_pending(void);
#endif

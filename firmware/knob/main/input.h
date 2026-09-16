/* input.h - the rotary encoder and its push.
 *
 * Hand-rolled rather than pulled in from a component: it is a Gray-code
 * table and two timestamps, it is the same decoder already proven on the
 * Pico side of this project, and it keeps the button semantics - short
 * press wakes, 2 s press is power - in one place where they can be read.
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    IN_NONE = 0,
    IN_ROTATE,      /* value = signed detents since the last poll */
    IN_CLICK,       /* short press, released                      */
    IN_HOLD,        /* held past T_POWER_HOLD_MS, fires once      */
} input_event_t;

void          input_init(void);
input_event_t input_poll(int *value);

bool     input_button_down(void);
uint32_t input_hold_ms(void);      /* 0 when not held; drives the ring */

/* Arm the deep-sleep wake pins and stop the ISRs.  Returns false when
 * nothing could be armed because every candidate is being held down -
 * sleeping then would wake instantly, over and over. */
bool input_prepare_sleep(void);
#endif

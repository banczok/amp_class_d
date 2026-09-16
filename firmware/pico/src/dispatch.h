/* dispatch.h - one place where a function id becomes an action.
 *
 * Implemented in main.c.  Every source of user intent goes through here:
 * a learned IR key, the ESP-NOW knob, the Pi.  Without this the remote
 * paths drift apart - the knob ends up able to do things the remote
 * cannot, or worse, the menu swallows one and not the other.
 */
#ifndef DISPATCH_H
#define DISPATCH_H

#include <stdbool.h>
#include "proto.h"

void dispatch_fn(fn_id_t fn, bool repeat);

/* Signed detents from a rotary source.  Separate from FN_VOL_UP because
 * a knob spun quickly produces a burst, and one radio frame per detent
 * is both slow and pointless. */
void dispatch_volume_delta(int detents);

#endif /* DISPATCH_H */

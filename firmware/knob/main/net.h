/* net.h - WiFi lifecycle for the knob.
 *
 * WiFi here exists for one thing: pulling the cover image. Control does
 * not use it, so association is deliberately deferred until after the
 * first frame is on screen - the user should never wait on a DHCP lease
 * to change the volume.
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

void net_init(void);          /* brings the radio up, does NOT associate */

/* Park on the channel we associated to last time, straight out of NVS.
 * ESP-NOW peers use channel 0 (whatever we are on), so getting this
 * right before the first hello is the difference between an instant
 * response and a retry a second later. */
void net_use_cached_channel(void);

void net_connect(void);       /* async; watch net_is_up()                */
bool net_is_up(void);
void net_stop(void);          /* disconnect and idle the radio for sleep */
#endif

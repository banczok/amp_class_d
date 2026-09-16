/* battery.h - LiPo cell voltage on ADC1_CH3.
 *
 * Sampled once per wake, deliberately early: the reading has to happen
 * before WiFi associates, because a 300 mA transmit burst sags a small
 * cell by 100 mV or more through its own internal resistance, and a
 * gauge that drops ten points every time the cover loads is worse than
 * no gauge.
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include <stdbool.h>

void     battery_init(void);
void     battery_sample(void);     /* blocking, ~1 ms                   */

bool     battery_valid(void);
uint16_t battery_mv(void);         /* at the cell, divider undone       */
uint8_t  battery_percent(void);    /* from a LiPo curve, not a ramp     */
bool     battery_low(void);        /* <= 10%                            */
#endif

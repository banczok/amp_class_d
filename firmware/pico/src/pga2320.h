#ifndef PGA2320_H
#define PGA2320_H
#include <stdint.h>
#include <stdbool.h>

void pga_init(void);                        /* leaves the part muted     */
void pga_write(uint8_t left, uint8_t right);/* raw gain codes            */
void pga_set_mute_pin(bool muted);          /* hardware ~MUTE via U3     */
bool pga_mute_pin(void);
#endif

#ifndef RELAYS_H
#define RELAYS_H
#include <stdint.h>
#include <stdbool.h>

void relays_init(void);            /* all coils off, which is also reset */
void relay_mains(bool on);         /* K3  230 V, 2 pole                  */
void relay_secondary(bool on);     /* K2  T1 2x12 V secondary            */
void relay_input(uint8_t sel);     /* IN_SEL_DIGITAL / IN_SEL_ANALOG     */
void relay_eq_path(uint8_t path);  /* EQ_PATH_ACTIVE / EQ_PATH_BYPASS    */

bool relay_mains_state(void);
bool relay_secondary_state(void);
uint8_t relay_input_state(void);
uint8_t relay_eq_path_state(void);
#endif

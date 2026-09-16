#ifndef MENU_H
#define MENU_H
#include <stdint.h>
#include <stdbool.h>
#include "proto.h"
#include "encoder.h"

typedef enum {
    SCR_CLOSED = 0,
    SCR_ROOT,
    SCR_IR_LIST,      /* pick a function to learn                */
    SCR_IR_WAIT,      /* press a key on the remote               */
    SCR_INPUTS,       /* pick an input to rename                 */
    SCR_NAME_EDIT,    /* character-by-character on the encoder   */
    SCR_PRESETS,
    SCR_PRESET_ACT,   /* load / save / rename / clear            */
    SCR_LEVELS,
    SCR_LEVEL_EDIT
} menu_screen_t;

/* Keys the Pi can inject from the touch panel, same set the encoder
 * generates - the menu does not care which one it came from. */
#define MK_UP    1
#define MK_DOWN  2
#define MK_OK    3
#define MK_BACK  4

void  menu_open(void);
void  menu_close(void);
bool  menu_is_open(void);
void  menu_key(uint8_t k);
void  menu_encoder(enc_event_t e);
void  menu_tick(void);
void  menu_start_ir_learn(fn_id_t fn);
/* Serialise the current screen for the panel.  Returns payload length. */
uint8_t menu_serialise(uint8_t *out, uint8_t max);
menu_screen_t menu_screen(void);
#endif

#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdint.h>
#include <stdbool.h>
#include "irrx.h"
#include "proto.h"

#define IR_MAP_MAX     24
#define PRESET_MAX      4
#define NAME_LEN       16
#define PRESET_NAME_LEN 12

typedef struct { ir_key_t key; uint8_t fn; } ir_map_entry_t;

typedef struct {
    char    name[PRESET_NAME_LEN];
    int8_t  tone_gain[3];
    uint8_t tone_f0[3];
    uint8_t tone_q[3];
    uint8_t loudness;
    uint8_t loudness_f0;
    uint8_t used;
} preset_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t seq;

    char     input_name[2][NAME_LEN];   /* [0] digital, [1] analogue     */
    uint8_t  ir_count;
    ir_map_entry_t ir_map[IR_MAP_MAX];
    preset_t preset[PRESET_MAX];

    uint8_t  startup_vol;               /* PGA code applied on power-on  */
    uint8_t  max_vol;                   /* user ceiling, <= PGA_CODE_MAX */
    int8_t   balance;
    uint8_t  last_input;
    uint8_t  standby_dim;               /* LED brightness in standby     */

    uint32_t crc;
} settings_t;

void        settings_load(void);         /* falls back to defaults       */
bool        settings_save(void);         /* wear-levelled, two slots     */
settings_t *settings(void);
void        settings_defaults(settings_t *s);

/* IR map helpers */
fn_id_t settings_lookup_ir(const ir_key_t *k);
bool    settings_learn_ir(const ir_key_t *k, fn_id_t fn);
void    settings_forget_fn(fn_id_t fn);
#endif

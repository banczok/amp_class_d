#ifndef IRRX_H
#define IRRX_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    IR_PROTO_NONE = 0,
    IR_PROTO_NEC,      /* 8-bit address  */
    IR_PROTO_NEC_EXT,  /* 16-bit address */
    IR_PROTO_RC5,
    IR_PROTO_RC5X,
    IR_PROTO_SIRC12,
    IR_PROTO_SIRC15,
    IR_PROTO_SIRC20,
    IR_PROTO_COUNT
} ir_proto_t;

/* A normalised key.  Learning stores this triple rather than raw
 * timings: it is compact, it survives remote repeat frames, and it means
 * any household remote works without a per-model table. */
typedef struct {
    uint8_t  proto;
    uint16_t addr;
    uint16_t cmd;
} ir_key_t;

void  irrx_init(void);
bool  irrx_poll(ir_key_t *out, bool *repeat);
const char *ir_proto_name(uint8_t p);
#endif

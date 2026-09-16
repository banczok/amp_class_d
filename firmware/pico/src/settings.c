/* settings.c - flash-backed configuration.
 *
 * Two 4 kB sectors at the very top of flash, written alternately.  Each
 * copy carries a sequence number and a CRC; load picks the newest valid
 * one.  That way a power cut during a write can never leave the preamp
 * with no usable configuration - the older slot is still intact.
 *
 * This is what makes the design doc true when it says volume, balance
 * and EQ presets survive a Pi reboot and standby: none of it depends on
 * the Pi being alive.
 */
#include "settings.h"
#include <stdio.h>
#include <stddef.h>
#include "board.h"
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>

#define SET_MAGIC   0x50524D50u   /* "PRMP" */
#define SET_VERSION 1

#define SLOT_SIZE   FLASH_SECTOR_SIZE
#define SLOT_A_OFF  (PICO_FLASH_SIZE_BYTES - 2 * SLOT_SIZE)
#define SLOT_B_OFF  (PICO_FLASH_SIZE_BYTES - 1 * SLOT_SIZE)

static settings_t s_cfg;
static int        s_slot = 0;

static uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; i++)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    }
    return ~c;
}

static uint32_t calc_crc(const settings_t *s)
{
    return crc32((const uint8_t *)s, offsetof(settings_t, crc));
}

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->magic   = SET_MAGIC;
    s->version = SET_VERSION;
    s->seq     = 1;
    strncpy(s->input_name[0], "Digital", NAME_LEN - 1);
    strncpy(s->input_name[1], "Analogue", NAME_LEN - 1);
    s->startup_vol = (uint8_t)pga_db_to_code(-40.0f);
    s->max_vol     = PGA_CODE_MAX;
    s->balance     = 0;
    s->last_input  = IN_SEL_DIGITAL;
    s->standby_dim = 8;
    for (int i = 0; i < PRESET_MAX; i++) {
        snprintf(s->preset[i].name, PRESET_NAME_LEN, "Preset %d", i + 1);
        s->preset[i].tone_f0[0] = 1; s->preset[i].tone_q[0] = 1;
        s->preset[i].tone_f0[1] = 1; s->preset[i].tone_q[1] = 1;
        s->preset[i].tone_f0[2] = 0; s->preset[i].tone_q[2] = 0;
    }
}

static const settings_t *slot_ptr(int n)
{
    return (const settings_t *)(XIP_BASE + (n ? SLOT_B_OFF : SLOT_A_OFF));
}

static bool slot_valid(const settings_t *s)
{
    return s->magic == SET_MAGIC &&
           s->version == SET_VERSION &&
           s->crc == calc_crc(s);
}

void settings_load(void)
{
    const settings_t *a = slot_ptr(0), *b = slot_ptr(1);
    bool va = slot_valid(a), vb = slot_valid(b);

    if (va && vb)      { bool bn = (int16_t)(b->seq - a->seq) > 0;
                         s_cfg = bn ? *b : *a; s_slot = bn ? 1 : 0; }
    else if (va)       { s_cfg = *a; s_slot = 0; }
    else if (vb)       { s_cfg = *b; s_slot = 1; }
    else               { settings_defaults(&s_cfg); s_slot = 1; }

    if (s_cfg.max_vol > PGA_CODE_MAX)     s_cfg.max_vol = PGA_CODE_MAX;
    if (s_cfg.startup_vol > s_cfg.max_vol) s_cfg.startup_vol = s_cfg.max_vol;
}

typedef struct { uint32_t off; const uint8_t *src; } write_job_t;

static void do_write(void *param)
{
    write_job_t *j = (write_job_t *)param;
    flash_range_erase(j->off, SLOT_SIZE);
    flash_range_program(j->off, j->src, ((sizeof(settings_t) + 255) / 256) * 256);
}

bool settings_save(void)
{
    static uint8_t page[((sizeof(settings_t) + 255) / 256) * 256];

    s_cfg.magic   = SET_MAGIC;
    s_cfg.version = SET_VERSION;
    s_cfg.seq++;
    s_cfg.crc     = calc_crc(&s_cfg);

    memset(page, 0xFF, sizeof page);
    memcpy(page, &s_cfg, sizeof s_cfg);

    int next = s_slot ? 0 : 1;            /* alternate, never in place */
    write_job_t job = { next ? SLOT_B_OFF : SLOT_A_OFF, page };

    /* flash_safe_execute parks the other core: core1 runs the meter and
     * would otherwise be executing from XIP while the sector erases. */
    int rc = flash_safe_execute(do_write, &job, 2000);
    if (rc != PICO_OK) return false;
    s_slot = next;
    return true;
}

settings_t *settings(void) { return &s_cfg; }

fn_id_t settings_lookup_ir(const ir_key_t *k)
{
    for (int i = 0; i < s_cfg.ir_count; i++) {
        const ir_map_entry_t *e = &s_cfg.ir_map[i];
        if (e->key.proto == k->proto && e->key.addr == k->addr && e->key.cmd == k->cmd)
            return (fn_id_t)e->fn;
    }
    return FN_NONE;
}

bool settings_learn_ir(const ir_key_t *k, fn_id_t fn)
{
    /* One function per key, and one key per function: re-learning a
     * function replaces its old code rather than accumulating. */
    settings_forget_fn(fn);
    for (int i = 0; i < s_cfg.ir_count; i++) {
        ir_map_entry_t *e = &s_cfg.ir_map[i];
        if (e->key.proto == k->proto && e->key.addr == k->addr && e->key.cmd == k->cmd) {
            e->fn = (uint8_t)fn;
            return true;
        }
    }
    if (s_cfg.ir_count >= IR_MAP_MAX) return false;
    s_cfg.ir_map[s_cfg.ir_count].key = *k;
    s_cfg.ir_map[s_cfg.ir_count].fn  = (uint8_t)fn;
    s_cfg.ir_count++;
    return true;
}

void settings_forget_fn(fn_id_t fn)
{
    for (int i = 0; i < s_cfg.ir_count; ) {
        if (s_cfg.ir_map[i].fn == (uint8_t)fn) {
            s_cfg.ir_map[i] = s_cfg.ir_map[s_cfg.ir_count - 1];
            s_cfg.ir_count--;
        } else i++;
    }
}

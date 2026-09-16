/* menu.c - the settings menu.
 *
 * The menu lives here and is *rendered* elsewhere: the Pico owns the
 * state machine and ships a description of the current screen over
 * UART0, and the Pi sends back four keys from the 8.8" panel.  The
 * encoder produces the same four keys, so the two input paths are
 * interchangeable and nothing below has to know which one is driving.
 * That is deliberate - pull the Pi cable and the encoder still gets you
 * through every screen here.
 *
 * Entered by a long press on the encoder.  Long press again, or BACK
 * from the root, saves and leaves.
 */
#include "menu.h"
#include "board.h"
#include "audio.h"
#include "settings.h"
#include "irrx.h"
#include "linkpi.h"
#include "bd37033.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

static menu_screen_t s_scr      = SCR_CLOSED;
static int           s_cursor   = 0;
static int           s_sub      = 0;   /* 0,1 = input; 2..5 = preset+2  */
static fn_id_t       s_learn_fn = FN_NONE;
static uint32_t      s_learn_t0 = 0;
static char          s_edit[NAME_LEN];
static int           s_edit_pos = 0;
static int           s_edit_len = NAME_LEN - 1;

#define IR_LEARN_TIMEOUT_MS 12000

static const char *k_root[] = {
    "IR remote learning",
    "Input names",
    "EQ presets",
    "Levels",
    "Save and exit"
};
#define ROOT_N ((int)(sizeof k_root / sizeof k_root[0]))

static const char *k_pact[] = { "Load", "Save current", "Rename", "Clear" };
#define PACT_N ((int)(sizeof k_pact / sizeof k_pact[0]))

#define LEVEL_N 3   /* startup volume, max volume, balance */

/* Character set for on-encoder name editing.  Space first so a fresh
 * name starts blank and one click per character is the worst case. */
static const char k_charset[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.+";
#define CHARSET_N ((int)(sizeof k_charset - 1))

/* ------------------------------------------------------------ opening */

void menu_open(void)
{
    s_scr = SCR_ROOT;
    s_cursor = 0;
    linkpi_send_menu();
}

void menu_close(void)
{
    s_scr = SCR_CLOSED;
    linkpi_send_menu();
    linkpi_send_state();
}

bool menu_is_open(void) { return s_scr != SCR_CLOSED; }
menu_screen_t menu_screen(void) { return s_scr; }

void menu_start_ir_learn(fn_id_t fn)
{
    if (fn <= FN_NONE || fn >= FN_COUNT) return;
    s_learn_fn = fn;
    s_learn_t0 = to_ms_since_boot(get_absolute_time());
    s_scr = SCR_IR_WAIT;
    linkpi_send_irlearn((uint8_t)fn, IRLEARN_WAITING, 0, 0, 0);
    linkpi_send_menu();
}

/* ------------------------------------------------------------- cursor */

static int screen_count(void)
{
    switch (s_scr) {
    case SCR_ROOT:       return ROOT_N;
    case SCR_IR_LIST:    return FN_COUNT - 1;      /* everything but FN_NONE */
    case SCR_INPUTS:     return 2;
    case SCR_NAME_EDIT:  return s_edit_len;
    case SCR_PRESETS:    return PRESET_MAX;
    case SCR_PRESET_ACT: return PACT_N;
    case SCR_LEVELS:
    case SCR_LEVEL_EDIT: return LEVEL_N;
    default:             return 1;
    }
}

static void clampc(void)
{
    int n = screen_count();
    if (n < 1) { s_cursor = 0; return; }
    if (s_cursor < 0)  s_cursor = n - 1;   /* wrap - it is a knob */
    if (s_cursor >= n) s_cursor = 0;
}

static void begin_name_edit(const char *initial, int maxlen)
{
    memset(s_edit, 0, sizeof s_edit);
    strncpy(s_edit, initial, (size_t)maxlen);
    s_edit[maxlen] = 0;
    for (int i = 0; i < maxlen; i++)
        if (s_edit[i] == 0) s_edit[i] = ' ';
    s_edit_len = maxlen;
    s_edit_pos = 0;
    s_scr = SCR_NAME_EDIT;
}

static void commit_name(void)
{
    settings_t *st = settings();

    /* Trailing spaces are an artefact of the editor, not the name. */
    for (int i = s_edit_len - 1; i >= 0 && s_edit[i] == ' '; i--) s_edit[i] = 0;

    if (s_sub < 2) {
        strncpy(st->input_name[s_sub], s_edit, NAME_LEN - 1);
        st->input_name[s_sub][NAME_LEN - 1] = 0;
        s_scr = SCR_INPUTS;
        s_cursor = s_sub;
    } else {
        preset_t *p = &st->preset[s_sub - 2];
        strncpy(p->name, s_edit, PRESET_NAME_LEN - 1);
        p->name[PRESET_NAME_LEN - 1] = 0;
        s_scr = SCR_PRESETS;
        s_cursor = s_sub - 2;
    }
    settings_save();
}

/* ------------------------------------------------------------- levels */

static void adjust_level(int dir)
{
    settings_t *st = settings();

    if (s_cursor == 0) {                              /* startup volume */
        int v = (int)st->startup_vol + dir;
        if (v < PGA_CODE_MIN) v = PGA_CODE_MIN;
        if (v > st->max_vol)  v = st->max_vol;
        st->startup_vol = (uint8_t)v;

    } else if (s_cursor == 1) {                       /* max volume     */
        int v = (int)st->max_vol + dir;
        if (v < PGA_CODE_MIN) v = PGA_CODE_MIN;
        if (v > PGA_CODE_MAX) v = PGA_CODE_MAX;       /* 0 dB ceiling   */
        st->max_vol = (uint8_t)v;
        if (st->startup_vol > st->max_vol) st->startup_vol = st->max_vol;
        if (audio_volume_code() > st->max_vol) audio_set_volume(st->max_vol);

    } else {                                          /* balance        */
        int v = (int)st->balance + dir;
        if (v >  24) v =  24;
        if (v < -24) v = -24;
        st->balance = (int8_t)v;
        audio_set_balance(st->balance);               /* audible at once */
    }
}

/* ------------------------------------------------------------- presets */

static void preset_load(preset_t *p)
{
    if (!p->used) return;
    for (int b = 0; b < 3; b++) {
        audio_set_band_shape((bd_band_t)b, p->tone_f0[b], p->tone_q[b]);
        audio_set_tone((bd_band_t)b, p->tone_gain[b]);
    }
    audio_set_loudness(p->loudness, p->loudness_f0);
}

static void preset_store(preset_t *p)
{
    const audio_state_t *a = audio_get();
    for (int b = 0; b < 3; b++) {
        p->tone_gain[b] = a->tone_gain[b];
        p->tone_f0[b]   = a->tone_f0[b];
        p->tone_q[b]    = a->tone_q[b];
    }
    p->loudness    = a->loudness;
    p->loudness_f0 = a->loudness_f0;
    p->used        = 1;
}

/* --------------------------------------------------------- transitions */

static void activate(void)
{
    settings_t *st = settings();

    switch (s_scr) {
    case SCR_ROOT:
        switch (s_cursor) {
        case 0: s_scr = SCR_IR_LIST; s_cursor = 0; break;
        case 1: s_scr = SCR_INPUTS;  s_cursor = 0; break;
        case 2: s_scr = SCR_PRESETS; s_cursor = 0; break;
        case 3: s_scr = SCR_LEVELS;  s_cursor = 0; break;
        default: settings_save(); menu_close(); return;
        }
        break;

    case SCR_IR_LIST:
        menu_start_ir_learn((fn_id_t)(s_cursor + 1));
        return;                                  /* it sent its own menu */

    case SCR_IR_WAIT:
        /* OK while waiting means "clear whatever is bound to this". */
        settings_forget_fn(s_learn_fn);
        settings_save();
        linkpi_send_irlearn((uint8_t)s_learn_fn, IRLEARN_CLEARED, 0, 0, 0);
        s_scr = SCR_IR_LIST;
        break;

    case SCR_INPUTS:
        s_sub = s_cursor;
        begin_name_edit(st->input_name[s_sub], NAME_LEN - 1);
        break;

    case SCR_NAME_EDIT:
        /* Click steps right; clicking off the end commits. */
        if (++s_edit_pos >= s_edit_len) commit_name();
        break;

    case SCR_PRESETS:
        s_sub = s_cursor + 2;
        s_scr = SCR_PRESET_ACT;
        s_cursor = 0;
        break;

    case SCR_PRESET_ACT: {
        preset_t *p = &st->preset[s_sub - 2];
        switch (s_cursor) {
        case 0:
            preset_load(p);
            s_scr = SCR_PRESETS; s_cursor = s_sub - 2;
            break;
        case 1:
            preset_store(p);
            settings_save();
            s_scr = SCR_PRESETS; s_cursor = s_sub - 2;
            break;
        case 2:
            begin_name_edit(p->name, PRESET_NAME_LEN - 1);
            break;
        default:
            memset(p, 0, sizeof *p);
            snprintf(p->name, PRESET_NAME_LEN, "Preset %d", s_sub - 1);
            settings_save();
            s_scr = SCR_PRESETS; s_cursor = s_sub - 2;
            break;
        }
        break;
    }

    case SCR_LEVELS:
        s_scr = SCR_LEVEL_EDIT;                  /* knob now edits value */
        break;

    case SCR_LEVEL_EDIT:
        s_scr = SCR_LEVELS;                      /* knob moves again     */
        settings_save();
        break;

    default:
        break;
    }
    linkpi_send_menu();
}

static void go_back(void)
{
    switch (s_scr) {
    case SCR_ROOT:       settings_save(); menu_close(); return;
    case SCR_IR_LIST:
    case SCR_INPUTS:
    case SCR_PRESETS:
    case SCR_LEVELS:     s_scr = SCR_ROOT;      s_cursor = 0;     break;
    case SCR_IR_WAIT:    s_scr = SCR_IR_LIST;                     break;
    case SCR_NAME_EDIT:
        /* Backspace-ish: step left, and only leave from position 0. */
        if (s_edit_pos > 0) { s_edit_pos--; }
        else                { s_scr = (s_sub < 2) ? SCR_INPUTS : SCR_PRESETS;
                              s_cursor = (s_sub < 2) ? s_sub : s_sub - 2; }
        break;
    case SCR_PRESET_ACT: s_scr = SCR_PRESETS;   s_cursor = s_sub - 2; break;
    case SCR_LEVEL_EDIT: s_scr = SCR_LEVELS;    settings_save();  break;
    default: break;
    }
    linkpi_send_menu();
}

static void move(int dir)
{
    if (s_scr == SCR_LEVEL_EDIT) {
        adjust_level(dir);
    } else if (s_scr == SCR_NAME_EDIT) {
        char c = s_edit[s_edit_pos] ? s_edit[s_edit_pos] : ' ';
        const char *f = strchr(k_charset, c);
        int idx = f ? (int)(f - k_charset) : 0;
        idx = (idx + dir + CHARSET_N) % CHARSET_N;
        s_edit[s_edit_pos] = k_charset[idx];
    } else {
        s_cursor += dir;
        clampc();
    }
    linkpi_send_menu();
}

/* ----------------------------------------------------------- input in */

void menu_key(uint8_t k)
{
    if (s_scr == SCR_CLOSED) return;    /* opening is power.c/main.c job */

    switch (k) {
    case MK_UP:   move(-1); break;
    case MK_DOWN: move(+1); break;
    case MK_OK:   activate(); break;
    case MK_BACK: go_back(); break;
    default: break;
    }
}

void menu_encoder(enc_event_t e)
{
    switch (e) {
    case ENC_CW:    menu_key(MK_DOWN); break;
    case ENC_CCW:   menu_key(MK_UP);   break;
    case ENC_CLICK: menu_key(MK_OK);   break;
    case ENC_LONG:  settings_save(); menu_close(); break;  /* long = out */
    default: break;
    }
}

/* ------------------------------------------------------------ learning */

void menu_tick(void)
{
    if (s_scr != SCR_IR_WAIT) return;

    ir_key_t k;
    bool rpt;
    if (irrx_poll(&k, &rpt)) {
        if (rpt) return;                 /* holding the key is not a press */
        bool ok = settings_learn_ir(&k, s_learn_fn);
        settings_save();
        linkpi_send_irlearn((uint8_t)s_learn_fn,
                            ok ? IRLEARN_OK : IRLEARN_FULL,
                            k.proto, k.addr, k.cmd);
        s_scr = SCR_IR_LIST;
        linkpi_send_menu();
        return;
    }

    if (to_ms_since_boot(get_absolute_time()) - s_learn_t0 > IR_LEARN_TIMEOUT_MS) {
        linkpi_send_irlearn((uint8_t)s_learn_fn, IRLEARN_TIMEOUT, 0, 0, 0);
        s_scr = SCR_IR_LIST;
        linkpi_send_menu();
    }
}

/* --------------------------------------------------------- serialise */
/*
 * Payload: [screen][cursor][count] then count x (len, chars).
 * Deliberately dumb - the panel draws a list and highlights one row, and
 * gets no say in what the rows mean.  Adding a screen here needs no
 * change on the Pi.
 */

static uint8_t put(uint8_t *o, uint8_t max, uint8_t n, const char *s)
{
    uint8_t l = (uint8_t)strlen(s);
    if ((int)n + 1 + (int)l > (int)max) return n;   /* silently truncate */
    o[n++] = l;
    memcpy(&o[n], s, l);
    return (uint8_t)(n + l);
}

uint8_t menu_serialise(uint8_t *out, uint8_t max)
{
    settings_t *st = settings();
    char line[48];
    uint8_t n = 0;
    int cnt = (s_scr == SCR_CLOSED) ? 0 : screen_count();

    out[n++] = (uint8_t)s_scr;
    out[n++] = (uint8_t)((s_scr == SCR_NAME_EDIT) ? s_edit_pos : s_cursor);
    out[n++] = (uint8_t)cnt;

    for (int i = 0; i < cnt; i++) {
        switch (s_scr) {

        case SCR_ROOT:
            n = put(out, max, n, k_root[i]);
            break;

        case SCR_IR_LIST: {
            fn_id_t f = (fn_id_t)(i + 1);
            const ir_key_t *found = NULL;
            for (int j = 0; j < st->ir_count; j++)
                if (st->ir_map[j].fn == (uint8_t)f) { found = &st->ir_map[j].key; break; }
            if (found)
                snprintf(line, sizeof line, "%-11s %s %04X/%02X",
                         fn_name(f), ir_proto_name(found->proto),
                         found->addr, found->cmd & 0xFF);
            else
                snprintf(line, sizeof line, "%-11s --", fn_name(f));
            n = put(out, max, n, line);
            break;
        }

        case SCR_IR_WAIT:
            snprintf(line, sizeof line, "Press a key for %s", fn_name(s_learn_fn));
            n = put(out, max, n, line);
            break;

        case SCR_INPUTS:
            snprintf(line, sizeof line, "%-9s %s",
                     (i == IN_SEL_DIGITAL) ? "Digital" : "Analogue",
                     st->input_name[i]);
            n = put(out, max, n, line);
            break;

        case SCR_NAME_EDIT:
            /* One row per character so the panel can draw a caret and
             * the encoder can walk it without a second cursor concept. */
            line[0] = s_edit[i] ? s_edit[i] : ' ';
            line[1] = 0;
            n = put(out, max, n, line);
            break;

        case SCR_PRESETS:
            snprintf(line, sizeof line, "%-12s%s",
                     st->preset[i].name,
                     st->preset[i].used ? "" : " (empty)");
            n = put(out, max, n, line);
            break;

        case SCR_PRESET_ACT:
            n = put(out, max, n, k_pact[i]);
            break;

        case SCR_LEVELS:
        case SCR_LEVEL_EDIT:
            if (i == 0) {
                snprintf(line, sizeof line, "Startup volume  %+.1f dB",
                         (double)pga_code_to_db(st->startup_vol));
            } else if (i == 1) {
                snprintf(line, sizeof line, "Max volume      %+.1f dB",
                         (double)pga_code_to_db(st->max_vol));
            } else {
                snprintf(line, sizeof line, "Balance         %+.1f dB %s",
                         (double)(0.5f * (float)st->balance),
                         st->balance == 0 ? "" : (st->balance > 0 ? "R" : "L"));
            }
            n = put(out, max, n, line);
            break;

        default:
            break;
        }
    }
    return n;
}

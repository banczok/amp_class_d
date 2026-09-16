/* ui.c - LVGL for a 480x480 round panel.
 *
 * Two constraints shape everything below.
 *
 * It is round.  There are no corners to put things in, and the outer
 * ~40 px disappears under the bezel at an angle, so nothing that has to
 * be read or touched goes near the edge.  The outer ring is used only
 * for the volume arc and the power-hold arc, where being clipped costs
 * nothing.
 *
 * The cover is arbitrary.  Text on top of an unknown photograph is only
 * legible if something guarantees the contrast, so a flat scrim sits
 * between them.  Flat, not blurred: a 480x480 blur is tens of
 * milliseconds per frame on this chip, and it would have to be redone
 * every time the volume overlay faded in over it.
 */
#include "ui.h"
#include "board_knob.h"
#include "link.h"
#include "art.h"
#include "battery.h"
#include "display.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";

#define FONT_HUGE   (&lv_font_montserrat_48)
#define FONT_TITLE  (&lv_font_montserrat_20)
#define FONT_SUB    (&lv_font_montserrat_16)
#define FONT_ICON   (&lv_font_montserrat_28)

#define COL_BG       lv_color_hex(0x000000)
#define COL_TEXT     lv_color_hex(0xF4F2EC)
#define COL_DIM      lv_color_hex(0xB4B2A9)
#define COL_FAINT    lv_color_hex(0x5F5E5A)
#define COL_ACCENT   lv_color_hex(0xEF9F27)
#define COL_POWER    lv_color_hex(0xF0997B)
#define COL_TRACK    lv_color_hex(0x3A3A38)
#define COL_WARN     lv_color_hex(0xE24B4A)

#define SCRIM_OPA    158              /* ~62% */

static ui_mode_t s_mode = UI_PROMPT;
static void (*s_activity_cb)(void);

static lv_obj_t *s_art, *s_scrim;
static lv_obj_t *s_grp_meta, *s_lbl_title, *s_lbl_artist;
static lv_obj_t *s_grp_idle,  *s_lbl_idle_sub;
static lv_obj_t *s_grp_prompt;
static lv_obj_t *s_grp_tport, *s_btn_play_lbl;
static lv_obj_t *s_arc_vol, *s_grp_vol, *s_lbl_vol;
static lv_obj_t *s_arc_hold;
static lv_obj_t *s_lbl_batt;
static lv_obj_t *s_lbl_link;
static lv_obj_t *s_grp_nolink;
static bool      s_linked = true;

static int64_t  s_vol_until;
static uint32_t s_seen_rev = 0xFFFFFFFFu;
static uint8_t  s_seen_art = 0xFF;
static art_state_t s_seen_art_state = ART_FAILED;
static int      s_seen_batt = -1;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

void ui_set_activity_cb(void (*cb)(void)) { s_activity_cb = cb; }
ui_mode_t ui_mode(void) { return s_mode; }

static void note_activity(void) { if (s_activity_cb) s_activity_cb(); }

/* ------------------------------------------------------------ helpers */

static void hide(lv_obj_t *o, bool h)
{
    if (!o) return;
    if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *plain_container(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c,
                       const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, text);
    return l;
}

/* The volume and hold rings share a shape: full circle, thick stroke,
 * no knob, not touchable.  Only the colour and range differ. */
static lv_obj_t *ring(lv_obj_t *parent, lv_color_t indic, int32_t max)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, 462, 462);
    lv_obj_center(a);
    lv_arc_set_rotation(a, 90);            /* start at the bottom       */
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, max);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, indic, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    return a;
}

/* ------------------------------------------------------------ actions */

static void transport_cb(lv_event_t *e)
{
    fn_id_t fn = (fn_id_t)(uintptr_t)lv_event_get_user_data(e);
    note_activity();
    link_send_key(fn, false);
}

static lv_obj_t *transport_btn(lv_obj_t *parent, const char *sym,
                               const lv_font_t *f, lv_color_t c, fn_id_t fn)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    /* 72 px targets.  The glyphs are half that, but a finger on a round
     * panel lands imprecisely and there is nothing else down there to
     * hit by mistake. */
    lv_obj_set_size(b, 72, 72);
    lv_obj_add_event_cb(b, transport_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)fn);

    lv_obj_t *l = label(b, f, c, sym);
    lv_obj_center(l);
    return l;
}

/* -------------------------------------------------------------- build */

void ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- cover, then the scrim that makes text on it legible --- */
    s_art = lv_img_create(scr);
    lv_obj_set_size(s_art, LCD_H_RES, LCD_V_RES);
    lv_obj_center(s_art);
    hide(s_art, true);

    s_scrim = plain_container(scr);
    lv_obj_set_size(s_scrim, LCD_H_RES, LCD_V_RES);
    lv_obj_center(s_scrim);
    lv_obj_set_style_bg_color(s_scrim, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scrim, SCRIM_OPA, 0);
    hide(s_scrim, true);

    /* --- track text, near the top, inside the safe circle --- */
    s_grp_meta = plain_container(scr);
    lv_obj_set_size(s_grp_meta, 368, 96);
    lv_obj_align(s_grp_meta, LV_ALIGN_TOP_MID, 0, 84);

    s_lbl_title = label(s_grp_meta, FONT_TITLE, COL_TEXT, "");
    lv_obj_set_width(s_lbl_title, 368);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 0);

    s_lbl_artist = label(s_grp_meta, FONT_SUB, COL_DIM, "");
    lv_obj_set_width(s_lbl_artist, 368);
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_artist, LV_ALIGN_TOP_MID, 0, 32);

    /* --- nothing playing --- */
    s_grp_idle = plain_container(scr);
    lv_obj_set_size(s_grp_idle, 320, 120);
    lv_obj_center(s_grp_idle);

    lv_obj_t *idle_icon = label(s_grp_idle, FONT_HUGE, COL_FAINT, LV_SYMBOL_AUDIO);
    lv_obj_align(idle_icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *idle_main = label(s_grp_idle, FONT_SUB, COL_DIM, "Nothing playing");
    lv_obj_align(idle_main, LV_ALIGN_TOP_MID, 0, 62);
    s_lbl_idle_sub = label(s_grp_idle, FONT_SUB, COL_FAINT, "");
    lv_obj_align(s_lbl_idle_sub, LV_ALIGN_TOP_MID, 0, 88);

    /* --- power prompt --- */
    s_grp_prompt = plain_container(scr);
    lv_obj_set_size(s_grp_prompt, 320, 130);
    lv_obj_center(s_grp_prompt);

    lv_obj_t *pwr = label(s_grp_prompt, FONT_HUGE, COL_POWER, LV_SYMBOL_POWER);
    lv_obj_align(pwr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *hint = label(s_grp_prompt, FONT_SUB, COL_FAINT, "hold 2 s");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 74);

    /* --- transport --- */
    s_grp_tport = plain_container(scr);
    lv_obj_set_size(s_grp_tport, 300, 80);
    lv_obj_align(s_grp_tport, LV_ALIGN_BOTTOM_MID, 0, -76);
    lv_obj_set_flex_flow(s_grp_tport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_grp_tport, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    transport_btn(s_grp_tport, LV_SYMBOL_PREV, FONT_ICON, COL_DIM, FN_PREV);
    s_btn_play_lbl = transport_btn(s_grp_tport, LV_SYMBOL_PAUSE, FONT_HUGE,
                                   COL_TEXT, FN_PLAY_PAUSE);
    transport_btn(s_grp_tport, LV_SYMBOL_NEXT, FONT_ICON, COL_DIM, FN_NEXT);

    /* --- volume overlay --- */
    s_arc_vol = ring(scr, COL_ACCENT, 192);
    hide(s_arc_vol, true);

    s_grp_vol = plain_container(scr);
    lv_obj_set_size(s_grp_vol, 300, 110);
    lv_obj_center(s_grp_vol);
    s_lbl_vol = label(s_grp_vol, FONT_HUGE, COL_TEXT, "");
    lv_obj_align(s_lbl_vol, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *db = label(s_grp_vol, FONT_SUB, COL_DIM, "dB");
    lv_obj_align(db, LV_ALIGN_TOP_MID, 0, 66);
    hide(s_grp_vol, true);

    /* --- battery gauge --- */
    /* 30 px down, where the round panel still gives ~230 px of chord.
     * Small and quiet: it is reference information, not the point of the
     * screen. */
    s_lbl_batt = label(scr, FONT_SUB, COL_DIM, "");
    lv_obj_align(s_lbl_batt, LV_ALIGN_TOP_MID, 0, 30);
    hide(s_lbl_batt, true);

    /* --- link indicator, just left of the battery --- */
    s_lbl_link = label(scr, FONT_SUB, COL_FAINT, LV_SYMBOL_WIFI);
    lv_obj_align(s_lbl_link, LV_ALIGN_TOP_MID, -52, 30);

    /* --- no link to the amplifier --- */
    /* Its own screen rather than an error badge on a normal one: with no
     * link there is no volume to show, no track, and nothing any control
     * would do.  Drawing the usual furniture would be a lie. */
    s_grp_nolink = plain_container(scr);
    lv_obj_set_size(s_grp_nolink, 340, 150);
    lv_obj_center(s_grp_nolink);

    lv_obj_t *nl_icon = label(s_grp_nolink, FONT_HUGE, COL_WARN, LV_SYMBOL_WARNING);
    lv_obj_align(nl_icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *nl_main = label(s_grp_nolink, FONT_TITLE, COL_TEXT, "No link to amp");
    lv_obj_align(nl_main, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_t *nl_sub = label(s_grp_nolink, FONT_SUB, COL_FAINT, "still trying");
    lv_obj_align(nl_sub, LV_ALIGN_TOP_MID, 0, 96);
    hide(s_grp_nolink, true);

    /* --- power-hold ring, on top of everything --- */
    s_arc_hold = ring(scr, COL_POWER, T_POWER_HOLD_MS);
    hide(s_arc_hold, true);

    ui_set_mode(UI_PROMPT);
}

/* --------------------------------------------------------------- mode */

static void apply_visibility(void)
{
    bool vol = !lv_obj_has_flag(s_grp_vol, LV_OBJ_FLAG_HIDDEN);
    bool have_art = (art_state() == ART_READY);
    bool nolink = (s_mode == UI_NOLINK);

    /* The gauge stands down while the volume ring is up - that overlay
     * wants the screen to itself.  The link indicator is redundant on the
     * no-link screen, which is already about nothing else. */
    hide(s_lbl_batt, vol || !battery_valid());
    hide(s_lbl_link, vol || nolink);
    hide(s_grp_nolink, !nolink);

    if (nolink) {
        hide(s_art, true);
        hide(s_scrim, true);
        hide(s_grp_meta, true);
        hide(s_grp_idle, true);
        hide(s_grp_tport, true);
        hide(s_grp_prompt, true);
        return;
    }

    switch (s_mode) {
    case UI_NOLINK:
        break;                  /* returned above, before this switch */

    case UI_PROMPT:
        /* No cover and no volume overlay here: the amplifier is off, so
         * there is nothing to show a level against. */
        hide(s_art, true);
        hide(s_scrim, true);
        hide(s_grp_meta, true);
        hide(s_grp_idle, true);
        hide(s_grp_tport, true);
        hide(s_grp_prompt, false);
        break;

    case UI_IDLE:
        hide(s_art, true);
        hide(s_scrim, true);
        hide(s_grp_meta, true);
        hide(s_grp_prompt, true);
        hide(s_grp_idle, vol);
        hide(s_grp_tport, true);
        break;

    case UI_PLAYING:
        hide(s_art, !have_art);
        hide(s_scrim, !have_art);
        hide(s_grp_prompt, true);
        hide(s_grp_idle, true);
        hide(s_grp_meta, vol);
        hide(s_grp_tport, vol);
        break;
    }
}

void ui_set_mode(ui_mode_t m)
{
    if (!display_lock(200)) return;
    s_mode = m;
    apply_visibility();
    display_unlock();
}

/* ------------------------------------------------------------ overlay */

void ui_set_link(bool linked)
{
    if (linked == s_linked) return;
    s_linked = linked;
    if (!display_lock(50)) return;
    /* Quiet when it is working, obvious when it is not.  An indicator
     * that shouts while everything is fine gets ignored when it matters. */
    lv_obj_set_style_text_color(s_lbl_link,
                                linked ? COL_FAINT : COL_WARN, 0);
    display_unlock();
}

void ui_flash_volume(void)
{
    s_vol_until = now_ms() + T_VOL_OVERLAY_MS;

    if (!display_lock(50)) return;
    if (s_mode != UI_PROMPT && s_mode != UI_NOLINK) {
        char buf[16];
        float db = link_volume_db();
        snprintf(buf, sizeof buf, "%+.1f", (double)db);
        lv_label_set_text(s_lbl_vol, buf);
        lv_arc_set_value(s_arc_vol, link_amp().vol_code);
        hide(s_arc_vol, false);
        hide(s_grp_vol, false);
        apply_visibility();
    }
    display_unlock();
}

static void volume_expire(void)
{
    if (lv_obj_has_flag(s_grp_vol, LV_OBJ_FLAG_HIDDEN)) return;
    if (now_ms() < s_vol_until) return;

    hide(s_arc_vol, true);
    hide(s_grp_vol, true);
    apply_visibility();
}

void ui_set_hold_progress(uint32_t ms)
{
    if (!display_lock(50)) return;
    if (ms == 0) {
        hide(s_arc_hold, true);
    } else {
        if (ms > T_POWER_HOLD_MS) ms = T_POWER_HOLD_MS;
        lv_arc_set_value(s_arc_hold, (int32_t)ms);
        hide(s_arc_hold, false);
    }
    display_unlock();
}

/* --------------------------------------------------------------- tick */

static void battery_refresh(void)
{
    if (!battery_valid()) return;

    uint8_t pct = battery_percent();
    const char *icon = pct >= 88 ? LV_SYMBOL_BATTERY_FULL :
                       pct >= 63 ? LV_SYMBOL_BATTERY_3    :
                       pct >= 38 ? LV_SYMBOL_BATTERY_2    :
                       pct >= 13 ? LV_SYMBOL_BATTERY_1    :
                                   LV_SYMBOL_BATTERY_EMPTY;
    char buf[24];
    snprintf(buf, sizeof buf, "%s %u%%", icon, pct);
    lv_label_set_text(s_lbl_batt, buf);
    lv_obj_set_style_text_color(s_lbl_batt,
                                battery_low() ? COL_WARN : COL_DIM, 0);
}

static void refresh_content(void)
{
    battery_refresh();

    nowplaying_t np = link_nowplaying();
    amp_state_t  a  = link_amp();

    if (s_mode == UI_PLAYING) {
        lv_label_set_text(s_lbl_title,  np.title[0]  ? np.title  : "Unknown track");
        lv_label_set_text(s_lbl_artist, np.artist[0] ? np.artist : "");
        lv_label_set_text(s_btn_play_lbl,
                          np.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

        if (art_state() == ART_READY && art_loaded_id() == np.art_id)
            lv_img_set_src(s_art, art_image());
    } else if (s_mode == UI_IDLE) {
        char buf[48];
        snprintf(buf, sizeof buf, "%s  %+.1f dB",
                 a.input == 0 ? "Digital" : "Analogue",
                 (double)link_volume_db());
        lv_label_set_text(s_lbl_idle_sub, a.muted ? "Muted" : buf);
    }
    apply_visibility();
}

void ui_tick(void)
{
    uint32_t rev = link_revision();
    art_state_t as = art_state();
    uint8_t aid = art_loaded_id();
    int batt = battery_valid() ? (int)battery_percent() : -1;

    bool changed = (rev != s_seen_rev) || (as != s_seen_art_state) ||
                   (aid != s_seen_art) || (batt != s_seen_batt);

    if (!changed && lv_obj_has_flag(s_grp_vol, LV_OBJ_FLAG_HIDDEN)) return;

    if (!display_lock(50)) return;
    if (changed) {
        s_seen_rev = rev;
        s_seen_art_state = as;
        s_seen_art = aid;
        s_seen_batt = batt;
        refresh_content();
    }
    volume_expire();
    display_unlock();
}

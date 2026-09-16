/* ui.h - the five screen states.
 *
 * One LVGL screen with layers that show and hide, rather than five
 * screens: the album cover has to survive a mode change without being
 * re-decoded, and a 460 kB image is not something to reload because the
 * volume overlay appeared.
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    UI_NOLINK = 0,   /* the amplifier did not answer                  */
    UI_PROMPT,       /* woken, amp is off: power symbol and a hint    */
    UI_IDLE,         /* amp on, nothing playing                       */
    UI_PLAYING,      /* cover, artist, title, transport               */
} ui_mode_t;

void      ui_create(void);
void      ui_set_mode(ui_mode_t m);
ui_mode_t ui_mode(void);

/* Called every main-loop pass; pulls from link.h and art.h and only
 * touches LVGL when something actually changed. */
void ui_tick(void);

void ui_flash_volume(void);            /* knob turned: show the ring   */

/* Small link indicator, left of the battery gauge.  Quiet while the
 * amplifier is answering, obvious the moment it stops. */
void ui_set_link(bool linked);
void ui_set_hold_progress(uint32_t ms); /* 0 clears the hold ring      */

/* The UI is the only thing that sees touch, so it is the only thing
 * that can tell the sleep timer a finger arrived. */
void ui_set_activity_cb(void (*cb)(void));
#endif

/* art.h - the album cover.
 *
 * The Pi serves a pre-scaled 480x480 baseline JPEG.  That is deliberate:
 * the Pi has the CPU to resample an arbitrary cover, and this chip does
 * not want to.  See README for the two endpoints the Pi has to provide.
 */
#ifndef ART_H
#define ART_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

typedef enum { ART_NONE = 0, ART_LOADING, ART_READY, ART_FAILED } art_state_t;

void        art_init(void);
void        art_request(uint8_t art_id);   /* no-op if already loaded   */
art_state_t art_state(void);
uint8_t     art_loaded_id(void);

/* Valid only while art_state() == ART_READY. */
const lv_img_dsc_t *art_image(void);
#endif

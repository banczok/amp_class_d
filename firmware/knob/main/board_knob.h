/* board_knob.h - VIEWE UEDX48480021-MD80ESP32, 2.1" 480x480 round.
 *
 * ESP32-S3-R8: 16 MB flash, 8 MB octal PSRAM.
 * ST7701S over 3-wire SPI for setup, 16-bit parallel RGB for pixels.
 * CST816-class capacitive touch on I2C0.
 * Rotary encoder on GPIO6/5, its push on GPIO0.
 *
 * Every number here comes from the vendor BSP
 * (examples/ESP-IDF/DX48480021ET-WB-A-LVGL/components/bsp/include/bsp/esp-bsp.h),
 * not from a datasheet guess.  The RGB timings in particular are the
 * touch-enabled variant: 18 MHz pixel clock, because the touch
 * controller shares enough of the ground plane that the 20 MHz timing
 * used on the touchless board couples into it.
 */
#ifndef BOARD_KNOB_H
#define BOARD_KNOB_H

#include "driver/gpio.h"
#include "driver/i2c.h"

/* ------------------------------------------------------------ display */

#define LCD_H_RES            480
#define LCD_V_RES            480
#define LCD_PIXEL_CLOCK_HZ   (18 * 1000 * 1000)

#define LCD_HSYNC_PULSE       8
#define LCD_HSYNC_BACK        20
#define LCD_HSYNC_FRONT       40
#define LCD_VSYNC_PULSE       8
#define LCD_VSYNC_BACK        20
#define LCD_VSYNC_FRONT       50

#define PIN_LCD_VSYNC        GPIO_NUM_3
#define PIN_LCD_HSYNC        GPIO_NUM_46
#define PIN_LCD_DE           GPIO_NUM_17
#define PIN_LCD_PCLK         GPIO_NUM_9
#define PIN_LCD_RST          GPIO_NUM_8
#define PIN_LCD_BL           GPIO_NUM_7

/* 16-bit RGB565 bus, in the order the panel expects. */
#define PIN_LCD_DATA0        GPIO_NUM_10   /* B3 */
#define PIN_LCD_DATA1        GPIO_NUM_11   /* B4 */
#define PIN_LCD_DATA2        GPIO_NUM_12   /* B5 */
#define PIN_LCD_DATA3        GPIO_NUM_13   /* B6 */
#define PIN_LCD_DATA4        GPIO_NUM_14   /* B7 */
#define PIN_LCD_DATA5        GPIO_NUM_21   /* G2 */
#define PIN_LCD_DATA6        GPIO_NUM_47   /* G3 */
#define PIN_LCD_DATA7        GPIO_NUM_48   /* G4 */
#define PIN_LCD_DATA8        GPIO_NUM_45   /* G5 */
#define PIN_LCD_DATA9        GPIO_NUM_38   /* G6 */
#define PIN_LCD_DATA10       GPIO_NUM_39   /* G7 */
#define PIN_LCD_DATA11       GPIO_NUM_40   /* R3 */
#define PIN_LCD_DATA12       GPIO_NUM_41   /* R4 */
#define PIN_LCD_DATA13       GPIO_NUM_42   /* R5 */
#define PIN_LCD_DATA14       GPIO_NUM_2    /* R6 */
#define PIN_LCD_DATA15       GPIO_NUM_1    /* R7 */

/* 3-wire SPI, used only for the ST7701S init sequence.  SCK and SDO are
 * shared with DATA3 and DATA2 - the panel tri-states them once the RGB
 * bus starts, which is why the init has to complete first. */
#define PIN_LCD_SPI_CS       GPIO_NUM_18
#define PIN_LCD_SPI_SCK      GPIO_NUM_13
#define PIN_LCD_SPI_SDO      GPIO_NUM_12

/* -------------------------------------------------------------- touch */

#define TOUCH_I2C_PORT       I2C_NUM_0
#define PIN_TOUCH_SCL        GPIO_NUM_15
#define PIN_TOUCH_SDA        GPIO_NUM_16
#define PIN_TOUCH_RST        GPIO_NUM_NC
#define PIN_TOUCH_INT        GPIO_NUM_NC   /* not brought out on this board */

/* ------------------------------------------------------------- inputs */

#define PIN_ENC_A            GPIO_NUM_6
#define PIN_ENC_B            GPIO_NUM_5

/* The knob press.  Shared with BOOT, active low, and - the reason it can
 * be the wake source at all - an RTC GPIO, so ext0 can watch it while
 * the whole digital domain is powered down.  Touch cannot do this: this
 * board does not bring TP_INT out, so the touch controller can only be
 * polled over I2C, which needs the CPU running. */
#define PIN_BTN              GPIO_NUM_0
#define BTN_ACTIVE_LEVEL     0

/* Neither the phases nor the button has an external pull-up: on the
 * vendor schematic PHA/PHB go straight to the encoder contacts and then
 * to ground, and R14/R15 (4k7) are the touch I2C pull-ups, not these.
 * So the internal pull-ups are load-bearing, awake and asleep alike.
 * The debounce footprints C10/C11 on PHA/PHB are marked NC and are not
 * fitted, which is why the Gray-code decoder does all the filtering. */

/* Wake on turning the knob as well as pressing it.
 *
 * Off by default, and not only to avoid knocks waking it: the first
 * detent is unrecoverable.  Quadrature needs two edges to know which way
 * it went and the CPU is not running for the first one, so a turn from
 * cold brings the screen up showing the current volume and only the
 * second detent onward moves it.
 *
 * Cost depends on where your encoder rests.  If its detent leaves both
 * contacts open - most EC11-style parts - this is free.  If a contact
 * rests closed, the internal pull-up (~45 kOhm) burns ~73 uA
 * continuously through it, which dwarfs everything else in the sleep
 * budget.  Measure before switching this on. */
#define WAKE_ON_ROTATION     0

/* ------------------------------------------------------- battery sense */

/* GPIO4 is ADC1_CH3.  ADC1 is not optional here: ADC2 shares its SAR with
 * the WiFi radio and reads fail outright while WiFi is up, which on this
 * device is exactly when you would want a battery reading.
 *
 * Divider from the LiPo positive terminal:
 *
 *     BAT+ ---[ R_TOP ]---+--- GPIO4
 *                         |
 *                      [ R_BOT ]   plus 100 nF to GND at the pin
 *                         |
 *                        GND
 *
 * The cap is not optional either.  The SAR samples onto a small internal
 * capacitor and needs a low source impedance to charge it; anything above
 * about 10 kOhm of Thevenin impedance reads low without a local reservoir
 * to sample against. */
#define PIN_VBAT             GPIO_NUM_4
#define VBAT_ADC_CHANNEL     ADC_CHANNEL_3
#define VBAT_ADC_UNIT        ADC_UNIT_1

/* Divider ratio as a per-mille integer: mV_battery = mV_pin * 1000 / this.
 * 500 is a 1:1 divider (two equal resistors).  Change this and nothing
 * else if you re-pick the resistors. */
#define VBAT_DIVIDER_PERMILLE  500

/* Deliberately NOT scaled to put 4.2 V near 3.0 V at the pin.  The S3's
 * 12 dB attenuation curve compresses toward the top of its ~3.1 V range,
 * so a 1.4:1 divider puts a full battery in the least accurate part of
 * the curve - and leaves no margin if a charger holds the cell above
 * 4.2 V.  A 1:1 divider lands 4.2 V at 2.1 V, in the linear region, and
 * still gives ~800 counts across the usable 3.3-4.2 V range. */

#define VBAT_SAMPLES         32     /* averaged                          */
#define VBAT_WARMUP_SAMPLES   4     /* discarded after the mux switches  */

/* ------------------------------------------------------------ timings */

#define T_POWER_HOLD_MS      2000   /* hold this long = power button      */
#define T_PROMPT_TIMEOUT_MS  6000   /* woken, amp off, nothing pressed    */

/* How long to keep asking before deciding the amplifier is not there.
 * The C3 answers a hello in about 5 ms when it is listening, so this is
 * not about its speed - it is about the cached channel being stale, which
 * costs a retry on another channel. */
#define T_LINK_WAIT_MS        900
#define T_LINK_RETRY_MS       250

/* A wake that found nothing is a wake the user did not get anything out
 * of, so it should cost as little as possible: dimmer, and brief. */
#define T_NOLINK_TIMEOUT_MS  4000
#define BL_NOLINK_PERCENT      22

/* The link is stale rather than dead until this long without a frame.
 *
 * This only works because of the keepalive below: the amplifier pushes
 * state when it changes, not on a schedule, so a quiet minute would look
 * exactly like an unplugged amplifier without something to ask.  The
 * keepalive has to be comfortably shorter than the stale window. */
#define T_LINK_STALE_MS      4000
#define T_LINK_KEEPALIVE_MS  1500
#define T_IDLE_TIMEOUT_MS   10000   /* awake with the amp on, but unused  */
#define T_VOL_OVERLAY_MS     1500   /* how long the volume ring lingers   */
#define T_BL_FADE_MS          200
#define BL_ACTIVE_PERCENT      40   /* bright enough indoors, easy on the
                                     * cell - the backlight is by far the
                                     * largest draw while awake          */

/* Knob detents are batched into one radio frame rather than sent one at
 * a time; this is how long we wait for the burst to end. */
#define T_KNOB_COALESCE_MS     60

#endif /* BOARD_KNOB_H */

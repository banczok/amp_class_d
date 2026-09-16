/* display.c - ST7701S panel bring-up, RGB bus, LVGL and touch.
 *
 * Order matters here and is not negotiable: the panel's 3-wire SPI
 * configuration lines are physically shared with RGB data bits 2 and 3.
 * So the whole ST7701S register sequence has to be bit-banged out first,
 * the two pins released, and only then can the RGB peripheral take the
 * bus.  Create the RGB panel first and the init sequence goes nowhere.
 *
 * The register table below is the vendor's, verbatim, for the
 * touch-equipped variant of this board - including the 18 MHz pixel
 * clock rather than the 20 MHz used on the touchless one.  Panel init
 * sequences are not something to derive from a datasheet: they encode
 * the specific glass, and getting one byte wrong shows up as a tint or a
 * roll that is very hard to trace back.
 */
#include "display.h"
#include "board_knob.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static lv_disp_t             *s_disp;
static uint8_t                s_bl_percent;

/* ---------------------------------------------- 9-bit bit-banged SPI */

#define BB_DELAY_US 10

static void bb_write9(uint16_t data)
{
    for (int n = 0; n < 9; n++) {
        gpio_set_level(PIN_LCD_SPI_SDO, (data & 0x0100) ? 1 : 0);
        data <<= 1;
        gpio_set_level(PIN_LCD_SPI_SCK, 0);
        esp_rom_delay_us(BB_DELAY_US);
        gpio_set_level(PIN_LCD_SPI_SCK, 1);
        esp_rom_delay_us(BB_DELAY_US);
    }
}

/* Bit 8 is the data/command flag: 0 = command, 1 = parameter. */
static void bb_frame(uint16_t word)
{
    gpio_set_level(PIN_LCD_SPI_CS, 0);
    esp_rom_delay_us(BB_DELAY_US);
    bb_write9(word);
    esp_rom_delay_us(BB_DELAY_US);
    gpio_set_level(PIN_LCD_SPI_CS, 1);
    gpio_set_level(PIN_LCD_SPI_SCK, 1);
    gpio_set_level(PIN_LCD_SPI_SDO, 1);
    esp_rom_delay_us(BB_DELAY_US);
}

static void bb_cmd(uint8_t c)  { bb_frame((uint16_t)c); }
static void bb_data(uint8_t d) { bb_frame((uint16_t)d | 0x0100u); }

/* ------------------------------------------------- ST7701S init table */

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;           /* 0xFF terminates the table */
} st7701_cmd_t;

static const st7701_cmd_t k_init[] = {
    { 0xFF, { 0x77, 0x01, 0x00, 0x00, 0x13 }, 5 },
    { 0xEF, { 0x08 }, 1 },
    { 0xFF, { 0x77, 0x01, 0x00, 0x00, 0x10 }, 5 },

    { 0xC0, { 0x3B, 0x00 }, 2 },
    { 0xC1, { 0x0B, 0x02 }, 2 },
    { 0xC2, { 0x07, 0x02 }, 2 },
    { 0xC7, { 0x04 }, 1 },
    { 0xCC, { 0x10 }, 1 },
    { 0xCD, { 0x08 }, 1 },

    { 0xB0, { 0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09,
              0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18 }, 16 },
    { 0xB1, { 0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09,
              0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18 }, 16 },

    { 0xFF, { 0x77, 0x01, 0x00, 0x00, 0x11 }, 5 },
    { 0xB0, { 0x6D }, 1 },
    { 0xB1, { 0x37 }, 1 },
    { 0xB2, { 0x8B }, 1 },
    { 0xB3, { 0x80 }, 1 },
    { 0xB5, { 0x43 }, 1 },
    { 0xB7, { 0x85 }, 1 },
    { 0xB8, { 0x20 }, 1 },
    { 0xC0, { 0x09 }, 1 },
    { 0xC1, { 0x78 }, 1 },
    { 0xC2, { 0x78 }, 1 },
    { 0xD0, { 0x88 }, 1 },

    { 0xE0, { 0x00, 0x00, 0x02 }, 3 },
    { 0xE1, { 0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00,
              0x00, 0x20, 0x20 }, 11 },
    { 0xE2, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x00, 0x00, 0x00 }, 13 },
    { 0xE3, { 0x00, 0x00, 0x11, 0x00 }, 4 },
    { 0xE4, { 0x22, 0x00 }, 2 },
    { 0xE5, { 0x05, 0xEC, 0xF6, 0xCA, 0x07, 0xEE, 0xF6, 0xCA,
              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 16 },
    { 0xE6, { 0x00, 0x00, 0x11, 0x00 }, 4 },
    { 0xE7, { 0x22, 0x00 }, 2 },
    { 0xE8, { 0x06, 0xED, 0xF6, 0xCA, 0x08, 0xEF, 0xF6, 0xCA,
              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 16 },
    { 0xE9, { 0x36, 0x00 }, 2 },
    /* One byte, not seven - the vendor table declares length 1 here and
     * the panel is happy with it.  Left as found on purpose. */
    { 0xEB, { 0x00 }, 1 },
    { 0xED, { 0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xFF, 0x45, 0xFF,
              0xFF, 0x54, 0xFF, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF }, 16 },
    { 0xEF, { 0x08, 0x08, 0x08, 0x45, 0x3F, 0x54 }, 6 },

    { 0xFF, { 0x77, 0x01, 0x00, 0x00, 0x13 }, 5 },
    { 0xE8, { 0x00, 0x0E }, 2 },
    { 0xFF, { 0x77, 0x01, 0x00, 0x00, 0x00 }, 5 },

    { 0x11, { 0x00 }, 1 },                       /* sleep out */

    { 0xFF, { 0x77, 0x01, 0x00, 0x00, 0x13 }, 5 },
    { 0xE8, { 0x00, 0x0C }, 2 },
    { 0xE8, { 0x00, 0x00 }, 2 },
    { 0xFF, { 0x77, 0x01, 0x00, 0x00, 0x00 }, 5 },

    { 0x36, { 0x00 }, 1 },
    { 0x3A, { 0x77 }, 1 },

    { 0x00, { 0x00 }, 0xFF },
};

static void st7701_config(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pin_bit_mask = BIT64(PIN_LCD_SPI_CS) | BIT64(PIN_LCD_SPI_SCK) |
                        BIT64(PIN_LCD_SPI_SDO) | BIT64(PIN_LCD_RST),
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    gpio_set_level(PIN_LCD_RST, 1);
    gpio_set_level(PIN_LCD_SPI_CS, 1);
    gpio_set_level(PIN_LCD_SPI_SCK, 1);
    gpio_set_level(PIN_LCD_SPI_SDO, 1);

    for (int i = 0; k_init[i].len != 0xFF; i++) {
        bb_cmd(k_init[i].cmd);
        for (int j = 0; j < k_init[i].len; j++) bb_data(k_init[i].data[j]);
    }

    vTaskDelay(pdMS_TO_TICKS(120));
    bb_cmd(0x29);                                /* display on */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Hand SCK and SDO back - they are RGB data bits 3 and 2. */
    gpio_reset_pin(PIN_LCD_SPI_SCK);
    gpio_reset_pin(PIN_LCD_SPI_SDO);
}

/* ---------------------------------------------------------- backlight */

#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_RES     LEDC_TIMER_10_BIT

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num       = BL_TIMER,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
    ESP_ERROR_CHECK(ledc_fade_func_install(0));
    s_bl_percent = 0;
}

void display_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)((1023u * percent) / 100u);
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty, T_BL_FADE_MS);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, BL_CHANNEL, LEDC_FADE_NO_WAIT);
    s_bl_percent = percent;
}

void display_sleep(void)
{
    /* Backlight first, then the panel: the other order shows a frame of
     * whatever the panel does on the way down. */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
    s_bl_percent = 0;
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
}

/* --------------------------------------------------------------- init */

static esp_err_t touch_init(void)
{
    const i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, i2c_cfg.mode, 0, 0, 0));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_I2C_PORT,
                                             &tp_io_cfg, &tp_io));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,   /* NC on this board: polled */
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp));

    const lvgl_port_touch_cfg_t touch_cfg = { .disp = s_disp, .handle = tp };
    return lvgl_port_add_touch(&touch_cfg) ? ESP_OK : ESP_FAIL;
}

esp_err_t display_init(void)
{
    backlight_init();
    st7701_config();

    const esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .psram_trans_align = 64,
        .data_width = 16,
        .bits_per_pixel = 16,
        .de_gpio_num    = PIN_LCD_DE,
        .pclk_gpio_num  = PIN_LCD_PCLK,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .disp_gpio_num  = GPIO_NUM_NC,
        .data_gpio_nums = {
            PIN_LCD_DATA0,  PIN_LCD_DATA1,  PIN_LCD_DATA2,  PIN_LCD_DATA3,
            PIN_LCD_DATA4,  PIN_LCD_DATA5,  PIN_LCD_DATA6,  PIN_LCD_DATA7,
            PIN_LCD_DATA8,  PIN_LCD_DATA9,  PIN_LCD_DATA10, PIN_LCD_DATA11,
            PIN_LCD_DATA12, PIN_LCD_DATA13, PIN_LCD_DATA14, PIN_LCD_DATA15,
        },
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = LCD_HSYNC_PULSE,
            .hsync_back_porch  = LCD_HSYNC_BACK,
            .hsync_front_porch = LCD_HSYNC_FRONT,
            .vsync_pulse_width = LCD_VSYNC_PULSE,
            .vsync_back_porch  = LCD_VSYNC_BACK,
            .vsync_front_porch = LCD_VSYNC_FRONT,
            .flags.pclk_active_neg = false,
        },
        .num_fbs = 2,
        /* A bounce buffer keeps the RGB DMA fed out of internal RAM even
         * while PSRAM is busy with a JPEG decode.  Without it, decoding
         * the cover tears the whole screen. */
        .bounce_buffer_size_px = LCD_H_RES * 30,
        .flags.fb_in_psram = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rgb_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    const lvgl_port_cfg_t lv_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lv_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = s_panel,
        .buffer_size  = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = false, .buff_spiram = true },
    };
    const lvgl_port_display_rgb_cfg_t rgb_port_cfg = {
        .flags = { .bb_mode = true, .avoid_tearing = true },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_port_cfg);
    if (!s_disp) return ESP_FAIL;

    ESP_ERROR_CHECK(touch_init());

    ESP_LOGI(TAG, "panel up, %dx%d at %d MHz",
             LCD_H_RES, LCD_V_RES, LCD_PIXEL_CLOCK_HZ / 1000000);
    return ESP_OK;
}

lv_disp_t *display_lv(void) { return s_disp; }

bool display_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void display_unlock(void)              { lvgl_port_unlock(); }

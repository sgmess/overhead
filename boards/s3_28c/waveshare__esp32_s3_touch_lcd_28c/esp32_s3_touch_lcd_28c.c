/*
 * Board Support Package for the Waveshare ESP32-S3-Touch-LCD-2.8C.
 *
 * Keeps the bsp_* API of the ESP32-S3-Touch-AMOLED-1.43 BSP this replaces, so the application
 * does not know which board it is on beyond the panel size. Underneath almost everything
 * differs: an ST7701 IPS on the S3's parallel RGB peripheral instead of a CO5300 on QSPI, a
 * TCA9554 IO expander holding the panel's reset and chip select, a GT911 instead of an FT3168,
 * and a PWM backlight instead of a brightness register.
 *
 * The ST7701 init sequence, pin map and RGB porches are the Waveshare factory demo's, and match
 * the capsule-radar port where they run on this board.
 */

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp32_s3_touch_lcd_28c.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "bsp_err_check.h"

static const char *TAG = "ESP32-S3-Touch-LCD-2.8C";

static i2c_master_bus_handle_t i2c_handle = NULL;
static bool i2c_initialized = false;
static i2c_master_dev_handle_t expander_handle = NULL;
static uint8_t expander_outputs = 0;
static lv_indev_t *disp_indev = NULL;
static esp_lcd_touch_handle_t tp = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static bool backlight_initialized = false;
static int brightness = 0;

#define BSP_LEDC_TIMER      (LEDC_TIMER_0)
#define BSP_LEDC_CHANNEL    (LEDC_CHANNEL_0)
#define BSP_LEDC_RESOLUTION (LEDC_TIMER_10_BIT)
#define BSP_LEDC_MAX_DUTY   ((1 << 10) - 1)
/* The vendor demo's PWM rate: above hearing range, so the backlight does not whine. */
#define BSP_LEDC_FREQ_HZ    (20000)

#define TCA9554_REG_OUTPUT  (0x01)
#define TCA9554_REG_CONFIG  (0x03)

/*
 * ST7701 bring-up sequence, verbatim from the vendor demo: {command, length, data...} records
 * back to back. 0x11 (sleep out) needs 120 ms before the next command, which the player below
 * inserts.
 */
static const uint8_t st7701_init_seq[] = {
    0xFF,  5, 0x77, 0x01, 0x00, 0x00, 0x13,
    0xEF,  1, 0x08,
    0xFF,  5, 0x77, 0x01, 0x00, 0x00, 0x10,
    0xC0,  2, 0x3B, 0x00,
    0xC1,  2, 0x10, 0x0C,
    0xC2,  2, 0x07, 0x0A,
    0xC7,  1, 0x00,
    0xCC,  1, 0x10,
    0xCD,  1, 0x08,
    0xB0, 16, 0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09, 0x09, 0x23, 0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11,
    0xB1, 16, 0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08, 0x08, 0x22, 0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F,
    0xFF,  5, 0x77, 0x01, 0x00, 0x00, 0x11,
    0xB0,  1, 0x5D,
    0xB1,  1, 0x3E,
    0xB2,  1, 0x81,
    0xB3,  1, 0x80,
    0xB5,  1, 0x4E,
    0xB7,  1, 0x85,
    0xB8,  1, 0x20,
    0xC1,  1, 0x78,
    0xC2,  1, 0x78,
    0xD0,  1, 0x88,
    0xE0,  3, 0x00, 0x00, 0x02,
    0xE1, 11, 0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07, 0x30, 0x00, 0x33, 0x33,
    0xE2, 12, 0x11, 0x11, 0x33, 0x33, 0xF4, 0x00, 0x00, 0x00, 0xF4, 0x00, 0x00, 0x00,
    0xE3,  4, 0x00, 0x00, 0x11, 0x11,
    0xE4,  2, 0x44, 0x44,
    0xE5, 16, 0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0, 0x09, 0xF1, 0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0,
    0xE6,  4, 0x00, 0x00, 0x11, 0x11,
    0xE7,  2, 0x44, 0x44,
    0xE8, 16, 0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0, 0x08, 0xF0, 0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0,
    0xE9,  2, 0x36, 0x01,
    0xEB,  7, 0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40,
    0xED, 16, 0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF, 0xFF, 0xFC, 0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF,
    0xEF,  6, 0x08, 0x08, 0x08, 0x45, 0x3F, 0x54,
    0xFF,  5, 0x77, 0x01, 0x00, 0x00, 0x00,
    0x11,  0,
    0x3A,  1, 0x66,
    0x36,  1, 0x00,
    0x35,  1, 0x00,
    0x29,  0,
};

/**************************************************************************************************
 *
 * I2C
 *
 **************************************************************************************************/

esp_err_t bsp_i2c_init(void)
{
    if (i2c_initialized) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_config, &i2c_handle));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
    i2c_handle = NULL;
    i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    bsp_i2c_init();
    return i2c_handle;
}

/**************************************************************************************************
 *
 * TCA9554 IO expander
 *
 * Two registers are all this needs, so it is driven directly rather than through a registry
 * component. The output byte is shadowed here, which turns every pin change into one write
 * instead of a read-modify-write on a bus that touch is polling at the same time.
 *
 **************************************************************************************************/

static esp_err_t expander_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(expander_handle, buf, sizeof(buf), 100);
}

static esp_err_t bsp_io_expander_init(void)
{
    if (expander_handle) {
        return ESP_OK;
    }

    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_I2C_ADDR_EXPANDER,
        .scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_handle, &dev_cfg, &expander_handle),
                        TAG, "Expander add failed");

    /*
     * Levels first, direction second. The TCA9554 resets with every output register bit high,
     * and the buzzer is active high, so switching the pins to outputs before writing the levels
     * sounds the buzzer for as long as that gap lasts. Both resets start released and the panel
     * chip select starts deselected; the buzzer stays off for good.
     */
    expander_outputs = (1 << BSP_EXIO_LCD_RST) | (1 << BSP_EXIO_TOUCH_RST) | (1 << BSP_EXIO_LCD_CS);
    ESP_RETURN_ON_ERROR(expander_write(TCA9554_REG_OUTPUT, expander_outputs), TAG,
                        "TCA9554 not answering at 0x%02x", BSP_I2C_ADDR_EXPANDER);
    ESP_RETURN_ON_ERROR(expander_write(TCA9554_REG_CONFIG, 0x00), TAG, "Expander direction failed");

    return ESP_OK;
}

esp_err_t bsp_io_expander_set(uint8_t bit, bool level)
{
    ESP_RETURN_ON_FALSE(bit < 8, ESP_ERR_INVALID_ARG, TAG, "Expander bit %u out of range", bit);
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Expander init failed");

    const uint8_t next = level ? (expander_outputs | (1 << bit)) : (expander_outputs & ~(1 << bit));
    ESP_RETURN_ON_ERROR(expander_write(TCA9554_REG_OUTPUT, next), TAG, "Expander write failed");
    expander_outputs = next;
    return ESP_OK;
}

/**************************************************************************************************
 *
 * Brightness
 *
 * The IPS panel has a real backlight, PWM'd on one LEDC channel. Duty 0 is dark.
 *
 **************************************************************************************************/

esp_err_t bsp_display_brightness_init(void)
{
    if (backlight_initialized) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BSP_LEDC_RESOLUTION,
        .timer_num = BSP_LEDC_TIMER,
        .freq_hz = BSP_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "Backlight timer failed");

    /* Start dark; bsp_display_backlight_on() or the application's brightness turns it up. */
    const ledc_channel_config_t channel_cfg = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BSP_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "Backlight channel failed");

    backlight_initialized = true;
    brightness = 0;
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (!backlight_initialized) {
        ESP_LOGE(TAG, "Backlight is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (brightness_percent < 0 || brightness_percent > 100) {
        ESP_LOGE(TAG, "Invalid brightness percentage. Should be between 0 and 100.");
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t duty = (uint32_t)(brightness_percent * BSP_LEDC_MAX_DUTY + 50) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LEDC_CHANNEL, duty), TAG, "Set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LEDC_CHANNEL), TAG, "Update duty failed");
    brightness = brightness_percent;
    return ESP_OK;
}

int bsp_display_brightness_get(void)
{
    if (!backlight_initialized) {
        ESP_LOGE(TAG, "Backlight is not initialized");
        return -1;
    }

    return brightness;
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

/**************************************************************************************************
 *
 * Display
 *
 **************************************************************************************************/

/*
 * Send the ST7701 init sequence over its 9-bit 3-wire SPI. The ninth bit is D/C and leads each
 * word, so the device is set up with one command bit (D/C) and eight address bits (the byte),
 * and no data phase at all. Chip select is an expander pin, so the SPI driver never touches it.
 *
 * The bus is released afterwards. Nothing talks to the panel again once the RGB scanout starts,
 * and GPIO 1 and 2 have no other role on this board.
 */
static esp_err_t st7701_send_init_sequence(void)
{
    const spi_bus_config_t buscfg = {
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = BSP_LCD_SPI_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 64,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_DISABLED), TAG, "SPI bus init failed");

    const spi_device_interface_config_t devcfg = {
        .command_bits = 1,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 10 * 1000 * 1000,
        .spics_io_num = GPIO_NUM_NC,
        .queue_size = 1,
    };
    spi_device_handle_t spi = NULL;
    esp_err_t ret = spi_bus_add_device(BSP_LCD_SPI_NUM, &devcfg, &spi);
    if (ret != ESP_OK) {
        spi_bus_free(BSP_LCD_SPI_NUM);
        ESP_LOGE(TAG, "SPI device add failed");
        return ret;
    }

    ret = bsp_io_expander_set(BSP_EXIO_LCD_CS, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    for (size_t i = 0; ret == ESP_OK && i < sizeof(st7701_init_seq);) {
        const uint8_t cmd = st7701_init_seq[i++];
        const uint8_t len = st7701_init_seq[i++];
        spi_transaction_t t = {.cmd = 0, .addr = cmd};
        ret = spi_device_polling_transmit(spi, &t);
        for (uint8_t n = 0; ret == ESP_OK && n < len; ++n) {
            spi_transaction_t d = {.cmd = 1, .addr = st7701_init_seq[i++]};
            ret = spi_device_polling_transmit(spi, &d);
        }
        if (cmd == 0x11) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }
    esp_err_t cs_ret = bsp_io_expander_set(BSP_EXIO_LCD_CS, true);

    spi_bus_remove_device(spi);
    spi_bus_free(BSP_LCD_SPI_NUM);

    ESP_RETURN_ON_ERROR(ret, TAG, "ST7701 init sequence failed");
    return cs_ret;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    assert(config != NULL);

    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "IO expander init failed");

    ESP_LOGI(TAG, "Reset ST7701 and send init sequence");
    ESP_RETURN_ON_ERROR(bsp_io_expander_set(BSP_EXIO_LCD_RST, false), TAG, "Panel reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(bsp_io_expander_set(BSP_EXIO_LCD_RST, true), TAG, "Panel reset failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(st7701_send_init_sequence(), TAG, "Panel init failed");

    ESP_LOGI(TAG, "Create RGB panel");
    esp_lcd_rgb_panel_config_t rgb_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
            .h_res = BSP_LCD_H_RES,
            .v_res = BSP_LCD_V_RES,
            .hsync_pulse_width = BSP_LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = BSP_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = BSP_LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = BSP_LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = BSP_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = BSP_LCD_VSYNC_FRONT_PORCH,
        },
        .data_width = 16,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        /* As many as the adapter's tear-avoid mode needs: it takes these over as LVGL's buffers. */
        .num_fbs = esp_lv_adapter_get_required_frame_buffer_count(config->tear_avoid_mode, config->rotation),
        /*
         * The scanout never reads PSRAM directly. The CPU copies each stretch of the frame into
         * one of two internal bounce buffers and the DMA feeds the panel from those, so PSRAM
         * traffic from WiFi, LWIP and LVGL's canvases can delay a copy without starving the
         * bus mid-line. Each buffer costs CONFIG_BSP_LCD_RGB_BOUNCE_LINES * 960 bytes of
         * internal RAM, and there are two.
         */
        .bounce_buffer_size_px = BSP_LCD_H_RES * CONFIG_BSP_LCD_RGB_BOUNCE_LINES,
        .dma_burst_size = 64,
        .hsync_gpio_num = BSP_LCD_HSYNC,
        .vsync_gpio_num = BSP_LCD_VSYNC,
        .de_gpio_num = BSP_LCD_DE,
        .pclk_gpio_num = BSP_LCD_PCLK,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {
            BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
            BSP_LCD_DATA4, BSP_LCD_DATA5, BSP_LCD_DATA6, BSP_LCD_DATA7,
            BSP_LCD_DATA8, BSP_LCD_DATA9, BSP_LCD_DATA10, BSP_LCD_DATA11,
            BSP_LCD_DATA12, BSP_LCD_DATA13, BSP_LCD_DATA14, BSP_LCD_DATA15,
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&rgb_config, &panel_handle), TAG, "RGB panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "RGB panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "RGB panel init failed");

    if (ret_panel) {
        *ret_panel = panel_handle;
    }
    if (ret_io) {
        *ret_io = NULL;
    }

    return ESP_OK;
}

esp_err_t bsp_touch_new(const bsp_display_cfg_t *cfg, esp_lcd_touch_handle_t *ret_touch)
{
    assert(cfg != NULL);

    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "IO expander init failed");

    /*
     * The GT911 latches its I2C address from INT while reset is released: INT low selects
     * 0x5D. Its reset is on the expander, where esp_lcd_touch cannot reach it, so the strap is
     * done here and the driver is told there is no reset line. INT is released to an input
     * afterwards and left unused; the adapter polls.
     */
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << BSP_LCD_TOUCH_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "Touch INT config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_TOUCH_INT, 0), TAG, "Touch INT low failed");
    ESP_RETURN_ON_ERROR(bsp_io_expander_set(BSP_EXIO_TOUCH_RST, false), TAG, "Touch reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(bsp_io_expander_set(BSP_EXIO_TOUCH_RST, true), TAG, "Touch reset failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(gpio_set_direction(BSP_LCD_TOUCH_INT, GPIO_MODE_INPUT), TAG, "Touch INT release failed");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = cfg->touch_flags.swap_xy,
            .mirror_x = cfg->touch_flags.mirror_x,
            .mirror_y = cfg->touch_flags.mirror_y,
        },
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.dev_addr = BSP_I2C_ADDR_TOUCH;
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "Touch IO init failed");

    return esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);

    const bsp_display_config_t disp_config = {
        .tear_avoid_mode = cfg->tear_avoid_mode,
        .rotation = cfg->rotation,
    };

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&disp_config, &panel_handle, NULL));

    ESP_LOGD(TAG, "Add LCD screen");
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel_handle,
        .panel_io = NULL,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_RGB,
            .rotation = cfg->rotation,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT,
            /*
             * LVGL renders into internal RAM and only the finished strip is copied to the PSRAM
             * frame buffer. Rendering straight into PSRAM would add blend read-modify-write
             * traffic to the bus the scanout depends on, which is what makes the picture shift.
             */
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = cfg->tear_avoid_mode,
    };

    return esp_lv_adapter_register_display(&disp_cfg);
}

static lv_indev_t *bsp_display_indev_init(const bsp_display_cfg_t *cfg, lv_display_t *disp)
{
    assert(cfg != NULL);

    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(cfg, &tp));
    assert(tp);

    const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);

    return esp_lv_adapter_register_touch(&touch_cfg);
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        /*
         * Raw GT911 coordinates already match the panel orientation on this board: no swap,
         * no mirror. Verified 29 Sep 2026 with a crosshair tracking a fingertip (TD_TOUCH_TEST)
         * and the logged coordinates at centre and the four edges -- see HARDWARE.md.
         */
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg)
{
    lv_display_t *disp;

    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(esp_lv_adapter_init(&cfg->lv_adapter_cfg));

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());

    /* Display before touch, as on the vendor demo: the panel reset is the first expander use. */
    BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);
    BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(cfg, disp), NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

esp_err_t bsp_display_lock(uint32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
}

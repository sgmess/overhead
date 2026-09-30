#pragma once
#include "esp_lcd_types.h"
#include "esp_err.h"
#include "esp_lv_adapter.h"

/* LCD color formats */
#define ESP_LCD_COLOR_FORMAT_RGB565    (1)
#define ESP_LCD_COLOR_FORMAT_RGB888    (2)

/* LCD display color format */
#define BSP_LCD_COLOR_FORMAT        (ESP_LCD_COLOR_FORMAT_RGB565)
/* LCD display color bytes endianess */
#define BSP_LCD_BIGENDIAN           (0)
/* LCD display color bits */
#define BSP_LCD_BITS_PER_PIXEL      (16)
/* LCD display color space */
#define BSP_LCD_COLOR_SPACE         (LCD_RGB_ELEMENT_ORDER_RGB)

/* LCD display definition. Single fixed 480x480 round IPS panel; no column or row offset. */
#define BSP_LCD_H_RES              (480)
#define BSP_LCD_V_RES              (480)

/*
 * RGB timing. The porches are the vendor demo's. The pixel clock is not: the demo's 30 MHz runs
 * the panel at ~107 Hz and saturates PSRAM, and once network traffic joins the scanout the
 * bounce buffers underrun and the picture drifts sideways. The capsule-radar port on this board
 * ran clean for weeks at 16 MHz (~57 Hz), and found 18 MHz marginal.
 */
#define BSP_LCD_PIXEL_CLOCK_HZ     (CONFIG_BSP_LCD_RGB_PCLK_MHZ * 1000 * 1000)
#define BSP_LCD_HSYNC_PULSE_WIDTH  (8)
#define BSP_LCD_HSYNC_BACK_PORCH   (10)
#define BSP_LCD_HSYNC_FRONT_PORCH  (50)
#define BSP_LCD_VSYNC_PULSE_WIDTH  (2)
#define BSP_LCD_VSYNC_BACK_PORCH   (18)
#define BSP_LCD_VSYNC_FRONT_PORCH  (8)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BSP display configuration structure
 */
typedef struct {
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode; /*!< Decides how many frame buffers the RGB panel allocates */
    esp_lv_adapter_rotation_t        rotation;
} bsp_display_config_t;

/**
 * @brief Create new display panel
 *
 * Resets the ST7701, sends its init sequence over the 3-wire SPI and creates the RGB panel.
 * The returned IO handle is always NULL: after init the panel has no command channel.
 *
 * @param[in]  config    display configuration
 * @param[out] ret_panel esp_lcd panel handle
 * @param[out] ret_io    esp_lcd IO handle (set to NULL)
 */
esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io);

/**
 * @brief Initialize display's brightness
 *
 * Sets up the LEDC channel that PWMs the backlight.
 */
esp_err_t bsp_display_brightness_init(void);

/**
 * @brief Set display's brightness
 *
 * @param[in] brightness_percent Brightness in [%]
 */
esp_err_t bsp_display_brightness_set(int brightness_percent);

/**
 * @brief Get display's brightness in [%]
 */
int bsp_display_brightness_get(void);

/**
 * @brief Turn the backlight on at full brightness
 */
esp_err_t bsp_display_backlight_on(void);

/**
 * @brief Turn the backlight off
 */
esp_err_t bsp_display_backlight_off(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "bsp/config.h"
#include "bsp/display.h"

#include "lvgl.h"
#include "esp_lv_adapter.h"

/**************************************************************************************************
 *  BSP Capabilities
 *
 *  The 2.8C carries a buzzer rather than an audio codec, and a microSD slot this firmware does
 *  not use. Neither is exposed here. The buzzer is held off (see the expander notes below);
 *  whether a traffic display should ever sound it is a design question, not a BSP one.
 **************************************************************************************************/

#define BSP_CAPS_DISPLAY        1
#define BSP_CAPS_TOUCH          1
#define BSP_CAPS_BUTTONS        0
#define BSP_CAPS_AUDIO          0
#define BSP_CAPS_AUDIO_SPEAKER  0
#define BSP_CAPS_AUDIO_MIC      0
#define BSP_CAPS_SDCARD         0
#define BSP_CAPS_IMU            0

/**************************************************************************************************
 * Waveshare ESP32-S3-Touch-LCD-2.8C pinout
 *
 * Pins come from the Waveshare factory demo for this board (ESP32-S3-Touch-LCD-2.8C-Demo.zip:
 * Display_ST7701.h, I2C_Driver.h, Touch_GT911.h, TCA9554PWR.h) and match the capsule-radar port,
 * where the same map runs on hardware.
 *
 * The panel is an ST7701 with no GRAM. The S3's LCD peripheral scans a whole RGB565 frame out of
 * PSRAM continuously over a 16-bit parallel bus; the ST7701 only takes its init sequence over a
 * separate 9-bit 3-wire SPI, whose chip select and reset are on the TCA9554 IO expander rather
 * than on GPIOs.
 **************************************************************************************************/

/* I2C - shared by the TCA9554 expander, touch (GT911), IMU (QMI8658) and RTC (PCF85063) */
#define BSP_I2C_SCL           (GPIO_NUM_7)
#define BSP_I2C_SDA           (GPIO_NUM_15)

/* ST7701 3-wire SPI, used only for the init sequence. CS and reset are expander pins, below. */
#define BSP_LCD_SPI_SCLK      (GPIO_NUM_2)
#define BSP_LCD_SPI_MOSI      (GPIO_NUM_1)

/* 16-bit parallel RGB565 bus */
#define BSP_LCD_HSYNC         (GPIO_NUM_38)
#define BSP_LCD_VSYNC         (GPIO_NUM_39)
#define BSP_LCD_DE            (GPIO_NUM_40)
#define BSP_LCD_PCLK          (GPIO_NUM_41)
#define BSP_LCD_DATA0         (GPIO_NUM_5)
#define BSP_LCD_DATA1         (GPIO_NUM_45)
#define BSP_LCD_DATA2         (GPIO_NUM_48)
#define BSP_LCD_DATA3         (GPIO_NUM_47)
#define BSP_LCD_DATA4         (GPIO_NUM_21)
#define BSP_LCD_DATA5         (GPIO_NUM_14)
#define BSP_LCD_DATA6         (GPIO_NUM_13)
#define BSP_LCD_DATA7         (GPIO_NUM_12)
#define BSP_LCD_DATA8         (GPIO_NUM_11)
#define BSP_LCD_DATA9         (GPIO_NUM_10)
#define BSP_LCD_DATA10        (GPIO_NUM_9)
#define BSP_LCD_DATA11        (GPIO_NUM_46)
#define BSP_LCD_DATA12        (GPIO_NUM_3)
#define BSP_LCD_DATA13        (GPIO_NUM_8)
#define BSP_LCD_DATA14        (GPIO_NUM_18)
#define BSP_LCD_DATA15        (GPIO_NUM_17)

/* Backlight: LEDC PWM, active high. The IPS panel has no brightness register of its own. */
#define BSP_LCD_BACKLIGHT     (GPIO_NUM_6)

/*
 * GT911 touch. Its reset is on the expander; INT is a GPIO but doubles as the address strap
 * while reset is released -- held low there, the chip answers at 0x5D. After that the adapter
 * polls the chip, so INT is not used as an interrupt.
 */
#define BSP_LCD_TOUCH_INT     (GPIO_NUM_16)

/* Divided battery voltage (x3 per the vendor demo). Not read by this firmware. */
#define BSP_BAT_ADC           (GPIO_NUM_4)

#define BSP_BOOT_BUTTON       (GPIO_NUM_0)

/*
 * TCA9554 IO expander bits, numbered 0..7 (the vendor schematic's EXIO1..EXIO8 minus one).
 * All eight are outputs. The buzzer is active high, and the chip powers up with every output
 * high, so the BSP writes the output register before it switches the pins to outputs -- the
 * other order chirps the buzzer at every boot.
 */
#define BSP_EXIO_LCD_RST      (0)
#define BSP_EXIO_TOUCH_RST    (1)
#define BSP_EXIO_LCD_CS       (2)
#define BSP_EXIO_BUZZER       (7)

/* I2C addresses of the devices on the shared bus. */
#define BSP_I2C_ADDR_EXPANDER (0x20)
#define BSP_I2C_ADDR_TOUCH    (0x5D)
#define BSP_I2C_ADDR_IMU      (0x6B)
#define BSP_I2C_ADDR_RTC      (0x51)

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************************
 *
 * I2C interface
 *
 **************************************************************************************************/
#define BSP_I2C_NUM     CONFIG_BSP_I2C_NUM

/**
 * @brief Init I2C driver
 */
esp_err_t bsp_i2c_init(void);

/**
 * @brief Deinit I2C driver and free its resources
 */
esp_err_t bsp_i2c_deinit(void);

/**
 * @brief Get I2C driver handle
 */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/**************************************************************************************************
 *
 * IO expander
 *
 **************************************************************************************************/

/**
 * @brief Drive one TCA9554 output
 *
 * @param[in] bit   Expander bit, 0..7 (see BSP_EXIO_*)
 * @param[in] level Output level
 */
esp_err_t bsp_io_expander_set(uint8_t bit, bool level);

/**************************************************************************************************
 *
 * LCD interface
 *
 * LVGL is used as graphics library. LVGL is NOT thread safe, therefore the user must take the
 * LVGL mutex by calling bsp_display_lock() before calling any LVGL API (lv_...) and then give
 * the mutex back with bsp_display_unlock().
 *
 **************************************************************************************************/
#define BSP_LCD_SPI_NUM            (SPI2_HOST)

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

/**
 * @brief BSP display configuration structure
 */
typedef struct {
    esp_lv_adapter_config_t          lv_adapter_cfg;
    esp_lv_adapter_rotation_t        rotation;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;
    struct {
        unsigned int swap_xy;  /*!< Swap X and Y after read coordinates */
        unsigned int mirror_x; /*!< Mirror X after read coordinates */
        unsigned int mirror_y; /*!< Mirror Y after read coordinates */
    } touch_flags;
} bsp_display_cfg_t;

/**
 * @brief Initialize display with default settings
 *
 * This function initializes the panel, the touch controller and starts the LVGL handling task.
 *
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *bsp_display_start(void);

/**
 * @brief Initialize display
 *
 * @param cfg display configuration
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg);

/**
 * @brief Get pointer to input device (touch)
 */
lv_indev_t *bsp_display_get_input_dev(void);

/**
 * @brief Take LVGL mutex
 *
 * @param timeout_ms Timeout in [ms]. -1 will block indefinitely.
 */
esp_err_t bsp_display_lock(uint32_t timeout_ms);

/**
 * @brief Give LVGL mutex
 */
void bsp_display_unlock(void);

#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

#ifdef __cplusplus
}
#endif

// M5Stack Tab5
#include "board.h"

#include <math.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define TOUCH_ADDR_ST712X 0x55 // board revisions 2 and 3
#define TOUCH_ADDR_GT911 0x14  // revision 1
#define TOUCH_WAIT_MS 3000

// The BSP tells the panel revisions apart by which touch controller answers
// on I2C, 500 ms after releasing its reset, and asserts if neither does.
// On this unit (revision 3, ST7121) that probe failed on every boot of the
// first flash. Releasing the resets ourselves and waiting until the
// controller is actually there made detection succeed.
static void wait_for_touch_controller(void)
{
    bsp_feature_enable(BSP_FEATURE_LCD, true);
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);

    const int64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < TOUCH_WAIT_MS * 1000LL) {
        if (i2c_master_probe(bsp_i2c_get_handle(), TOUCH_ADDR_ST712X, 50) == ESP_OK ||
            i2c_master_probe(bsp_i2c_get_handle(), TOUCH_ADDR_GT911, 50) == ESP_OK) {
            ESP_LOGI(TAG, "touch controller answered after %lld ms",
                     (esp_timer_get_time() - start) / 1000);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "no touch controller after %d ms; the BSP's detection will fail", TOUCH_WAIT_MS);
}

void board_display_start(int rotation_deg)
{
    bsp_i2c_init();
    wait_for_touch_controller();

    // Not bsp_display_start(): its defaults turn on rotation with 50-line
    // buffers, and the rotation buffer then doesn't fit in internal DMA RAM.
    // LVGL renders changed areas into two internal buffers and DMA2D copies
    // them into the panel's (portrait) frame buffer.
    if (rotation_deg == 0) {
        bsp_display_cfg_t cfg = {
            .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
            .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
            .double_buffer = true,
            .flags = {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = false,
            },
        };
        bsp_display_start_with_config(&cfg);
    } else {
        // Landscape: LVGL renders 1280x720 and the P4's PPA rotates each strip
        // into the portrait frame buffer (CONFIG_LVGL_PORT_ENABLE_PPA), so the CPU
        // never touches the rotation. The PPA's output buffer is the same size as
        // a draw buffer, so the strips are 24 lines (in panel terms) instead of
        // 50: three 34.5 KB internal buffers, less than portrait's two of 72 KB.
        bsp_display_cfg_t cfg = {
            .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
            .buffer_size = BSP_LCD_H_RES * 24,
            .double_buffer = true,
            .flags = {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
        };
        lv_display_t *disp = bsp_display_start_with_config(&cfg);
        bsp_display_lock(0);
        bsp_display_rotate(disp, rotation_deg == 270 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
        bsp_display_unlock();
    }
    bsp_display_backlight_on();
}

void board_set_brightness(int percent)
{
    bsp_display_brightness_set(percent);
}

// ---------------------------------------------------------------- chime
//
// The ES8388 codec and its amplifier, set up on the first chime. Playing
// takes about half a second, so it runs in its own task.

#define CHIME_RATE 48000 // the BSP's I2S rate
#define CHIME_TONE_MS 160
#define CHIME_GAP_MS 40
#define CHIME_TAIL_MS 120 // silence, so closing the codec doesn't clip the end
#define CHIME_VOLUME 70

static TaskHandle_t s_chime_task;

// 880 Hz then 1319 Hz (A5, E6), each faded in and out so it doesn't click
static int16_t *make_chime(size_t *bytes)
{
    const int tone = CHIME_RATE * CHIME_TONE_MS / 1000, gap = CHIME_RATE * CHIME_GAP_MS / 1000;
    const int n = 2 * tone + gap + CHIME_RATE * CHIME_TAIL_MS / 1000;
    int16_t *pcm = heap_caps_calloc(n, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) return NULL;
    const float freqs[2] = {880.0f, 1318.5f};
    for (int t = 0; t < 2; t++) {
        int16_t *out = pcm + t * (tone + gap);
        for (int i = 0; i < tone; i++) {
            float env = fminf(1.0f, fminf(i, tone - i) / (CHIME_RATE * 0.01f));
            out[i] = (int16_t)(12000.0f * env * sinf(2.0f * (float)M_PI * freqs[t] * i / CHIME_RATE));
        }
    }
    *bytes = n * sizeof(int16_t);
    return pcm;
}

static void chime_task(void *arg)
{
    esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
    size_t bytes = 0;
    int16_t *pcm = make_chime(&bytes);
    if (!spk || !pcm) {
        ESP_LOGE(TAG, "speaker not available, no chimes");
        s_chime_task = NULL;
        vTaskDelete(NULL);
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = CHIME_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Open per chime: closing powers the amplifier down, so it can't hiss
        if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) continue;
        esp_codec_dev_set_out_vol(spk, CHIME_VOLUME);
        esp_codec_dev_write(spk, pcm, bytes);
        esp_codec_dev_close(spk);
    }
}

void board_chime(void)
{
    if (!s_chime_task && xTaskCreate(chime_task, "chime", 4096, NULL, 2, &s_chime_task) != pdPASS) {
        s_chime_task = NULL;
        return;
    }
    xTaskNotifyGive(s_chime_task);
}

void board_wifi_power_on(void)
{
    // The C6's supply is switched by an I/O expander bit (BSP_WIFI_EN);
    // without this the radio is unpowered.
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi power on failed: %s", esp_err_to_name(err));
    }
}

void board_lock(void)
{
    bsp_display_lock(0); // 0 waits forever in esp_lvgl_port
}

void board_unlock(void)
{
    bsp_display_unlock();
}

// ---------------------------------------------------------------- battery
//
// INA226 at 0x41 measures the pack through a 5 mOhm shunt (values from
// M5Stack's Tab5 firmware). The IP2326 charger and the power latch are on the
// second I/O expander (0x44): P4 PWROFF_PULSE, P5 nCHG_QC_EN (active low),
// P7 CHG_EN. P6 is CHG_STAT_LED, but it read 1 whether charging or not, so
// charging is judged from the current instead.
//
// Measured on this unit, 2026-10-06, on USB from a PC with the pack at 7.91 V:
// CHG_EN off, +0.001 A; CHG_EN on, -0.42 A with the pack rising to 7.99 V in
// a minute. So the INA226 reads negative while charging.

#define INA226_ADDR 0x41
#define INA226_REG_CONFIG 0x00
#define INA226_REG_SHUNT 0x01 // 2.5 uV per bit, signed
#define INA226_REG_BUS 0x02   // 1.25 mV per bit
#define INA226_REG_MFR_ID 0xFE
#define INA226_MFR_TI 0x5449
// 16-sample average, 1.1 ms bus and shunt conversions, continuous (as M5Stack)
#define INA226_CONFIG 0x0527
#define SHUNT_OHMS 0.005f

#define PIN_PWROFF IO_EXPANDER_PIN_NUM_4
#define PIN_NQC_EN IO_EXPANDER_PIN_NUM_5
#define PIN_CHG_EN IO_EXPANDER_PIN_NUM_7

static i2c_master_dev_handle_t s_ina;
static esp_io_expander_handle_t s_pwr_exp;

static esp_err_t ina_read(uint8_t reg, uint16_t *val)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(s_ina, &reg, 1, rx, 2, 50);
    *val = (uint16_t)(rx[0] << 8 | rx[1]);
    return err;
}

// The expander driver's reset leaves every pin high-impedance (and pulled
// down), so an output does nothing until it is also made push-pull. Missing
// that is why CHG_EN never reached the charger at first.
static void drive_pin(uint32_t pin, int level)
{
    esp_io_expander_set_dir(s_pwr_exp, pin, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(s_pwr_exp, pin, level);
    esp_io_expander_set_output_mode(s_pwr_exp, pin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
}

bool board_battery_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_ADDR,
        .scl_speed_hz = 400000,
    };
    uint16_t id = 0;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_ina) != ESP_OK ||
        ina_read(INA226_REG_MFR_ID, &id) != ESP_OK || id != INA226_MFR_TI) {
        ESP_LOGE(TAG, "no INA226 at 0x%02x (id 0x%04x), battery monitor off", INA226_ADDR, id);
        return false;
    }
    uint8_t w[3] = {INA226_REG_CONFIG, INA226_CONFIG >> 8, INA226_CONFIG & 0xFF};
    i2c_master_transmit(s_ina, w, sizeof(w), 50);

    // The Tab5 only charges while firmware holds CHG_EN high. QC fast charge
    // on as well, as M5Stack's firmware does.
    s_pwr_exp = bsp_io_expander1_init();
    drive_pin(PIN_PWROFF, 0);
    drive_pin(PIN_NQC_EN, 0);
    drive_pin(PIN_CHG_EN, 1);
    ESP_LOGI(TAG, "battery monitor on, charging enabled");
    return true;
}

bool board_battery_read(board_battery_t *out)
{
    uint16_t bus, shunt;
    if (!s_ina || ina_read(INA226_REG_BUS, &bus) != ESP_OK || ina_read(INA226_REG_SHUNT, &shunt) != ESP_OK) {
        return false;
    }
    out->volts = bus * 0.00125f;
    // Discharge reads positive, charge negative: already the board.h convention
    out->amps = (int16_t)shunt * 0.0000025f / SHUNT_OHMS;
    return true;
}

// The power latch drops on pulses of PWROFF_PULSE. M5Stack's firmware sends
// three, 100 ms high and 100 ms low, to be sure one is seen.
void board_power_off(void)
{
    ESP_LOGW(TAG, "powering off");
    bsp_display_backlight_off();
    for (int i = 0; i < 3; i++) {
        esp_io_expander_set_level(s_pwr_exp, PIN_PWROFF, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_io_expander_set_level(s_pwr_exp, PIN_PWROFF, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "power-off pulses sent but still running");
}

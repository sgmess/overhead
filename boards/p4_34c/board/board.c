// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C
#include "board.h"

#include <math.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

void board_display_start(int rotation_deg)
{
    (void)rotation_deg;
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
}

void board_set_brightness(int percent)
{
    bsp_display_brightness_set(percent);
}

// ---------------------------------------------------------------- chime
//
// The ES8311 codec and the amplifier on the speaker header (GPIO 53), set up
// on the first chime; as on the Tab5, in its own task, which plays for about
// half a second.

#define CHIME_RATE 22050 // the BSP's I2S rate
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
    // The C6 is always powered; esp_hosted resets it over GPIO 54.
}

void board_lock(void)
{
    bsp_display_lock(-1);
}

void board_unlock(void)
{
    bsp_display_unlock();
}

bool board_battery_init(void)
{
    return false; // no battery monitor on this board
}

bool board_battery_read(board_battery_t *out)
{
    return false;
}

void board_power_off(void)
{
}

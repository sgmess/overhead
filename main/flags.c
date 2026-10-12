#include "flags.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "misc/cache/lv_cache.h" // lv_image_cache_drop(), not in lvgl.h

extern const uint8_t flags_bin_start[] asm("_binary_flags_bin_start");
extern const uint8_t flags_bin_end[] asm("_binary_flags_bin_end");

#define HEADER 8 // "FLG1", u16 count, u8 width, u8 height

static lv_image_dsc_t s_dsc;
static uint8_t *s_buf; // PSRAM; embedded files aren't aligned for 16-bit reads
static char s_shown[3];

const lv_image_dsc_t *flag_image(const char *iso)
{
    const uint8_t *b = flags_bin_start;
    if (flags_bin_end - b < HEADER || memcmp(b, "FLG1", 4) != 0 || !iso || strlen(iso) != 2) return NULL;
    const int count = b[4] | b[5] << 8, w = b[6], h = b[7];
    const size_t size = (size_t)w * h * 3;
    if (strcmp(iso, s_shown) == 0) return &s_dsc;

    int i = 0;
    while (i < count && memcmp(b + HEADER + 2 * i, iso, 2) != 0) i++;
    const uint8_t *img = b + HEADER + 2 * count + i * size;
    if (i == count || img + size > flags_bin_end) return NULL;

    if (!s_buf) s_buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!s_buf) return NULL;
    lv_image_cache_drop(&s_dsc);
    memcpy(s_buf, img, size);
    s_dsc = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .w = w, .h = h, .stride = w * 2},
        .data_size = size,
        .data = s_buf,
    };
    strlcpy(s_shown, iso, sizeof(s_shown));
    return &s_dsc;
}

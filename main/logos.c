#include "logos.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "misc/cache/lv_cache.h" // lv_image_cache_drop(), not in lvgl.h

extern const uint8_t logos_bin_start[] asm("_binary_logos_bin_start");
extern const uint8_t logos_bin_end[] asm("_binary_logos_bin_end");

#define HEADER 6 // "LOG1", u16 count
#define ENTRY 12 // ICAO code and padding, u16 width, u16 height, u32 offset

static struct {
    lv_image_dsc_t dsc;
    uint8_t *buf; // PSRAM; embedded files aren't aligned for 16-bit reads
    char shown[4];
} s_slot[LOGO_SLOTS];

static unsigned u16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

const lv_image_dsc_t *logo_image(const char *icao, int slot)
{
    const uint8_t *b = logos_bin_start;
    if (logos_bin_end - b < HEADER || memcmp(b, "LOG1", 4) != 0 || !icao || strlen(icao) != 3) return NULL;
    if (slot < 0 || slot >= LOGO_SLOTS) return NULL;
    lv_image_dsc_t *dsc = &s_slot[slot].dsc;
    if (strcmp(icao, s_slot[slot].shown) == 0) return dsc;
    const unsigned count = u16(b + 4);
    if (logos_bin_end - b < HEADER + (ptrdiff_t)count * ENTRY) return NULL;

    for (unsigned i = 0; i < count; i++) {
        const uint8_t *e = b + HEADER + i * ENTRY;
        if (memcmp(e, icao, 3) != 0) continue;
        const unsigned w = u16(e + 4), h = u16(e + 6);
        const uint32_t off = u16(e + 8) | (uint32_t)u16(e + 10) << 16;
        const size_t size = (size_t)w * h * 2;
        if (w > LOGO_MAX_W || h > LOGO_MAX_H || off + size > (size_t)(logos_bin_end - b)) return NULL;

        uint8_t **buf = &s_slot[slot].buf;
        if (!*buf) *buf = heap_caps_malloc(LOGO_MAX_W * LOGO_MAX_H * 2, MALLOC_CAP_SPIRAM);
        if (!*buf) return NULL;
        lv_image_cache_drop(dsc);
        memcpy(*buf, b + off, size);
        *dsc = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = w, .h = h, .stride = w * 2},
            .data_size = size,
            .data = *buf,
        };
        strlcpy(s_slot[slot].shown, icao, sizeof(s_slot[slot].shown));
        return dsc;
    }
    return NULL;
}

// stb_image's JPEG decoder (baseline and progressive: planespotters.net's
// photos are progressive, which the P4's hardware decoder and LVGL's TJPGD
// can't read), with its working memory in PSRAM.

#include "esp_heap_caps.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MALLOC(sz) heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p, newsz) heap_caps_realloc(p, newsz, MALLOC_CAP_SPIRAM)
#define STBI_FREE(p) heap_caps_free(p)
#include "stb_image.h"

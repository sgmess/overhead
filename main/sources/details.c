#include "details.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "third_party/stb_image.h"

static const char *TAG = "details";

#define OWNER_API "https://api.adsbdb.com/v0/aircraft/"
#define PHOTO_API "https://api.planespotters.net/pub/photos/"
// planespotters.net refuses a User-Agent without a way to reach whoever runs it
#define CONTACT "+https://github.com/sgmess/overhead"
#define CACHE_N 16
#define RESP_MAX (192 * 1024) // the larger thumbnails are 20-60 KB
#define RETRY_US (60 * 1000000LL) // after a failed request, not after "unknown"
#define PHOTO_PX (DETAILS_PHOTO_W_MAX * DETAILS_PHOTO_H)

typedef struct {
    bool done;         // looked up, successfully or not
    int64_t failed_us; // when the request itself last failed, or was queued; 0 if not
} state_t;

typedef struct {
    char hex[8];
    uint32_t used; // for evicting the least recently used
    state_t owner_state, photo_state;
    char owner[48];
    uint16_t *px; // NULL when there is no photo
    int w, h;
    char credit[48];
} entry_t;

// In PSRAM, as route.c's: static internal RAM is short
static entry_t *s_cache;
static uint32_t s_clock;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

// The one aircraft waiting, newest wins: only the one on show matters
static char s_want[8], s_want_reg[12];
static bool s_want_owner, s_want_photo;

typedef struct {
    uint8_t *data;
    size_t len;
    bool overflow;
} resp_t;

static resp_t s_resp; // the task's alone

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || r->overflow) return ESP_OK;
    if (r->len + evt->data_len >= RESP_MAX) {
        r->overflow = true;
        return ESP_OK;
    }
    memcpy(r->data + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    r->data[r->len] = '\0';
    return ESP_OK;
}

// GET url into s_resp: the HTTP status, or -1 when the request failed
static int get(const char *url)
{
    static char agent[112];
    if (!agent[0]) snprintf(agent, sizeof(agent), "overhead/%s (" CONTACT ")", esp_app_get_description()->version);
    s_resp.len = 0;
    s_resp.overflow = false;
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http_event,
        .user_data = &s_resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = agent,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || s_resp.overflow) {
        ESP_LOGW(TAG, "%s: %s%s", url, esp_err_to_name(err), s_resp.overflow ? ", too long" : "");
        return -1;
    }
    return status;
}

static entry_t *find(const char *hex)
{
    for (int i = 0; i < CACHE_N; i++) {
        if (s_cache[i].hex[0] && strcmp(s_cache[i].hex, hex) == 0) return &s_cache[i];
    }
    return NULL;
}

static entry_t *find_or_add(const char *hex)
{
    entry_t *e = find(hex);
    if (e) return e;
    e = &s_cache[0];
    for (int i = 1; i < CACHE_N; i++) {
        if (s_cache[i].used < e->used) e = &s_cache[i];
    }
    heap_caps_free(e->px);
    *e = (entry_t){0};
    strlcpy(e->hex, hex, sizeof(e->hex));
    return e;
}

// The owner into owner (n bytes): false when the request failed, true with
// "" when adsbdb doesn't know the aircraft
static bool lookup_owner(const char *hex, char *owner, size_t n)
{
    char url[96];
    snprintf(url, sizeof(url), OWNER_API "%s", hex);
    const int status = get(url);
    owner[0] = '\0';
    if (status == 404) return true; // {"response":"unknown aircraft"}
    if (status != 200) return false;
    cJSON *root = cJSON_ParseWithLength((const char *)s_resp.data, s_resp.len);
    const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
    const cJSON *ac = cJSON_GetObjectItemCaseSensitive(resp, "aircraft");
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(ac, "registered_owner");
    if (cJSON_IsString(o)) strlcpy(owner, o->valuestring, n);
    cJSON_Delete(root);
    return true;
}

// Scale an RGB888 image down into px as RGB565, averaging the source pixels
// each one covers, to DETAILS_PHOTO_H high or DETAILS_PHOTO_W_MAX wide
static void scale(const uint8_t *src, int sw, int sh, uint16_t *px, int *w, int *h)
{
    int tw = sw * DETAILS_PHOTO_H / sh, th = DETAILS_PHOTO_H;
    if (tw > DETAILS_PHOTO_W_MAX) {
        tw = DETAILS_PHOTO_W_MAX;
        th = sh * tw / sw;
    }
    if (tw > sw || th > sh) { // never up
        tw = sw;
        th = sh;
    }
    for (int y = 0; y < th; y++) {
        const int y0 = y * sh / th, y1 = (y + 1) * sh / th;
        for (int x = 0; x < tw; x++) {
            const int x0 = x * sw / tw, x1 = (x + 1) * sw / tw;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = src + (yy * sw + x0) * 3;
                for (int xx = x0; xx < x1; xx++, p += 3, n++) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            r /= n;
            g /= n;
            b /= n;
            px[y * tw + x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
        }
    }
    *w = tw;
    *h = th;
}

// A registration as it can go in a URL as it is
static bool url_safe(const char *s)
{
    if (!s[0]) return false;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-') return false;
    }
    return true;
}

// The photo into a new buffer *px: false when a request failed, true with
// *px NULL when there is no photo
static bool lookup_photo(const char *hex, const char *reg, uint16_t **px, int *w, int *h, char *credit, size_t cn)
{
    char url[192];
    if (url_safe(reg)) {
        snprintf(url, sizeof(url), PHOTO_API "reg/%s", reg);
    } else {
        snprintf(url, sizeof(url), PHOTO_API "hex/%s", hex);
    }
    *px = NULL;
    credit[0] = '\0';
    if (get(url) != 200) return false;

    cJSON *root = cJSON_ParseWithLength((const char *)s_resp.data, s_resp.len);
    const cJSON *photo = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "photos"), 0);
    const cJSON *thumb = cJSON_GetObjectItemCaseSensitive(photo, "thumbnail_large");
    if (!thumb) thumb = cJSON_GetObjectItemCaseSensitive(photo, "thumbnail");
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(thumb, "src");
    const cJSON *by = cJSON_GetObjectItemCaseSensitive(photo, "photographer");
    url[0] = '\0';
    if (cJSON_IsString(src)) strlcpy(url, src->valuestring, sizeof(url));
    if (cJSON_IsString(by)) strlcpy(credit, by->valuestring, cn);
    cJSON_Delete(root);
    if (!url[0]) return true; // none of this one

    if (get(url) != 200) return false;
    int sw, sh, comp;
    uint8_t *rgb = stbi_load_from_memory(s_resp.data, (int)s_resp.len, &sw, &sh, &comp, 3);
    if (!rgb) {
        ESP_LOGW(TAG, "%s: can't decode the photo: %s", hex, stbi_failure_reason());
        return true; // it would fail again
    }
    *px = heap_caps_malloc(PHOTO_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (*px) scale(rgb, sw, sh, *px, w, h);
    stbi_image_free(rgb);
    return *px != NULL;
}

static void details_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char hex[8], reg[12];
        bool want_owner, want_photo;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(hex, s_want, sizeof(hex));
        strlcpy(reg, s_want_reg, sizeof(reg));
        want_owner = s_want_owner;
        want_photo = s_want_photo;
        s_want[0] = '\0';
        s_want_owner = s_want_photo = false;
        xSemaphoreGive(s_lock);
        if (!hex[0]) continue;

        if (want_owner) {
            char owner[48];
            const bool ok = lookup_owner(hex, owner, sizeof(owner));
            xSemaphoreTake(s_lock, portMAX_DELAY);
            entry_t *e = find_or_add(hex);
            e->used = ++s_clock;
            if (ok) {
                strlcpy(e->owner, owner, sizeof(e->owner));
                e->owner_state = (state_t){.done = true};
            } else {
                e->owner_state.failed_us = esp_timer_get_time();
            }
            xSemaphoreGive(s_lock);
        }
        if (want_photo) {
            uint16_t *px;
            int w = 0, h = 0;
            char credit[48];
            const bool ok = lookup_photo(hex, reg, &px, &w, &h, credit, sizeof(credit));
            ESP_LOGI(TAG, "%s: photo %s", hex, px ? credit : ok ? "none" : "failed");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            entry_t *e = find_or_add(hex);
            e->used = ++s_clock;
            if (ok) {
                heap_caps_free(e->px);
                e->px = px;
                e->w = w;
                e->h = h;
                strlcpy(e->credit, credit, sizeof(e->credit));
                e->photo_state = (state_t){.done = true};
            } else {
                e->photo_state.failed_us = esp_timer_get_time();
            }
            xSemaphoreGive(s_lock);
        }
    }
}

void details_start(void)
{
    s_cache = heap_caps_calloc(CACHE_N, sizeof(entry_t), MALLOC_CAP_SPIRAM);
    s_resp.data = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    // TLS and the decoder are on the heap; this is call depth
    if (s_cache && s_resp.data && lock &&
        (xTaskCreateWithCaps(details_task, "details", 12288, NULL, 3, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ==
             pdPASS ||
         xTaskCreate(details_task, "details", 12288, NULL, 3, &s_task) == pdPASS)) {
        s_lock = lock; // last: lookups take it as everything ready; the task waits for one first
        return;
    }
    ESP_LOGE(TAG, "not started: no memory");
    if (lock) vSemaphoreDelete(lock);
    heap_caps_free(s_cache);
    heap_caps_free(s_resp.data);
    s_cache = NULL;
    s_resp.data = NULL;
}

// Whether to ask (again) for something in state st; with the lock held
static bool wanted(state_t *st)
{
    if (st->done || (st->failed_us && esp_timer_get_time() - st->failed_us < RETRY_US)) return false;
    st->failed_us = esp_timer_get_time(); // stops it asking every second while the request is out
    return true;
}

// Queue hex for the task; with the lock held
static void want(const char *hex, const char *reg, bool owner, bool photo)
{
    if (strcmp(s_want, hex) != 0) {
        // One waiting at a time: the one this replaces was never asked for,
        // so let it ask again at once rather than after RETRY_US
        entry_t *gone = s_want[0] ? find(s_want) : NULL;
        if (gone) {
            if (s_want_owner && !gone->owner_state.done) gone->owner_state.failed_us = 0;
            if (s_want_photo && !gone->photo_state.done) gone->photo_state.failed_us = 0;
        }
        strlcpy(s_want, hex, sizeof(s_want));
        s_want_reg[0] = '\0';
        s_want_owner = s_want_photo = false;
    }
    if (reg[0]) strlcpy(s_want_reg, reg, sizeof(s_want_reg));
    s_want_owner |= owner;
    s_want_photo |= photo;
    xTaskNotifyGive(s_task);
}

void details_owner(const char *hex, char *out, size_t n)
{
    out[0] = '\0';
    if (!s_lock || !hex[0] || hex[0] == '~') return; // "~": not an ICAO address
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find_or_add(hex);
    e->used = ++s_clock;
    if (e->owner_state.done) {
        strlcpy(out, e->owner, n);
    } else if (wanted(&e->owner_state)) {
        want(hex, "", true, false);
    }
    xSemaphoreGive(s_lock);
}

bool details_photo(const char *hex, const char *reg, uint16_t *px, int *w, int *h, char *credit, size_t cn)
{
    if (!s_lock || !hex[0] || hex[0] == '~') return false;
    bool have = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find_or_add(hex);
    e->used = ++s_clock;
    if (e->photo_state.done) {
        if (e->px) {
            memcpy(px, e->px, (size_t)e->w * e->h * sizeof(uint16_t));
            *w = e->w;
            *h = e->h;
            strlcpy(credit, e->credit, cn);
            have = true;
        }
    } else if (wanted(&e->photo_state)) {
        want(hex, reg, false, true);
    }
    xSemaphoreGive(s_lock);
    return have;
}

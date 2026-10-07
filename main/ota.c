#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define RELEASE_MAX (128 * 1024) // the release JSON, notes included
#define USER_AGENT "overhead-radar"

typedef enum {
    IDLE,
    CHECKING,
    CHECKED,
    INSTALLING,
    FAILED,
} state_t;

static const char *const STATE_NAMES[] = {"idle", "checking", "checked", "installing", "error"};

static volatile state_t s_state = IDLE;
static volatile int s_progress;
static char s_detail[96];
static char s_latest[32], s_published[11], s_notes_url[160];
static char s_image_url[512]; // empty: the release has no image for this board

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware %s confirmed", esp_app_get_description()->version);
    }
}

static void fail(const char *fmt, const char *arg)
{
    snprintf(s_detail, sizeof(s_detail), fmt, arg);
    ESP_LOGW(TAG, "%s", s_detail);
    s_state = FAILED;
}

// ---------------------------------------------------------------- check

typedef struct {
    char *data;
    size_t len;
    bool overflow;
} body_t;

static esp_err_t on_data(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || b->overflow) return ESP_OK;
    if (b->len + evt->data_len >= RELEASE_MAX) {
        b->overflow = true;
        return ESP_OK;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    b->data[b->len] = '\0';
    return ESP_OK;
}

static void copy_str(char *dst, size_t n, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    strlcpy(dst, cJSON_IsString(v) ? v->valuestring : "", n);
}

static void check(void)
{
    body_t b = {.data = heap_caps_malloc(RELEASE_MAX, MALLOC_CAP_SPIRAM)};
    if (!b.data) return fail("%s", "Out of memory");

    char url[128];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", CONFIG_OVERHEAD_OTA_REPO);
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_data,
        .user_data = &b,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = USER_AGENT, // GitHub's API refuses requests without one
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        free(b.data);
        return fail("Couldn't reach GitHub (%s)", esp_err_to_name(err));
    }
    if (status == 404) {
        free(b.data);
        return fail("%s has no releases yet", CONFIG_OVERHEAD_OTA_REPO);
    }
    cJSON *root = status == 200 && !b.overflow ? cJSON_ParseWithLength(b.data, b.len) : NULL;
    free(b.data);
    if (!root) {
        char code[8];
        snprintf(code, sizeof(code), "%d", status);
        return fail("GitHub answered HTTP %s", code);
    }

    copy_str(s_latest, sizeof(s_latest), root, "tag_name");
    copy_str(s_published, sizeof(s_published), root, "published_at"); // the date part of ISO 8601
    copy_str(s_notes_url, sizeof(s_notes_url), root, "html_url");

    // This board's app image, named after the project
    char want[64];
    snprintf(want, sizeof(want), "%s-ota.bin", esp_app_get_description()->project_name);
    s_image_url[0] = '\0';
    const cJSON *asset;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(root, "assets")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        if (cJSON_IsString(name) && strcmp(name->valuestring, want) == 0) {
            copy_str(s_image_url, sizeof(s_image_url), asset, "browser_download_url");
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "latest release %s (%s), running %s; %s %s", s_latest, s_published,
             esp_app_get_description()->version, want, s_image_url[0] ? "found" : "not in it");
    s_state = CHECKED;
}

// ---------------------------------------------------------------- install

// GitHub answers the download URL with a redirect to a long signed URL, and
// the request line for that has to fit the transmit buffer
static esp_err_t install(void)
{
    esp_http_client_config_t http = {
        .url = s_image_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = USER_AGENT,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        snprintf(s_detail, sizeof(s_detail), "Download didn't start (%s)", esp_err_to_name(err));
        return err;
    }

    // Refuse an image built for another board before writing any of it
    esp_app_desc_t incoming;
    err = esp_https_ota_get_img_desc(h, &incoming);
    const char *mine = esp_app_get_description()->project_name;
    if (err == ESP_OK && strncmp(incoming.project_name, mine, sizeof(incoming.project_name)) != 0) {
        snprintf(s_detail, sizeof(s_detail), "That image is for %.32s, not %s", incoming.project_name, mine);
        esp_https_ota_abort(h);
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_LOGI(TAG, "installing %s (%s)", incoming.version, incoming.project_name);

    const int size = esp_https_ota_get_image_size(h);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (size > 0) s_progress = (int)(100LL * esp_https_ota_get_image_len_read(h) / size);
    }
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(h)) err = ESP_ERR_INVALID_SIZE;
    if (err != ESP_OK) {
        snprintf(s_detail, sizeof(s_detail), "Download failed (%s)", esp_err_to_name(err));
        esp_https_ota_abort(h);
        return err;
    }
    // Checks the image and points the bootloader at it
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) snprintf(s_detail, sizeof(s_detail), "Image rejected (%s)", esp_err_to_name(err));
    return err;
}

// ---------------------------------------------------------------- tasks

static void check_task(void *arg)
{
    check();
    vTaskDelete(NULL);
}

static void install_task(void *arg)
{
    s_progress = 0;
    if (install() == ESP_OK) {
        s_progress = 100;
        ESP_LOGW(TAG, "installed %s, restarting", s_latest);
        vTaskDelay(pdMS_TO_TICKS(1500)); // let the page see it finish
        esp_restart();
    }
    ESP_LOGW(TAG, "%s", s_detail);
    s_state = FAILED;
    vTaskDelete(NULL);
}

static bool busy(void)
{
    return s_state == CHECKING || s_state == INSTALLING;
}

esp_err_t ota_check(void)
{
    if (busy()) return ESP_ERR_INVALID_STATE;
    s_state = CHECKING;
    s_detail[0] = '\0';
    if (xTaskCreate(check_task, "ota", 6144, NULL, 4, NULL) != pdPASS) {
        s_state = FAILED;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_install(void)
{
    if (busy() || s_state != CHECKED || !s_image_url[0]) return ESP_ERR_INVALID_STATE;
    s_state = INSTALLING;
    s_detail[0] = '\0';
    // Its stack must be internal: PSRAM is unreachable while flash is written
    if (xTaskCreate(install_task, "ota", 8192, NULL, 4, NULL) != pdPASS) {
        s_state = FAILED;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ota_get_status(ota_status_t *out)
{
    *out = (ota_status_t){
        .state = STATE_NAMES[s_state],
        .detail = s_detail,
        .latest = s_latest,
        .published = s_published,
        .notes_url = s_notes_url,
        .has_image = s_image_url[0] != '\0',
        .progress = s_progress,
    };
}

#pragma once
// A fake flash partition in RAM: writes can only clear bits, as on flash,
// and erasing sets a range back to 0xff
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_ERR_NOT_SUPPORTED 0x106
typedef enum { ESP_PARTITION_TYPE_DATA } esp_partition_type_t;
#define ESP_PARTITION_SUBTYPE_ANY 0xff
typedef struct {
    const char *label;
    size_t size;
    uint8_t *mem;
} esp_partition_t;
extern esp_partition_t g_parts[2]; // set by a test; size 0: absent
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, int subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t at, void *out, size_t n);
esp_err_t esp_partition_write(const esp_partition_t *p, size_t at, const void *in, size_t n);
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t at, size_t n);

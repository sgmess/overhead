#pragma once
#include <stddef.h>
#include <stdint.h>
static inline uint16_t esp_rom_crc16_le(uint16_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return ~crc;
}
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    return ~crc;
}

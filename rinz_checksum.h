/*
 * RinOS zlib チェックサム ✿
 * CRC32 & Adler32
 */

#ifndef RINZ_CHECKSUM_H
#define RINZ_CHECKSUM_H

#include <stdint.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════
 * CRC32 (PNG, ZIP用)
 * ═══════════════════════════════════════════════════════════════*/

static uint32_t g_rinz_crc32_table[256];
/* 0 = uninitialized, 1 = one caller is initializing, 2 = ready. */
static int g_rinz_crc32_init_state = 0;

static inline void rinz_crc32_init_table(void) {
    int expected = 0;
    if (__atomic_load_n(&g_rinz_crc32_init_state, __ATOMIC_ACQUIRE) == 2)
        return;
    if (!__atomic_compare_exchange_n(&g_rinz_crc32_init_state, &expected, 1,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&g_rinz_crc32_init_state, __ATOMIC_ACQUIRE) != 2) {
        }
        return;
    }
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        g_rinz_crc32_table[i] = c;
    }
    __atomic_store_n(&g_rinz_crc32_init_state, 2, __ATOMIC_RELEASE);
}

static inline uint32_t rinz_crc32(const uint8_t* data, size_t len) {
    rinz_crc32_init_table();
    
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = g_rinz_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

static inline uint32_t rinz_crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    rinz_crc32_init_table();
    
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = g_rinz_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* ═══════════════════════════════════════════════════════════════
 * Adler32 (zlib用)
 * ═══════════════════════════════════════════════════════════════*/

#define RINZ_ADLER_BASE 65521

static inline uint32_t rinz_adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % RINZ_ADLER_BASE;
        b = (b + a) % RINZ_ADLER_BASE;
    }
    
    return (b << 16) | a;
}

static inline uint32_t rinz_adler32_update(uint32_t adler, const uint8_t* data, size_t len) {
    uint32_t a = adler & 0xFFFF;
    uint32_t b = (adler >> 16) & 0xFFFF;
    
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % RINZ_ADLER_BASE;
        b = (b + a) % RINZ_ADLER_BASE;
    }
    
    return (b << 16) | a;
}

#endif /* RINZ_CHECKSUM_H */

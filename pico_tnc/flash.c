/* flash.c - multi-page flash blob store for RP2040 / Pico
 *
 * Replaces single-page-only behavior with:
 *  - header {magic, version, length, crc32}
 *  - supports up to FLASH_BLOB_MAX_SIZE bytes payload (default 32768)
 *  - erase / program across 4KiB sectors and 256B pages
 *
 * Keep an eye on flash_addr() — it returns the first sector-aligned address
 * after __flash_binary_end (same behavior as your original file).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

/* keep your magic for compatibility */
#define PICO_MAGIC 0x4f434950u

/* Limits */
#define FLASH_BLOB_MAX_SIZE   (32768u) /* 32 KiB payload */
#define FLASH_PROGRAM_SIZE    FLASH_PAGE_SIZE   /* expected 256 */
#define FLASH_ERASE_SIZE      FLASH_SECTOR_SIZE /* expected 4096 */

/* Header layout stored at the start of the reserved region */
struct flash_blob_header {
    uint32_t magic;    /* PICO_MAGIC */
    uint32_t version;  /* header version */
    uint32_t length;   /* payload length in bytes */
    uint32_t crc32;    /* CRC32 of payload */
};

/* header version */
#define FLASH_BLOB_HDR_VER 1

extern char __flash_binary_end;

/* existing behavior: first sector-aligned address after binary end */
static uint8_t *flash_addr(void)
{
    return (uint8_t *)(((uint32_t)&__flash_binary_end + FLASH_ERASE_SIZE - 1) & ~(FLASH_ERASE_SIZE - 1));
}

/* CRC32 (IEEE 802.3 polynomial 0xEDB88320) */
static uint32_t crc32_calc(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* helper round up */
static inline uint32_t round_up(uint32_t v, uint32_t gran) {
    return (v + gran - 1u) / gran * gran;
}

/* NOTE: flash_range_program expects an offset from start of flash (not XIP_BASE)
   and the data buffer must be in RAM. We will ensure that.
*/

/* Marking small programming helper to run from RAM. Use pico-sdk macro if available */
#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif

static bool __not_in_flash_func(_flash_program_page)(uint32_t flash_offset, const uint8_t *buf, size_t bytes)
{
    if ((flash_offset % FLASH_PROGRAM_SIZE) != 0) return false;
    if ((bytes % FLASH_PROGRAM_SIZE) != 0) return false;

    uint32_t ints = save_and_disable_interrupts();
    busy_wait_us_32(8334);
    flash_range_program(flash_offset, buf, (uint32_t)bytes);
    restore_interrupts(ints);
    return true;
}

/* erase N bytes starting at flash_offset (must be multiple of FLASH_ERASE_SIZE) */
static bool flash_erase_region(uint32_t flash_offset, size_t region_size)
{
    if ((flash_offset % FLASH_ERASE_SIZE) != 0) return false;
    if ((region_size % FLASH_ERASE_SIZE) != 0) return false;

    uint32_t ints = save_and_disable_interrupts();
    busy_wait_us_32(8334);
    flash_range_erase(flash_offset, region_size);
    restore_interrupts(ints);
    return true;
}

/* public API - read payload into buffer (len <= FLASH_BLOB_MAX_SIZE)
   returns true on success and len bytes copied to data */
bool flash_read(void *data, int len)
{
    if (!data) return false;
    if (len < 0) return false;
    if ((uint32_t)len > FLASH_BLOB_MAX_SIZE) return false;

    uint8_t *base = flash_addr();
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + (uint32_t)base - XIP_BASE); /* XIP_BASE + offset == pointer */
    /* simpler: pointer = XIP_BASE + offset */
    flash_ptr = (const uint8_t *)(XIP_BASE + ((uint32_t)base - XIP_BASE)); /* keep clear */

    /* read header */
    struct flash_blob_header hdr;
    memcpy(&hdr, flash_ptr, sizeof(hdr));

    if (hdr.magic != PICO_MAGIC) return false;
    if (hdr.version != FLASH_BLOB_HDR_VER) return false;
    if (hdr.length > (uint32_t)len) return false;
    if (hdr.length > FLASH_BLOB_MAX_SIZE) return false;

    const uint8_t *payload_ptr = flash_ptr + sizeof(hdr);
    /* copy payload */
    memcpy(data, payload_ptr, hdr.length);

    /* verify crc */
    uint32_t calc = crc32_calc(data, hdr.length);
    if (calc != hdr.crc32) return false;

    return true;
}

/* public API - write payload (len bytes) into reserved region
   returns true on success (and verifies after program) */
bool flash_write(void *data, int len)
{
    if (!data) return false;
    if (len < 0) return false;
    if ((uint32_t)len > FLASH_BLOB_MAX_SIZE) return false;

    uint8_t *base = flash_addr();
    uint32_t flash_offset = (uint32_t)base - XIP_BASE; /* offset for flash_range_* calls */

    /* Build header */
    struct flash_blob_header hdr;
    hdr.magic = PICO_MAGIC;
    hdr.version = FLASH_BLOB_HDR_VER;
    hdr.length = (uint32_t)len;
    hdr.crc32 = crc32_calc(data, len);

    /* construct full image: header + payload, then pad to FLASH_PROGRAM_SIZE */
    size_t header_sz = sizeof(hdr);
    size_t payload_sz = (size_t)len;
    size_t total = header_sz + payload_sz;
    size_t padded = round_up((uint32_t)total, FLASH_PROGRAM_SIZE);
    size_t erase_needed = round_up((uint32_t)padded, FLASH_ERASE_SIZE);

    /* Allocate a RAM buffer for programming (must be RAM) */
    uint8_t *buf = malloc(padded);
    if (!buf) return false;
    /* default erased state is 0xFF */
    memset(buf, 0xFF, padded);
    memcpy(buf, &hdr, header_sz);
    memcpy(buf + header_sz, data, payload_sz);

    /* Erase the region first */
    if (!flash_erase_region(flash_offset, erase_needed)) {
        free(buf);
        return false;
    }

    /* Program page-by-page (FLASH_PROGRAM_SIZE chunks) */
    bool ok = true;
    for (size_t pos = 0; pos < padded; pos += FLASH_PROGRAM_SIZE) {
        if (!_flash_program_page(flash_offset + (uint32_t)pos, buf + pos, FLASH_PROGRAM_SIZE)) {
            ok = false;
            break;
        }
    }

    if (!ok) {
        free(buf);
        return false;
    }

    /* Verify by comparing the XIP view of flash with our RAM buffer for padded bytes */
    const uint8_t *flash_view = (const uint8_t *)(XIP_BASE + flash_offset);
    if (memcmp(flash_view, buf, padded) != 0) {
        free(buf);
        return false;
    }

    free(buf);
    return true;
}

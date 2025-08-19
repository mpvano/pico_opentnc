#include "flash.h"
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif

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

static inline uint32_t round_up(uint32_t v, uint32_t gran) {
    return (v + gran - 1u) / gran * gran;
}

/* Base address of wear leveling region (offset from flash start) */
static uint32_t wl_region_base_offset(void) {
    return PICO_FLASH_SIZE_BYTES - FLASH_WL_REGION_SIZE;
}

/* Helpers */
static bool flash_erase_region(uint32_t flash_offset, size_t region_size) {
    if ((flash_offset % FLASH_SECTOR_SIZE) != 0) return false;
    if ((region_size % FLASH_SECTOR_SIZE) != 0) return false;

    uint32_t ints = save_and_disable_interrupts();
    busy_wait_us_32(8334);
    flash_range_erase(flash_offset, region_size);
    restore_interrupts(ints);
    return true;
}

static bool __not_in_flash_func(_flash_program_page)(uint32_t flash_offset, const uint8_t *buf, size_t bytes) {
    if ((flash_offset % FLASH_PAGE_SIZE) != 0) return false;
    if ((bytes % FLASH_PAGE_SIZE) != 0) return false;

    uint32_t ints = save_and_disable_interrupts();
    busy_wait_us_32(8334);
    flash_range_program(flash_offset, buf, (uint32_t)bytes);
    restore_interrupts(ints);
    return true;
}

/* Find the slot index of the newest valid blob, return -1 if none */
static int find_latest_slot(uint32_t *out_seq) {
    int latest_index = -1;
    uint32_t latest_seq = 0;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        const struct flash_blob_header *hdr = (const struct flash_blob_header *)(XIP_BASE + wl_region_base_offset() + i * SLOT_SIZE);
        if (hdr->magic == PICO_MAGIC && hdr->version == FLASH_BLOB_HDR_VER && hdr->length == FLASH_BLOB_PAYLOAD_SIZE) {
            /* verify CRC */
            const uint8_t *payload = (const uint8_t *)hdr + sizeof(*hdr);
            uint32_t calc = crc32_calc(payload, hdr->length);
            if (calc == hdr->crc32) {
                if (latest_index == -1 || hdr->seq > latest_seq) {
                    latest_index = i;
                    latest_seq = hdr->seq;
                }
            }
        }
    }
    if (out_seq) *out_seq = latest_seq;
    return latest_index;
}

/* Read latest valid payload */
int flash_read(void *data, int len) {
    if (!data || len != FLASH_BLOB_PAYLOAD_SIZE) return -1;

    int best_idx = -1;
    uint32_t best_seq = 0;

    for (int i = 0; i < SLOT_COUNT; ++i) {
        const uint8_t *slot_base = (const uint8_t *)(XIP_BASE + wl_region_base_offset() + i * SLOT_SIZE);
        const struct flash_blob_header *hdr = (const struct flash_blob_header *)slot_base;

        if (hdr->magic != PICO_MAGIC) continue;
        if (hdr->version != FLASH_BLOB_HDR_VER) continue;
        if (hdr->length != FLASH_BLOB_PAYLOAD_SIZE) continue;

        const uint8_t *payload = slot_base + sizeof(*hdr);
        uint32_t calc = crc32_calc(payload, hdr->length);
        if (calc != hdr->crc32) continue; // corrupted slot; skip

        if (best_idx < 0 || hdr->seq > best_seq) {
            best_idx = i;
            best_seq = hdr->seq;
        }
    }

    if (best_idx < 0) {
        // No valid slots found
        return -1;
    }

    const uint8_t *best_base = (const uint8_t *)(XIP_BASE + wl_region_base_offset() + best_idx * SLOT_SIZE);
    const struct flash_blob_header *best_hdr = (const struct flash_blob_header *)best_base;
    const uint8_t *best_payload = best_base + sizeof(*best_hdr);

    memcpy(data, best_payload, best_hdr->length);
    return best_idx;
}

/* Write payload to next slot */
int flash_write(void *data, int len) {
    if (!data || len != FLASH_BLOB_PAYLOAD_SIZE) return -1;

    uint32_t latest_seq = 0;
    int latest_index = find_latest_slot(&latest_seq);

    /* Next slot in ring */
    int next_index = (latest_index + 1) % SLOT_COUNT;
    if (latest_index < 0) { /* none yet */
        next_index = 0;
        latest_seq = 0;
    }

    struct flash_blob_header hdr;
    hdr.magic = PICO_MAGIC;
    hdr.version = FLASH_BLOB_HDR_VER;
    hdr.length = FLASH_BLOB_PAYLOAD_SIZE;
    hdr.crc32 = crc32_calc(data, len);
    hdr.seq = latest_seq + 1;

    /* Build RAM buffer */
    size_t total = sizeof(hdr) + len;
    size_t padded = round_up(total, FLASH_PAGE_SIZE);
    uint8_t *buf = malloc(padded);
    if (!buf) return -1;
    memset(buf, 0xFF, padded);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), data, len);

    uint32_t flash_offset = wl_region_base_offset() + next_index * SLOT_SIZE;

    /* Debug info */
    // printf("[WL] Next slot: %d\n", next_index);
    // printf("[WL] flash_offset: 0x%08x\n", flash_offset);
    // printf("[WL] XIP address: 0x%08x\n", XIP_BASE + flash_offset);
    // printf("[WL] SLOT_SIZE: %u\n", (unsigned)SLOT_SIZE);
    // printf("[WL] SLOT_COUNT: %u\n", (unsigned)SLOT_COUNT);

    /* Erase slot */
    if (!flash_erase_region(flash_offset, SLOT_SIZE)) {
        printf("[WL] Erase failed\n");
        free(buf);
        return -1;
    }

    /* Program */
    bool ok = true;
    for (size_t pos = 0; pos < padded; pos += FLASH_PAGE_SIZE) {
        if (!_flash_program_page(flash_offset + pos, buf + pos, FLASH_PAGE_SIZE)) {
            printf("[WL] Program failed at pos %u\n", (unsigned)pos);
            ok = false;
            break;
        }
    }
    // ... after the program loop and before free(buf);
    bool verify_ok = false;
    if (ok) {
        const uint8_t *flash_view = (const uint8_t *)(XIP_BASE + flash_offset);
        uint32_t crc_flash = crc32_calc(flash_view + sizeof(hdr), FLASH_BLOB_PAYLOAD_SIZE);
        if (crc_flash == hdr.crc32) {
            verify_ok = true;
        } else {
            printf("[WL] Verify CRC mismatch: wrote 0x%08x, flash 0x%08x\n", hdr.crc32, crc_flash);
            ok = false;
        }
    }

    free(buf);
    if (ok) {
        return next_index;  // slot written
    } else {
        return -1;          // failed
    }
}

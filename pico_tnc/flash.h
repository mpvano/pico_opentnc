#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* Payload size (fixed) */
#define FLASH_BLOB_PAYLOAD_SIZE   (32768u)   /* 32 KiB payload */
#define FLASH_SECTOR_SIZE         (4096u)    /* erase granularity */
#define FLASH_PAGE_SIZE           (256u)     /* program granularity */

/* Wear leveling region size from END of flash */
#define FLASH_WL_REGION_SIZE      (512u * 1024u) /* reserve last 512 KB for wear leveling */

/* Derived slot size and count */
#define SLOT_SIZE  ((FLASH_BLOB_PAYLOAD_SIZE + sizeof(struct flash_blob_header) + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE)
#define SLOT_COUNT (FLASH_WL_REGION_SIZE / SLOT_SIZE)

/* Magic for identifying valid blobs */
#define PICO_MAGIC 0x4f434950u
#define FLASH_BLOB_HDR_VER 1

struct flash_blob_header {
    uint32_t magic;    /* PICO_MAGIC */
    uint32_t version;  /* FLASH_BLOB_HDR_VER */
    uint32_t length;   /* payload length in bytes */
    uint32_t crc32;    /* CRC32 of payload */
    uint32_t seq;      /* wear-leveling sequence number */
};

/* API */
int flash_read(void *data, int len);
/* Writes payload, returns slot index (0..SLOT_COUNT-1) on success, -1 on failure */
int flash_write(void *data, int len);

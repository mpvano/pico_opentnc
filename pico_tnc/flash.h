#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* Flash storage limits for blob payload */
#define FLASH_BLOB_MAX_SIZE   (32768u)   /* 32 KiB payload max */
#define FLASH_SECTOR_SIZE     (4096u)    /* erase granularity */
#define FLASH_PAGE_SIZE       (256u)     /* program granularity */

/* Magic used to mark valid blob in flash (same as in flash.c) */
#define PICO_MAGIC 0x4f434950u
#define FLASH_BLOB_HDR_VER 1

/* Stored at the start of reserved flash region */
struct flash_blob_header {
    uint32_t magic;    /* PICO_MAGIC */
    uint32_t version;  /* FLASH_BLOB_HDR_VER */
    uint32_t length;   /* payload length in bytes */
    uint32_t crc32;    /* CRC32 of payload */
};

/* Reads payload into 'data' (up to len bytes).
   Returns true if valid payload found and CRC verified. */
bool flash_read(void *data, int len);

/* Writes payload from 'data' (len bytes) into reserved flash region.
   Erases and programs as needed. Returns true if write succeeded
   and verification passed. */
bool flash_write(void *data, int len);

/* Optional: check if a valid blob is stored and return its length.
   Returns true if valid header+CRC present. */
bool flash_blob_info(size_t *out_length);

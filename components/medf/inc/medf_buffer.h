/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDF_ARENA_ALIGNMENT 64
#define MEDF_READ_ALIGNMENT 64
#define MEDF_NO_REF 0

typedef struct {
    uint8_t *data;
    size_t size;
    /* Header and tag reads only: no read-ahead task, medf_buffer_read() is
       served from a window instead, and medf_buffer_view() always fails. */
    bool direct;
} medf_arena_t;

/* M5tabOS: the file is read through external functions. The system only reads
   the SD card through its own entry points (services/tab5_volumes: tab5_leitor),
   so it registers them here. Without registration, the previous POSIX path
   applies (open/lseek/read). */
typedef struct {
    void  *(*open)(const char *path, off_t *size);   /* NULL = failed to open */
    size_t (*read_at)(void *file, off_t offset, void *out, size_t size);
    void   (*close)(void *file);
    /* The read-ahead task: priority and core (0 and 0 = the original,
       priority 3 on core 0). Reading the SD card is almost all waiting for DMA:
       with a high priority it gets in nobody's way and does not starve of CPU
       when a decoder occupies the core. */
    int    readahead_prio;
    int    readahead_core;   /* -1 = any core */
} medf_io_t;
void medf_set_io(const medf_io_t *io);

typedef struct medf_buffer medf_buffer_t;

size_t medf_arena_max_view(const medf_arena_t *arena);

medf_buffer_t *medf_buffer_open(const char *path, const medf_arena_t *arena);
void medf_buffer_close(medf_buffer_t *buffer);

off_t medf_buffer_size(const medf_buffer_t *buffer);
off_t medf_buffer_tell(const medf_buffer_t *buffer);
size_t medf_buffer_read(medf_buffer_t *buffer, void *out, size_t size);

/* Reads `size` bytes at `offset` into a fresh PSRAM allocation. FATFS copies
   the leading partial sector through its own window and then DMAs the rest
   straight to the caller, so the transfer only runs at full speed when the
   destination carries the same MEDF_READ_ALIGNMENT phase as the file offset
   (6.4 vs 11.3 MB/s measured on an SD card). The buffer is over-allocated
   to place the data at that phase: the return value is the data, *out_owner
   the pointer to heap_caps_free(). */
uint8_t *medf_buffer_read_alloc(medf_buffer_t *buffer, off_t offset, size_t size, void **out_owner);
off_t medf_buffer_seek(medf_buffer_t *buffer, off_t offset);
void medf_buffer_skip(medf_buffer_t *buffer, off_t delta);
void medf_buffer_set_readahead(medf_buffer_t *buffer, bool enabled);

const uint8_t *medf_buffer_view(medf_buffer_t *buffer, size_t size, uint32_t *ref);
const uint8_t *medf_buffer_view_at(medf_buffer_t *buffer, off_t offset, size_t size, uint32_t *ref);
void medf_buffer_release(medf_buffer_t *buffer, uint32_t ref);
void medf_buffer_release_all(medf_buffer_t *buffer);
void medf_buffer_interrupt(medf_buffer_t *buffer, bool interrupted);

#ifdef __cplusplus
}
#endif

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define FF_SECTOR 4096u
#define FF_MAX_PAYLOAD (FF_SECTOR - 40u)
#define FF_MAX_SLOTS 1536u /* 6 MiB history partition */
/* Single owner only. IO must synchronously finish programming/erasing.
 * After any IO error, reopen before further operations. Never auto-format. */
typedef struct {
    void *ctx;
    int (*read)(void *, uint32_t, void *, size_t);
    int (*write)(void *, uint32_t, const void *, size_t);
    int (*erase)(void *, uint32_t, size_t);
    uint32_t size;
} ff_io_t;
enum { FF_OK, FF_EMPTY, FF_FULL, FF_IO, FF_CORRUPT, FF_ARGUMENT, FF_EXHAUSTED };
typedef struct {
    ff_io_t io;
    uint64_t ids[FF_MAX_SLOTS];
    uint8_t states[FF_MAX_SLOTS];
    unsigned slots, next, pending;
    uint64_t highest;
    bool ready;
} flash_fifo_t;
int ff_open(flash_fifo_t *f, const ff_io_t *io);
int ff_append(flash_fifo_t *f, const void *data, size_t len, uint64_t *id);
/* Peek never consumes; ack only accepts the current oldest record ID. */
int ff_peek(flash_fifo_t *f, void *data, size_t capacity, size_t *len, uint64_t *id);
int ff_ack(flash_fifo_t *f, uint64_t id);
/* Non-consuming cursor read: smallest retained ID strictly greater than after.
 * A cursor older than ring retention resumes at the oldest retained record. */
int ff_read_after(flash_fifo_t *f, uint64_t after, void *data, size_t capacity,
                  size_t *len, uint64_t *id);

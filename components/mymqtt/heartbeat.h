#pragma once
#include <stdbool.h>

typedef struct { int pending_id; unsigned failures; } heartbeat_t;
static inline void heartbeat_reset(heartbeat_t *h)
{
    h->pending_id = -1;
    h->failures = 0;
}
static inline void heartbeat_ack(heartbeat_t *h, int id)
{
    if (h->pending_id == id && id >= 0) heartbeat_reset(h);
}
static inline bool heartbeat_expire(heartbeat_t *h)
{
    if (h->pending_id >= 0) {
        h->pending_id = -1;
        ++h->failures;
    }
    return h->failures >= 3;
}
static inline bool heartbeat_sent(heartbeat_t *h, int id)
{
    h->pending_id = id;
    if (id < 0) ++h->failures;
    return h->failures >= 3;
}

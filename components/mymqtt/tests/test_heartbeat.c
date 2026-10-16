#include "../heartbeat.h"
#include <assert.h>
int main(void)
{
    heartbeat_t h;
    heartbeat_reset(&h);
    for (int i = 1; i <= 3; ++i) {
        assert(!heartbeat_sent(&h, i));
        heartbeat_ack(&h, 99); /* Sensor PUBACK must not satisfy heartbeat. */
        assert(heartbeat_expire(&h) == (i == 3));
    }
    heartbeat_reset(&h);
    assert(!heartbeat_sent(&h, 10));
    assert(!heartbeat_expire(&h));
    assert(!heartbeat_sent(&h, 11));
    heartbeat_ack(&h, 10); /* Late ACK from the timed-out attempt. */
    assert(h.failures == 1 && h.pending_id == 11);
    heartbeat_ack(&h, 11);
    assert(h.failures == 0 && h.pending_id == -1);
    assert(!heartbeat_expire(&h));
    assert(!heartbeat_sent(&h, -1));
    assert(!heartbeat_sent(&h, -2));
    assert(heartbeat_sent(&h, -1));
    heartbeat_reset(&h); /* Reconnection starts a new streak. */
    assert(h.failures == 0 && h.pending_id == -1);
    return 0;
}

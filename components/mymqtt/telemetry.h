#pragma once
#include <stdio.h>
#include <inttypes.h>
#include "modbusTCP.h"

/* device_id is generated from the MAC, not unescaped user input. */
static inline int telemetry_format(char *out, size_t size, const char *device_id,
                                   int64_t uptime_ms, const modbus_temp_sample_t *sample)
{
    char value[16] = "null", age[32] = "null", sampled_at[32] = "null";
    if (sample->has_value) {
        snprintf(value, sizeof(value), "%" PRIu16, sample->value);
        int64_t age_ms = uptime_ms - sample->last_success_ms;
        snprintf(age, sizeof(age), "%" PRId64, age_ms > 0 ? age_ms : 0);
        snprintf(sampled_at, sizeof(sampled_at), "%" PRId64, sample->last_success_ms);
    }
    int len = snprintf(out, size,
        "{\"device_id\":\"%s\",\"uptime_ms\":%" PRId64 ",\"temperature_raw\":%s,"
        "\"has_value\":%s,\"stale\":%s,\"sample_age_ms\":%s,\"sample_uptime_ms\":%s,"
        "\"modbus_state\":%d,\"modbus_error\":%d}",
        device_id, uptime_ms, value, sample->has_value ? "true" : "false",
        sample->stale || !sample->has_value ? "true" : "false", age, sampled_at,
        (int)sample->state, (int)sample->last_error);
    return len < 0 || (size_t)len >= size ? -1 : len;
}

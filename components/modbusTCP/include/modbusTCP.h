#ifndef INGAWAY_MODBUS_TCP_H
#define INGAWAY_MODBUS_TCP_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_types.h"

typedef enum {
    MODBUS_WAIT_NETWORK,
    MODBUS_CONNECTING,
    MODBUS_POLLING,
    MODBUS_BACKOFF,
    MODBUS_FAULT,
} modbus_acquisition_state_t;

typedef struct {
    uint16_t value;                 /* Last successful raw register value. */
    uint8_t type;
    bool has_value;                 /* False until the first successful read. */
    bool stale;                     /* Check before using value as current data. */
    int64_t last_success_ms;         /* Monotonic milliseconds since boot. */
    esp_err_t last_error;
    modbus_acquisition_state_t state;
} modbus_temp_sample_t;

/* Call on GOT_IP. Idempotent; returns task submission status, not TCP status.
 * netif must remain alive and must be the same interface on repeated calls.
 */
int modbusTCP_init(esp_netif_t *netif);
/* Call on Wi-Fi disconnect/lost IP; marks cached data stale immediately. */
void modbusTCP_network_down(void);
/* Nonblocking snapshot. ESP_OK means snapshot copied, not that value is fresh. */
esp_err_t modbusTCP_get_temp_sample(modbus_temp_sample_t *sample);
/* Nonblocking cache accessor. temp needs sizeof(uint16_t) bytes.
 * No sample -> INVALID_STATE; stale -> TIMEOUT, outputs remain unchanged.
 */
int read_temp(uint8_t *temp, uint8_t *type);
/* HMI register map is not configured; returns NOT_SUPPORTED. */
int read_hmi(uint8_t *value, uint8_t *type);

#endif

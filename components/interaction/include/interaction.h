#pragma once
#include "esp_err.h"

/* Starts LCD/UI and encoder tasks. Hardware errors are logged asynchronously. */
esp_err_t interaction_start(void);

/* Wake the LCD for external status changes without allocating UI resources.
 * Safe to call from normal task context after interaction_start(). */
esp_err_t interaction_notify_status_change(void);

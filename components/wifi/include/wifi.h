#ifndef __WIFI_H__
#define __WIFI_H__

void wifi_init(void);
/* Task-context only. Persists a one-shot force-BLUFI flag, then restarts.
 * Returns only if the NVS request fails; caller should display an error.
 * Does not erase unrelated NVS data or saved Wi-Fi credentials. */
void wifi_reprovision(void);
#endif

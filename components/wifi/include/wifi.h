#ifndef __WIFI_H__
#define __WIFI_H__

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
extern EventGroupHandle_t wifi_event_group;
void wifi_init(void);
#endif

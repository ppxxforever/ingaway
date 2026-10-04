#ifndef __BLUFI_H__
#define __BLUFI_H__

#pragma once

#include "soc/soc_caps.h"
#include "esp_blufi_api.h"

#define BLUFI_TAG "BLUFI"
#define BLUFI_INFO(fmt, ...)   ESP_LOGI(BLUFI_TAG, fmt, ##__VA_ARGS__)
#define BLUFI_ERROR(fmt, ...)  ESP_LOGE(BLUFI_TAG, fmt, ##__VA_ARGS__)

void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free);
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);

int blufi_security_init(void);
void blufi_security_deinit(void);
#if !SOC_MPI_SUPPORTED
void blufi_dh_pregen_start(void);
void blufi_dh_pregen_start_with_cb(void (*done_cb)(void));
void blufi_dh_pregen_wait(void);
#endif

int esp_blufi_gap_register_callback(void);
esp_err_t esp_blufi_host_init(void);
esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *callbacks);
/* 先调用 esp_blufi_profile_deinit() 并等待 ESP_BLUFI_EVENT_DEINIT_FINISH。 */
esp_err_t esp_blufi_host_deinit(void);
esp_err_t esp_blufi_controller_init(void);
esp_err_t esp_blufi_controller_deinit(void);


void blufi_start(void);

#endif

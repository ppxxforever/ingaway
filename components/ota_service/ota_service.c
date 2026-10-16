#include "ota_service.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "storage_archive.h"

static const char *TAG = "ota";
static TaskHandle_t ota_task_handle;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_service_status_t status = { .state = OTA_SERVICE_IDLE, .progress_percent = -1 };

static void set_status(ota_service_state_t state, int progress, esp_err_t err,
                       const char *candidate, const char *detail)
{
    portENTER_CRITICAL(&status_lock);
    status.state = state;
    status.progress_percent = progress;
    status.last_error = err;
    if (candidate) snprintf(status.candidate_version, sizeof(status.candidate_version), "%s", candidate);
    if (detail) snprintf(status.detail, sizeof(status.detail), "%s", detail);
    portEXIT_CRITICAL(&status_lock);
}

ota_service_status_t ota_service_get_status(void)
{
    portENTER_CRITICAL(&status_lock);
    ota_service_status_t copy = status;
    portEXIT_CRITICAL(&status_lock);
    return copy;
}

static void log_status(storage_level_t level, const char *event, esp_err_t err, const char *detail)
{
    storage_log(level, "ota", event, err, 0, detail ? detail : "");
}

static bool app_partition_is_ota(const esp_partition_t *partition)
{
    return partition && partition->type == ESP_PARTITION_TYPE_APP &&
           partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           partition->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static esp_err_t verify_platform(void)
{
#ifndef CONFIG_IDF_TARGET_ESP32S3
    set_status(OTA_SERVICE_FAILED, -1, ESP_ERR_NOT_SUPPORTED, NULL, "target_not_esp32s3");
    return ESP_ERR_NOT_SUPPORTED;
#else
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) return err;
    const uint32_t required = (uint32_t)CONFIG_INGAWAY_OTA_REQUIRED_FLASH_MB * 1024u * 1024u;
    if (flash_size < required) {
        ESP_LOGE(TAG, "Flash too small: detected=%" PRIu32 " required=%" PRIu32, flash_size, required);
        set_status(OTA_SERVICE_FAILED, -1, ESP_ERR_INVALID_SIZE, NULL, "flash_too_small");
        return ESP_ERR_INVALID_SIZE;
    }

    int ota_count = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        if (app_partition_is_ota(esp_partition_get(it))) ota_count++;
    }
    if (it) esp_partition_iterator_release(it);
    if (ota_count < 2) {
        ESP_LOGE(TAG, "Need two OTA app partitions, found %d", ota_count);
        set_status(OTA_SERVICE_FAILED, -1, ESP_ERR_NOT_FOUND, NULL, "dual_ota_missing");
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!app_partition_is_ota(running) || !app_partition_is_ota(update) || running == update) {
        set_status(OTA_SERVICE_FAILED, -1, ESP_ERR_INVALID_STATE, NULL, "ota_partition_invalid");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "S3 OTA platform OK: flash=%" PRIu32 " MiB running=%s next=%s ota_count=%d",
             flash_size / (1024u * 1024u), running->label, update->label, ota_count);
    return ESP_OK;
#endif
}

static esp_err_t validate_candidate(const esp_app_desc_t *candidate)
{
    const esp_app_desc_t *current = esp_app_get_description();
    portENTER_CRITICAL(&status_lock);
    snprintf(status.current_version, sizeof(status.current_version), "%s", current->version);
    snprintf(status.candidate_version, sizeof(status.candidate_version), "%s", candidate->version);
    portEXIT_CRITICAL(&status_lock);

    if (strcmp(candidate->project_name, CONFIG_INGAWAY_OTA_PROJECT_NAME) != 0) {
        ESP_LOGE(TAG, "OTA image model mismatch: candidate=%s expected=%s",
                 candidate->project_name, CONFIG_INGAWAY_OTA_PROJECT_NAME);
        set_status(OTA_SERVICE_FAILED, -1, ESP_ERR_INVALID_VERSION, candidate->version, "model_mismatch");
        return ESP_ERR_INVALID_VERSION;
    }
    if (strcmp(candidate->version, current->version) == 0) {
        ESP_LOGI(TAG, "OTA skipped: already on version %s", current->version);
        set_status(OTA_SERVICE_SKIPPED, 100, ESP_OK, candidate->version, "already_current");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "OTA candidate accepted: model=%s current=%s candidate=%s",
             candidate->project_name, current->version, candidate->version);
    return ESP_OK;
}

static esp_err_t run_pending_app_self_test(void)
{
    set_status(OTA_SERVICE_CHECKING, 0, ESP_OK, NULL, "self_test");

    esp_err_t err = verify_platform();
    if (err != ESP_OK) return err;

    const esp_app_desc_t *current = esp_app_get_description();
    if (strcmp(current->project_name, CONFIG_INGAWAY_OTA_PROJECT_NAME) != 0) {
        ESP_LOGE(TAG, "Running app model mismatch: current=%s expected=%s",
                 current->project_name, CONFIG_INGAWAY_OTA_PROJECT_NAME);
        set_status(OTA_SERVICE_FAILED, -1, ESP_ERR_INVALID_VERSION, NULL, "running_model_mismatch");
        return ESP_ERR_INVALID_VERSION;
    }

    /* ota_service_start_once() is called only after STA got IP. Treat that as
     * the network self-test gate and avoid touching/migrating NVS here, so a
     * rollback image can keep using the same stored Wi-Fi/NVS data. */
    ESP_LOGI(TAG, "Pending OTA app self-test passed: model=%s version=%s",
             current->project_name, current->version);
    return ESP_OK;
}

static void ota_task(void *arg)
{
    (void)arg;
#if CONFIG_INGAWAY_OTA_ENABLED
    vTaskDelay(pdMS_TO_TICKS(CONFIG_INGAWAY_OTA_CHECK_DELAY_MS));
    set_status(OTA_SERVICE_CHECKING, 0, ESP_OK, NULL, "checking");
    log_status(STORAGE_INFO, "check_start", ESP_OK, CONFIG_INGAWAY_OTA_URL);

    esp_err_t err = verify_platform();
    if (err != ESP_OK) goto done;

    const esp_http_client_config_t http_config = {
        .url = CONFIG_INGAWAY_OTA_URL,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    const esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
#if CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD
        .partial_http_download = true,
        .max_http_request_size = 4096,
#endif
    };

    esp_https_ota_handle_t handle = NULL;
    err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        set_status(OTA_SERVICE_FAILED, -1, err, NULL, "begin_failed");
        log_status(STORAGE_WARN, "begin_failed", err, NULL);
        goto done;
    }

    esp_app_desc_t candidate;
    err = esp_https_ota_get_img_desc(handle, &candidate);
    if (err != ESP_OK) {
        set_status(OTA_SERVICE_FAILED, -1, err, NULL, "read_desc_failed");
        esp_https_ota_abort(handle);
        goto done;
    }
    err = validate_candidate(&candidate);
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
        goto done;
    }

    set_status(OTA_SERVICE_DOWNLOADING, 1, ESP_OK, candidate.version, "downloading");
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int total = esp_https_ota_get_image_size(handle);
        int read = esp_https_ota_get_image_len_read(handle);
        int progress = total > 0 ? (read * 100) / total : -1;
        if (progress > 0 && progress < 100) set_status(OTA_SERVICE_DOWNLOADING, progress, ESP_OK, NULL, "downloading");
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        set_status(OTA_SERVICE_FAILED, -1, err, NULL, "download_failed");
        log_status(STORAGE_WARN, "download_failed", err, NULL);
        esp_https_ota_abort(handle);
        goto done;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        set_status(OTA_SERVICE_FAILED, -1, ESP_ERR_INVALID_SIZE, NULL, "incomplete_image");
        esp_https_ota_abort(handle);
        goto done;
    }
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        set_status(OTA_SERVICE_FAILED, -1, err, NULL, "finish_failed");
        log_status(STORAGE_WARN, "finish_failed", err, NULL);
        goto done;
    }

    set_status(OTA_SERVICE_UPDATED, 100, ESP_OK, candidate.version, "restarting");
    log_status(STORAGE_INFO, "updated", ESP_OK, candidate.version);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
#else
    set_status(OTA_SERVICE_SKIPPED, -1, ESP_OK, NULL, "disabled");
#endif

done:
    ota_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t ota_service_start_once(void)
{
#if !CONFIG_INGAWAY_OTA_ENABLED
    return ESP_OK;
#else
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = run_pending_app_self_test();
        if (err == ESP_OK) {
            err = esp_ota_mark_app_valid_cancel_rollback();
            if (err != ESP_OK) return err;
            set_status(OTA_SERVICE_IDLE, 100, ESP_OK, NULL, "app_valid");
            log_status(STORAGE_INFO, "app_valid", ESP_OK, running ? running->label : "");
        } else {
            log_status(STORAGE_WARN, "self_test_failed_rollback", err,
                       running ? running->label : "");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_ota_mark_app_invalid_rollback_and_reboot();
            return err;
        }
    }
    if (ota_task_handle) return ESP_OK;
    return xTaskCreate(ota_task, "https_ota", 8192, NULL, 4, &ota_task_handle) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
#endif
}

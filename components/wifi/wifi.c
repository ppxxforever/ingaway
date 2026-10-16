#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mymqtt.h"
#include "modbusTCP.h"
#include "wifi.h"
#include "storage_archive.h"
#include "ota_service.h"
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#include "esp_blufi_api.h"
#include "blufi.h"

#include "esp_blufi.h"

#ifndef CONFIG_SOC_BLUFI_SUPPORTED
#error "This SOC does not support BLUFI"
#endif

#define WIFI_CONNECTION_MAXIMUM_RETRY 10
#define INVALID_REASON                255
#define INVALID_RSSI                  -128

esp_netif_t * wifi_netif;
static void event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

#define WIFI_LIST_NUM   10

static wifi_config_t sta_config;
static bool boot_with_saved_wifi;
static bool force_provision;
#define SAVED_WIFI_TIMEOUT_MS 30000

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;
#define BLE_SHUTDOWN_REQUESTED_BIT BIT1
#define BLE_DISCONNECTED_BIT       BIT2
#define BLUFI_DEINITIALIZED_BIT    BIT3
#define WIFI_AUTO_RECONNECT_BIT    BIT4
#define WIFI_ASSOCIATED_BIT        BIT5
static uint8_t wifi_retry = 0;

/* store the station info for send back to phone */
static bool gl_sta_connected = false;
static bool gl_sta_got_ip = false;
static bool ble_is_connected = false;
static uint8_t gl_sta_bssid[6];
static uint8_t gl_sta_ssid[32];
static int gl_sta_ssid_len;
static wifi_sta_list_t gl_sta_list;
static bool gl_sta_is_connecting = false;
static esp_blufi_extra_info_t gl_sta_conn_info;

/* 关闭请求保持置位，避免断开回调或异步 DH 回调重新开启广播。 */
static void blufi_start_advertising(void)
{
    if (!(xEventGroupGetBits(wifi_event_group) & BLE_SHUTDOWN_REQUESTED_BIT)) {
        esp_blufi_adv_start();
    }
}

/* 独立任务中停止协议栈，不能在 BLE/IP 事件回调中同步销毁。 */
static void ble_shutdown_task(void *arg)
{
    xEventGroupWaitBits(wifi_event_group, BLE_SHUTDOWN_REQUESTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    /* 给配网成功通知留出发送时间；这不是手机已收到通知的确认。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_blufi_adv_stop();
    if (ble_is_connected) {
        esp_blufi_disconnect();
        xEventGroupWaitBits(wifi_event_group, BLE_DISCONNECTED_BIT,
                            pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
    }

    /* Profile 销毁是异步操作，等完成回调后才能销毁 BTC 任务。 */
    esp_err_t ret = esp_blufi_profile_deinit();
    if (ret != ESP_OK ||
        !(xEventGroupWaitBits(wifi_event_group, BLUFI_DEINITIALIZED_BIT,
                             pdFALSE, pdTRUE, pdMS_TO_TICKS(2000)) &
          BLUFI_DEINITIALIZED_BIT)) {
        BLUFI_ERROR("BLUFI profile shutdown failed or timed out\n");
        vTaskDelete(NULL);
        return;
    }

    ret = esp_blufi_host_deinit();
    if (ret != ESP_OK) {
        BLUFI_ERROR("BLUFI host shutdown failed: %s\n", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_deinit();
    if (ret != ESP_OK) {
        BLUFI_ERROR("BLE controller shutdown failed: %s\n", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
#endif

    blufi_security_deinit();
    ble_is_connected = false;
    BLUFI_INFO("Provisioning complete, BLE host and controller shut down\n");
    vTaskDelete(NULL);
}

/*
 * 记录最近一次 Wi-Fi 连接状态。
 * BLUFI 手机端查询连接状态时，会把这里保存的信息回传给手机。
 */
static void record_wifi_conn_info(int rssi, uint8_t reason)
{
    memset(&gl_sta_conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (gl_sta_is_connecting) {
        gl_sta_conn_info.sta_max_conn_retry_set = true;
        gl_sta_conn_info.sta_max_conn_retry = WIFI_CONNECTION_MAXIMUM_RETRY;
    } else {
        gl_sta_conn_info.sta_conn_rssi_set = true;
        gl_sta_conn_info.sta_conn_rssi = rssi;
        gl_sta_conn_info.sta_conn_end_reason_set = true;
        gl_sta_conn_info.sta_conn_end_reason = reason;
    }
}

/*
 * 真正发起 STA 连接。
 * 用于开机连接 NVS 配置，或 BLUFI 完整配置提交后的连接。
 */
static void wifi_connect(void)
{
    wifi_retry = 0;
    gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
    record_wifi_conn_info(INVALID_RSSI, INVALID_REASON);
}

/*
 * Wi-Fi 连接过程中的重试逻辑。
 * 只在 gl_sta_is_connecting 为 true 时重试，避免断线后无限重连。
 */
static bool wifi_reconnect(void)
{
    bool ret;
    if (gl_sta_is_connecting && wifi_retry++ < WIFI_CONNECTION_MAXIMUM_RETRY) {
        BLUFI_INFO("BLUFI WiFi starts reconnection\n");
        gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
        record_wifi_conn_info(INVALID_RSSI, INVALID_REASON);
        ret = true;
    } else {
        ret = false;
    }
    return ret;
}


/* Once provisioning succeeds, recover lost Wi-Fi even after BLE is shut down. */
static void wifi_recovery_task(void *arg)
{
    unsigned no_ip_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        if (!(bits & WIFI_AUTO_RECONNECT_BIT) || (bits & CONNECTED_BIT)) {
            no_ip_ms = 0;
            continue;
        }
        if ((bits & WIFI_AUTO_RECONNECT_BIT) && !(bits & WIFI_ASSOCIATED_BIT)) {
            no_ip_ms = 0;
            esp_err_t err = esp_wifi_connect();
            BLUFI_INFO("WiFi periodic reconnect: %s\n", esp_err_to_name(err));
            storage_log(err==ESP_OK?STORAGE_DEBUG:STORAGE_WARN,"wifi","reconnect_attempt",err,0,"");
        } else if ((no_ip_ms += 10000) >= 30000) {
            /* Associated but DHCP/IP recovery stalled: reassociate after a grace period. */
            no_ip_ms = 0;
            esp_wifi_disconnect();
        }
    }
}

/*
 * IP 事件处理：STA 拿到 IP 后，说明配网真正成功。
 * 回报成功后请求关闭 BLE；后续再次获取 IP 不再访问已关闭的 BLUFI。
 */
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_mode_t mode;

    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        esp_blufi_extra_info_t info;

        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT | WIFI_AUTO_RECONNECT_BIT);
        esp_wifi_get_mode(&mode);

        memset(&info, 0, sizeof(esp_blufi_extra_info_t));
        memcpy(info.sta_bssid, gl_sta_bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid = gl_sta_ssid;
        info.sta_ssid_len = gl_sta_ssid_len;
        gl_sta_got_ip = true;
        storage_log(STORAGE_INFO,"wifi","got_ip",0,0,"");
        modbusTCP_init(wifi_netif);
        mqtt_start();
        esp_err_t ota_err = ota_service_start_once();
        if (ota_err != ESP_OK) {
            storage_log(STORAGE_WARN,"ota","start_failed",ota_err,0,"");
        }
        if (!(xEventGroupGetBits(wifi_event_group) & BLE_SHUTDOWN_REQUESTED_BIT)) {
            if (ble_is_connected) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, 0, &info);
            }
            xEventGroupSetBits(wifi_event_group, BLE_SHUTDOWN_REQUESTED_BIT);
        }
        break;
    }
    case IP_EVENT_STA_LOST_IP:
        storage_log(STORAGE_WARN,"wifi","lost_ip",0,0,"");
        gl_sta_got_ip = false;
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        modbusTCP_network_down();
        mqtt_network_down();
        break;
    default:
        break;
    }
    return;
}

/*
 * Wi-Fi 事件处理：负责处理 STA/AP 的启动、连接、断开、扫描完成等事件。
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_event_sta_connected_t *event;
    wifi_event_sta_disconnected_t *disconnected_event;
    wifi_mode_t mode;

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        if (boot_with_saved_wifi) {
            BLUFI_INFO("Connecting with saved WiFi configuration\n");
            wifi_connect();
        } else {
            BLUFI_INFO("WiFi STA started, waiting for BLUFI credentials\n");
        }
        break;
    case WIFI_EVENT_STA_CONNECTED:
        storage_log(STORAGE_INFO,"wifi","associated",0,0,"");
        /* 已经连上路由器 AP，但还不代表能上网；还要等 IP_EVENT_STA_GOT_IP。 */
        gl_sta_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_ASSOCIATED_BIT);
        gl_sta_is_connecting = false;
        event = (wifi_event_sta_connected_t*) event_data;
        memcpy(gl_sta_bssid, event->bssid, 6);
        memcpy(gl_sta_ssid, event->ssid, event->ssid_len);
        gl_sta_ssid_len = event->ssid_len;
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        disconnected_event = (wifi_event_sta_disconnected_t*) event_data;
        storage_log(STORAGE_WARN,"wifi","disconnected",disconnected_event->reason,0,"");
        modbusTCP_network_down();
        mqtt_network_down();
        /* STA 断开：如果当前处于连接流程中，则按最大次数重试。 */
        if (!(xEventGroupGetBits(wifi_event_group) & WIFI_AUTO_RECONNECT_BIT) &&
            gl_sta_connected == false && wifi_reconnect() == false) {
            gl_sta_is_connecting = false;
            disconnected_event = (wifi_event_sta_disconnected_t*) event_data;
            record_wifi_conn_info(disconnected_event->rssi, disconnected_event->reason);
        }
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        gl_sta_connected = false;
        gl_sta_got_ip = false;
        memset(gl_sta_ssid, 0, 32);
        memset(gl_sta_bssid, 0, 6);
        gl_sta_ssid_len = 0;
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT | WIFI_ASSOCIATED_BIT);
        break;
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t apCount = 0;
        esp_wifi_scan_get_ap_num(&apCount);
        if (apCount == 0) {
            BLUFI_INFO("Nothing AP found");
            break;
        }
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * apCount);
        if (!ap_list) {
            BLUFI_ERROR("malloc error, ap_list is NULL");
            esp_wifi_clear_ap_list();
            break;
        }
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
        esp_blufi_ap_record_t * blufi_ap_list = (esp_blufi_ap_record_t *)malloc(apCount * sizeof(esp_blufi_ap_record_t));
        if (!blufi_ap_list) {
            if (ap_list) {
                free(ap_list);
            }
            BLUFI_ERROR("malloc error, blufi_ap_list is NULL");
            break;
        }
        for (int i = 0; i < apCount; ++i)
        {
            blufi_ap_list[i].rssi = ap_list[i].rssi;
            memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
        }

        if (ble_is_connected == true) {
            esp_blufi_send_wifi_list(apCount, blufi_ap_list);
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }
        esp_wifi_scan_stop();
        free(ap_list);
        free(blufi_ap_list);
        break;
    }
    default:
        break;
    }
    return;
}

/*
 * 初始化 Wi-Fi 子系统。
 * 显式启用 FLASH 存储，读取已保存配置，再由 STA_START 发起历史网络连接。
 */
static void initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_event_group ? ESP_OK : ESP_ERR_NO_MEM);
    esp_err_t event_loop_ret = esp_event_loop_create_default();
    if (event_loop_ret != ESP_OK && event_loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(event_loop_ret);
    }
    wifi_netif = esp_netif_create_default_wifi_sta();
    assert(wifi_netif);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, NULL));
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_FLASH) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_get_config(WIFI_IF_STA, &sta_config) );
    boot_with_saved_wifi = !force_provision && sta_config.sta.ssid[0] != '\0';
    if (boot_with_saved_wifi) {
        xEventGroupSetBits(wifi_event_group, WIFI_AUTO_RECONNECT_BIT);
    }
    record_wifi_conn_info(INVALID_RSSI, INVALID_REASON);
    ESP_ERROR_CHECK( esp_wifi_start() );
}

/*
 * BLUFI 回调表。
 * 手机端通过 BLE 发来的 BLUFI 命令，最终会进入 event_callback()。
 * 加密、解密、校验相关函数由 blufi 组件提供。
 */
static esp_blufi_callbacks_t callbacks = {
    .event_cb = event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

/*
 * BLUFI 核心事件回调。
 * 手机连接 BLE、发送 SSID、发送密码、请求连接 AP、查询状态等都会走到这里。
 */
static void event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        BLUFI_INFO("BLUFI init finish\n");
#if SOC_MPI_SUPPORTED
        blufi_start_advertising();
#endif
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        BLUFI_INFO("BLUFI deinit finish\n");
        xEventGroupSetBits(wifi_event_group, BLUFI_DEINITIALIZED_BIT);
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        /* 手机已经连上设备的 BLE GATT，停止广播并初始化 BLUFI 安全协商。 */
        BLUFI_INFO("BLUFI ble connect\n");
        ble_is_connected = true;
        xEventGroupClearBits(wifi_event_group, BLE_DISCONNECTED_BIT);
        esp_blufi_adv_stop();
        if (xEventGroupGetBits(wifi_event_group) & BLE_SHUTDOWN_REQUESTED_BIT) {
            esp_blufi_disconnect();
            break;
        }
        blufi_security_init();
        #ifdef CONFIG_BLUFI_BLE_SMP_ENABLE
        // Try to initiate BLE security request after connection established.
        BLUFI_INFO("Try to initiate BLE security request\n");
        esp_err_t ret = esp_blufi_start_security_request(param->connect.remote_bda);
        if (ret != ESP_OK) {
            BLUFI_ERROR("Failed to start security request: %s\n", esp_err_to_name(ret));
        }
        #endif // CONFIG_BLUFI_BLE_SMP_ENABLE
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        BLUFI_INFO("BLUFI ble disconnect\n");
        ble_is_connected = false;
        blufi_security_deinit();
        xEventGroupSetBits(wifi_event_group, BLE_DISCONNECTED_BIT);
        if (xEventGroupGetBits(wifi_event_group) & BLE_SHUTDOWN_REQUESTED_BIT) {
            break;
        }
#if !SOC_MPI_SUPPORTED
        blufi_dh_pregen_start_with_cb(blufi_start_advertising);
#else
        blufi_start_advertising();
#endif
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        /* 手机设置 Wi-Fi 工作模式，例如 WIFI_MODE_STA 或 WIFI_MODE_APSTA。 */
        BLUFI_INFO("BLUFI Set WIFI opmode %d\n", param->wifi_mode.op_mode);
        ESP_ERROR_CHECK( esp_wifi_set_mode(param->wifi_mode.op_mode) );
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
        /* Persist one complete configuration, not partial SSID/password events. */
        BLUFI_INFO("BLUFI request wifi connect to AP\n");
        if (sta_config.sta.ssid[0] == '\0') {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        xEventGroupClearBits(wifi_event_group, WIFI_AUTO_RECONNECT_BIT);
        gl_sta_is_connecting = false;
        esp_wifi_disconnect();
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (err != ESP_OK) {
            BLUFI_ERROR("Cannot save WiFi configuration: %s\n", esp_err_to_name(err));
            esp_blufi_send_error_info(ESP_BLUFI_MSG_STATE_ERROR);
            break;
        }
        wifi_connect();
        break;
    }
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        BLUFI_INFO("BLUFI request wifi disconnect from AP\n");
        xEventGroupClearBits(wifi_event_group, WIFI_AUTO_RECONNECT_BIT);
        gl_sta_is_connecting = false;
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        BLUFI_ERROR("BLUFI report error, error code %d\n", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
        /* 手机查询当前配网状态：成功、连接中、失败都会通过 BLUFI 回传。 */
        wifi_mode_t mode;
        esp_blufi_extra_info_t info;

        esp_wifi_get_mode(&mode);

        if (gl_sta_connected) {
            memset(&info, 0, sizeof(esp_blufi_extra_info_t));
            memcpy(info.sta_bssid, gl_sta_bssid, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = gl_sta_ssid;
            info.sta_ssid_len = gl_sta_ssid_len;
            esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, 0, &info);
        } else if (gl_sta_is_connecting) {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, 0, &gl_sta_conn_info);
        } else {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, 0, &gl_sta_conn_info);
        }
        BLUFI_INFO("BLUFI get wifi status from AP\n");

        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        BLUFI_INFO("blufi close a gatt connection");
        esp_blufi_disconnect();
        break;
	case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        sta_config.sta.bssid_set = 1;
        BLUFI_INFO("Recv STA BSSID %s\n", sta_config.sta.ssid);
        break;
	case ESP_BLUFI_EVENT_RECV_STA_SSID:
        /* Stage fields in RAM; REQ_CONNECT_TO_AP commits them together. */
        if (param->sta_ssid.ssid_len >= sizeof(sta_config.sta.ssid)/sizeof(sta_config.sta.ssid[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid STA SSID\n");
            break;
        }
        strncpy((char *)sta_config.sta.ssid, (char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        BLUFI_INFO("Recv STA SSID %s\n", sta_config.sta.ssid);
        break;
	case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        /* Stage password without logging or persisting an incomplete config. */
        if (param->sta_passwd.passwd_len >= sizeof(sta_config.sta.password)/sizeof(sta_config.sta.password[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid STA PASSWORD\n");
            break;
        }
        strncpy((char *)sta_config.sta.password, (char *)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
        sta_config.sta.threshold.authmode = param->sta_passwd.passwd_len
            ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        BLUFI_INFO("Received STA password\n");
        break;

    case ESP_BLUFI_EVENT_GET_WIFI_LIST:{
        /* 手机请求扫描附近 Wi-Fi；扫描完成后在 WIFI_EVENT_SCAN_DONE 中回传列表。 */
        wifi_scan_config_t scanConf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false
        };
        esp_err_t ret = esp_wifi_scan_start(&scanConf, true);
        if (ret != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }

    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        BLUFI_INFO("Recv Custom Data %" PRIu32 "\n", param->custom_data.data_len);
        ESP_LOG_BUFFER_HEX("Custom Data", param->custom_data.data, param->custom_data.data_len);
        break;
    default:
        break;
    }
}

/* One-shot flag preserves local reprovisioning even with saved Wi-Fi.
 * Reboot avoids racing BLE teardown; unrelated NVS data remains intact. */
void wifi_reprovision(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_boot", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "force_blufi", 1);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        BLUFI_ERROR("Cannot request provisioning: %s\n", esp_err_to_name(err));
        return;
    }
    BLUFI_INFO("Restarting into BLUFI provisioning by local request\n");
    esp_restart();
}

static void blufi_boot_task(void *arg);

void wifi_init(void)
{
    esp_err_t ret;

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("wifi_boot", NVS_READWRITE, &handle));
    uint8_t forced = 0;
    ret = nvs_get_u8(handle, "force_blufi", &forced);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) ESP_ERROR_CHECK(ret);
    force_provision = forced != 0;
    if (force_provision) {
        ESP_ERROR_CHECK(nvs_erase_key(handle, "force_blufi"));
        ESP_ERROR_CHECK(nvs_commit(handle));
    }
    nvs_close(handle);

    initialise_wifi();
    ESP_ERROR_CHECK(xTaskCreate(wifi_recovery_task, "wifi_recovery", 3072, NULL, 5, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(blufi_boot_task, "blufi_boot", 4096, NULL, 5, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

static void start_blufi(void)
{
    /* Driver keeps the saved config for recovery; start a clean provisioning
     * draft so an old password/BSSID cannot leak into a new network profile. */
    memset(&sta_config, 0, sizeof(sta_config));
    esp_err_t ret;
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_init();
    if (ret) {
        BLUFI_ERROR("%s BLUFI controller init failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
#endif
    //注册回调函数并初始化BLUFI
    ret = esp_blufi_host_and_cb_init(&callbacks);
    if (ret) {
        BLUFI_ERROR("%s initialise failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    BaseType_t task_created = xTaskCreate(ble_shutdown_task, "ble_shutdown",4096, NULL, 5, NULL);
                                         
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    BLUFI_INFO("BLUFI VERSION %04x\n", esp_blufi_get_version());
}

static void blufi_boot_task(void *arg)
{
    (void)arg;
    if (boot_with_saved_wifi) {
        /* SHUTDOWN also latches success, even if Wi-Fi drops before we run. */
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            CONNECTED_BIT | BLE_SHUTDOWN_REQUESTED_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(SAVED_WIFI_TIMEOUT_MS));
        if (bits & (CONNECTED_BIT | BLE_SHUTDOWN_REQUESTED_BIT)) {
            BLUFI_INFO("Saved WiFi connected; BLE remains disabled\n");
            vTaskDelete(NULL);
            return;
        }
        BLUFI_INFO("Saved WiFi timed out after 30s; enabling BLUFI\n");
    } else {
        BLUFI_INFO("No saved WiFi or local provisioning requested; enabling BLUFI\n");
    }
    /* GOT_IP racing initialization sets SHUTDOWN and suppresses advertising;
     * ble_shutdown_task handles teardown once initialization has completed. */
    start_blufi();
    vTaskDelete(NULL);
}

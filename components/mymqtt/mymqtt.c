#include "mymqtt.h"
#include "esp_log.h"
#include "esp_err.h"
#include "mqtt_client.h"
#include "bmp280.h"
#include "aht20.h"
#include "environment.h"

#define MQTT_BROKER_URI "mqtts://192.168.31.73"
#define MQTT_BROKER_PORT 8883
#define MQTT_TOPIC_TEMP "env/temp"
#define MQTT_TOPIC_HUMIDITY "env/humidity"
#define MQTT_TOPIC_PRESSURE "env/pressure"
#define DEV_MAX_RETRY 5


extern const char mqtt_ca[]
    asm("_binary_ca_crt_start");

extern const char mqtt_cert[]
    asm("_binary_client_crt_start");

extern const char mqtt_key[]
    asm("_binary_client_key_start");
static int dev_retry_count = 0;
static const char *TAG = "mqtt";
static esp_mqtt_topic_t topic_list[] = {
    {.filter = MQTT_TOPIC_TEMP, .qos = 1},
    {.filter = MQTT_TOPIC_HUMIDITY, .qos = 1},
    {.filter = MQTT_TOPIC_PRESSURE, .qos = 1},
};

#define TOPIC_LIST_NUM (sizeof(topic_list) / sizeof(topic_list[0]))

static const esp_mqtt_client_config_t mqtt_cfg = {
    .broker = {
        .address.uri = MQTT_BROKER_URI,
        .address.port = MQTT_BROKER_PORT,
        .verification.certificate = mqtt_ca,
    },
    .credentials = {
        .authentication = {
            .certificate = mqtt_cert,
            .key = mqtt_key,
        },
    },
};

static void mqtt_publish_msg(esp_mqtt_client_handle_t client, const char *topic, const char *msg) {
    int msg_id = esp_mqtt_client_publish(client, topic, msg, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish message to topic %s", topic);
    } else {
        ESP_LOGI(TAG, "Message published to topic %s, msg_id=%d", topic, msg_id);
    }
}

static void mqtt_publish_task(void *arg) {
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)arg;
    float temperature = 0.0f, humidity = 0.0f, pressure = 0.0f,bmp_temperature = 0.0f;
    ESP_LOGI(TAG, "Initializing environment sensors");
    esp_err_t sensor_err = environment_start();
    while(1){
        if (sensor_err != ESP_OK) {
            ESP_LOGE(TAG, "Environment start failed: %s", esp_err_to_name(sensor_err));
            if (++dev_retry_count >= DEV_MAX_RETRY) {
                ESP_LOGE(TAG, "Max retry count reached. Stopping MQTT publish task.");
                vTaskDelete(NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retrying
            continue;
        }

        if (aht20_read(aht, &temperature,&humidity) == ESP_OK &&
            bmp280_read(bmp,&bmp_temperature, &pressure) == ESP_OK) {
            
            char temp_str[16], hum_str[16], pres_str[16];
            snprintf(temp_str, sizeof(temp_str), "%.2f", temperature);
            snprintf(hum_str, sizeof(hum_str), "%.2f", humidity);
            snprintf(pres_str, sizeof(pres_str), "%.2f", pressure);

            mqtt_publish_msg(client, MQTT_TOPIC_TEMP, temp_str);
            mqtt_publish_msg(client, MQTT_TOPIC_HUMIDITY, hum_str);
            mqtt_publish_msg(client, MQTT_TOPIC_PRESSURE, pres_str);
        } else {
            ESP_LOGE(TAG, "Failed to read sensor data");
        }

        vTaskDelay(pdMS_TO_TICKS(5000)); // Publish every 5 seconds
    }
    

}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data){

    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            ESP_LOGI(TAG, "Connected to MQTT broker at %s", MQTT_BROKER_URI);
            msg_id = esp_mqtt_client_subscribe_multiple(client, topic_list, TOPIC_LIST_NUM);
            if (msg_id < 0) {
                ESP_LOGI(TAG, "Failed to subscribe to topics");
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, client, 5, NULL);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle) {
                ESP_LOGE(TAG, "type=%d, tls_err=0x%x, mbedtls_err=%d, cert_flags=0x%x, socket_errno=%d",
                         event->error_handle->error_type,
                         (unsigned)event->error_handle->esp_tls_last_esp_err,
                         event->error_handle->esp_tls_stack_err,
                         (unsigned)event->error_handle->esp_tls_cert_verify_flags,
                         event->error_handle->esp_transport_sock_errno);
            }
            break;

        default:
            ESP_LOGI(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
            break;
    }
}
/**
 * @brief Start MQTT client
 *
 * This function initializes and starts the MQTT client with the specified configuration.
 * It registers an event handler to handle various MQTT events.
 * 开始MQTT客户端，配置远程broker连接远程MQTT服务器 注册事件处理函数
 * @return None
 */
void mqtt_start(void){

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

#include "mqtt_bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "device_state_manager.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

static const char *TAG = "MQTT_BRIDGE";

#define MQTT_BROKER_URI        "mqtts://api.hellum.dev:8883"
#define MQTT_TOPIC_MAX_LEN     96
#define MQTT_PAYLOAD_MAX_LEN   160
#define MQTT_CLIENT_ID_LEN     32

static esp_mqtt_client_handle_t s_client;
static char s_cmd_topic[MQTT_TOPIC_MAX_LEN];
static char s_state_topic[MQTT_TOPIC_MAX_LEN];
static char s_client_id[MQTT_CLIENT_ID_LEN];
static bool s_connected;
static bool s_started;

static void publish_endpoint_state(device_endpoint_t endpoint, bool on)
{
    if (!s_client || !s_connected) {
        return;
    }

    char payload[MQTT_PAYLOAD_MAX_LEN];
    const int written = snprintf(payload, sizeof(payload),
                                 "{\"device\":\"%s\",\"state\":\"%s\"}",
                                 device_state_manager_endpoint_name(endpoint),
                                 on ? "on" : "off");
    if (written < 0 || written >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "State payload truncated for %s",
                 device_state_manager_endpoint_name(endpoint));
        return;
    }

    const int msg_id = esp_mqtt_client_publish(s_client, s_state_topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to queue state publish: %s", payload);
    } else {
        ESP_LOGI(TAG, "Published %s", payload);
    }
}

static void publish_all_states(void)
{
    for (device_endpoint_t endpoint = 0; endpoint < DEVICE_ENDPOINT_COUNT; endpoint++) {
        publish_endpoint_state(endpoint, device_state_manager_get_power(endpoint));
    }
}

static void state_observer(device_endpoint_t endpoint, bool on, device_state_source_t source, void *ctx)
{
    (void)source;
    (void)ctx;
    publish_endpoint_state(endpoint, on);
}

static void handle_command_payload(const char *payload, int payload_len)
{
    if (!payload || payload_len <= 0 || payload_len >= MQTT_PAYLOAD_MAX_LEN) {
        ESP_LOGW(TAG, "Ignoring invalid MQTT payload length=%d", payload_len);
        return;
    }

    char json_buf[MQTT_PAYLOAD_MAX_LEN];
    memcpy(json_buf, payload, payload_len);
    json_buf[payload_len] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        ESP_LOGW(TAG, "Ignoring non-JSON MQTT command: %s", json_buf);
        return;
    }

    const cJSON *device = cJSON_GetObjectItem(root, "device");
    const cJSON *state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(device) || !cJSON_IsString(state)) {
        ESP_LOGW(TAG, "MQTT command missing device/state strings");
        cJSON_Delete(root);
        return;
    }

    device_endpoint_t endpoint;
    if (!device_state_manager_endpoint_from_name(device->valuestring, &endpoint)) {
        ESP_LOGW(TAG, "Unknown MQTT device: %s", device->valuestring);
        cJSON_Delete(root);
        return;
    }

    bool on;
    if (strcmp(state->valuestring, "on") == 0) {
        on = true;
    } else if (strcmp(state->valuestring, "off") == 0) {
        on = false;
    } else {
        ESP_LOGW(TAG, "Unknown MQTT state: %s", state->valuestring);
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "MQTT command %s -> %s", device->valuestring, state->valuestring);
    device_state_manager_set_power(endpoint, on, DEVICE_STATE_SOURCE_MQTT);
    cJSON_Delete(root);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Connected to broker");
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        publish_all_states();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Disconnected from broker");
        break;
    case MQTT_EVENT_DATA:
        if (event->topic && event->topic_len == (int)strlen(s_cmd_topic) &&
                strncmp(event->topic, s_cmd_topic, event->topic_len) == 0) {
            handle_command_payload(event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT transport error");
        break;
    default:
        break;
    }
}

esp_err_t mqtt_bridge_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), TAG, "Failed to read STA MAC");

    snprintf(s_client_id, sizeof(s_client_id), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "smarthome/device/%02x%02x%02x%02x%02x%02x/cmd",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_state_topic, sizeof(s_state_topic), "smarthome/device/%02x%02x%02x%02x%02x%02x/state",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = MQTT_BROKER_URI,
            },
            .verification = {
                .crt_bundle_attach = esp_crt_bundle_attach,
            },
        },
        .credentials = {
            .client_id = s_client_id,
        },
        .session = {
            .keepalive = 60,
        },
        .network = {
            .reconnect_timeout_ms = 5000,
            .timeout_ms = 10000,
        },
        .task = {
            .priority = 5,
            .stack_size = 6144,
        },
        .buffer = {
            .size = 1024,
            .out_size = 1024,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY,
                        mqtt_event_handler, NULL), TAG, "Failed to register MQTT handler");
    ESP_RETURN_ON_ERROR(device_state_manager_register_observer(state_observer, NULL),
                        TAG, "Failed to register MQTT observer");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "Failed to start MQTT client");

    s_started = true;
    ESP_LOGI(TAG, "MQTT started. cmd=%s state=%s", s_cmd_topic, s_state_topic);
    return ESP_OK;
}

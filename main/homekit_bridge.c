#include "homekit_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "device_state_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hap.h"
#include "hap_apple_chars.h"
#include "hap_apple_servs.h"

static const char *TAG = "HOMEKIT_BRIDGE";

#define HOMEKIT_TASK_NAME        "homekit"
#define HOMEKIT_TASK_STACK_SIZE  8192
#define HOMEKIT_TASK_PRIORITY    4
#define HOMEKIT_SETUP_CODE       "111-22-333"
#define HOMEKIT_SETUP_ID         "ES32"
#define SERVICE_PRIV(endpoint)   ((void *)(intptr_t)((endpoint) + 1))
#define SERVICE_ENDPOINT(priv)   ((device_endpoint_t)((intptr_t)(priv) - 1))

typedef struct {
    hap_char_t *power_char[DEVICE_ENDPOINT_COUNT];
    hap_char_t *outlet_in_use_char;
} homekit_ctx_t;

static homekit_ctx_t s_hk;
static bool s_started;

static int identify_accessory(hap_acc_t *accessory)
{
    (void)accessory;
    ESP_LOGI(TAG, "Accessory identify requested");
    return HAP_SUCCESS;
}

static bool is_power_characteristic(hap_char_t *characteristic)
{
    const char *uuid = hap_char_get_type_uuid(characteristic);
    return uuid && (!strcmp(uuid, HAP_CHAR_UUID_ON) || !strcmp(uuid, HAP_CHAR_UUID_ACTIVE));
}

static void update_homekit_power(device_endpoint_t endpoint, bool on)
{
    if (endpoint < 0 || endpoint >= DEVICE_ENDPOINT_COUNT || !s_hk.power_char[endpoint]) {
        return;
    }

    hap_val_t value = {
        .b = on,
    };
    hap_char_update_val(s_hk.power_char[endpoint], &value);

    if (endpoint == DEVICE_ENDPOINT_PLUG && s_hk.outlet_in_use_char) {
        hap_char_update_val(s_hk.outlet_in_use_char, &value);
    }
}

static void state_observer(device_endpoint_t endpoint, bool on, device_state_source_t source, void *ctx)
{
    (void)source;
    (void)ctx;
    update_homekit_power(endpoint, on);
}

static int service_write(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv)
{
    (void)write_priv;

    const device_endpoint_t endpoint = SERVICE_ENDPOINT(serv_priv);
    if (endpoint < 0 || endpoint >= DEVICE_ENDPOINT_COUNT) {
        return HAP_FAIL;
    }

    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; i++) {
        hap_write_data_t *write = &write_data[i];
        *(write->status) = HAP_STATUS_VAL_INVALID;

        if (is_power_characteristic(write->hc)) {
            const bool on = write->val.b;
            ESP_LOGI(TAG, "HomeKit command %s -> %s",
                     device_state_manager_endpoint_name(endpoint), on ? "on" : "off");

            if (device_state_manager_set_power(endpoint, on, DEVICE_STATE_SOURCE_HOMEKIT) == ESP_OK) {
                hap_char_update_val(write->hc, &write->val);
                if (endpoint == DEVICE_ENDPOINT_PLUG && s_hk.outlet_in_use_char) {
                    hap_char_update_val(s_hk.outlet_in_use_char, &write->val);
                }
                *(write->status) = HAP_STATUS_SUCCESS;
            } else {
                ret = HAP_FAIL;
            }
        } else {
            *(write->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;
        }
    }

    return ret;
}

static esp_err_t add_named_char(hap_serv_t *service, const char *name)
{
    hap_char_t *name_char = hap_char_name_create((char *)name);
    if (!name_char) {
        return ESP_ERR_NO_MEM;
    }
    return hap_serv_add_char(service, name_char) == HAP_SUCCESS ? ESP_OK : ESP_FAIL;
}

static hap_serv_t *create_light_service(device_endpoint_t endpoint, const char *display_name)
{
    hap_serv_t *service = hap_serv_lightbulb_create(device_state_manager_get_power(endpoint));
    if (!service) {
        return NULL;
    }

    if (add_named_char(service, display_name) != ESP_OK) {
        return NULL;
    }
    hap_serv_set_priv(service, SERVICE_PRIV(endpoint));
    hap_serv_set_write_cb(service, service_write);
    s_hk.power_char[endpoint] = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_ON);
    return service;
}

static hap_serv_t *create_fan_service(void)
{
    const bool on = device_state_manager_get_power(DEVICE_ENDPOINT_FAN);
    hap_serv_t *service = hap_serv_fan_v2_create(on ? 1 : 0);
    if (!service) {
        return NULL;
    }

    if (add_named_char(service, "Fan") != ESP_OK) {
        return NULL;
    }
    hap_serv_set_priv(service, SERVICE_PRIV(DEVICE_ENDPOINT_FAN));
    hap_serv_set_write_cb(service, service_write);
    s_hk.power_char[DEVICE_ENDPOINT_FAN] = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_ACTIVE);
    return service;
}

static hap_serv_t *create_plug_service(void)
{
    const bool on = device_state_manager_get_power(DEVICE_ENDPOINT_PLUG);
    hap_serv_t *service = hap_serv_outlet_create(on, on);
    if (!service) {
        return NULL;
    }

    if (add_named_char(service, "Plug") != ESP_OK) {
        return NULL;
    }
    hap_serv_set_priv(service, SERVICE_PRIV(DEVICE_ENDPOINT_PLUG));
    hap_serv_set_write_cb(service, service_write);
    s_hk.power_char[DEVICE_ENDPOINT_PLUG] = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_ON);
    s_hk.outlet_in_use_char = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_OUTLET_IN_USE);
    return service;
}

static esp_err_t add_service_checked(hap_acc_t *accessory, hap_serv_t *service)
{
    if (!accessory || !service) {
        return ESP_FAIL;
    }
    hap_acc_add_serv(accessory, service);
    return ESP_OK;
}

static void homekit_task(void *arg)
{
    (void)arg;

    hap_cfg_t hap_cfg;
    hap_get_config(&hap_cfg);
    hap_cfg.unique_param = UNIQUE_NAME;
    hap_set_config(&hap_cfg);

    if (hap_init(HAP_TRANSPORT_WIFI) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "hap_init failed");
        goto done;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[16];
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    hap_acc_cfg_t cfg = {
        .name = "Hellum Switchboard",
        .manufacturer = "Hellum",
        .model = "DualStackSwitchboard4",
        .serial_num = serial,
        .fw_rev = "1.0.0",
        .hw_rev = "1.0",
        .pv = "1.1.0",
        .identify_routine = identify_accessory,
        .cid = HAP_CID_SWITCH,
    };

    hap_acc_t *accessory = hap_acc_create(&cfg);
    if (!accessory) {
        ESP_LOGE(TAG, "Failed to create HomeKit accessory");
        goto done;
    }

    uint8_t product_data[] = { 'H', 'E', 'L', 'L', 'U', 'M', '4', 'R' };
    hap_acc_add_product_data(accessory, product_data, sizeof(product_data));
    hap_acc_add_wifi_transport_service(accessory, 0);

    if (add_service_checked(accessory, create_light_service(DEVICE_ENDPOINT_LIGHT1, "Light 1")) != ESP_OK ||
            add_service_checked(accessory, create_light_service(DEVICE_ENDPOINT_LIGHT2, "Light 2")) != ESP_OK ||
            add_service_checked(accessory, create_fan_service()) != ESP_OK ||
            add_service_checked(accessory, create_plug_service()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create one or more HomeKit services");
        hap_acc_delete(accessory);
        goto done;
    }

    hap_add_accessory(accessory);
    hap_set_setup_code(HOMEKIT_SETUP_CODE);
    hap_set_setup_id(HOMEKIT_SETUP_ID);
    hap_enable_mfi_auth(HAP_MFI_AUTH_HW);

    if (device_state_manager_register_observer(state_observer, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register HomeKit state observer");
        hap_acc_delete(accessory);
        goto done;
    }

    if (hap_start() != HAP_SUCCESS) {
        ESP_LOGE(TAG, "hap_start failed");
        hap_acc_delete(accessory);
        goto done;
    }

    ESP_LOGI(TAG, "HomeKit started with setup code %s", HOMEKIT_SETUP_CODE);

done:
    vTaskDelete(NULL);
}

esp_err_t homekit_bridge_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(homekit_task, HOMEKIT_TASK_NAME, HOMEKIT_TASK_STACK_SIZE,
                                NULL, HOMEKIT_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}

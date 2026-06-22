#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVICE_ENDPOINT_LIGHT1 = 0,
    DEVICE_ENDPOINT_LIGHT2,
    DEVICE_ENDPOINT_FAN,
    DEVICE_ENDPOINT_PLUG,
    DEVICE_ENDPOINT_COUNT,
} device_endpoint_t;

typedef enum {
    DEVICE_STATE_SOURCE_BOOT = 0,
    DEVICE_STATE_SOURCE_PHYSICAL_SWITCH,
    DEVICE_STATE_SOURCE_HOMEKIT,
    DEVICE_STATE_SOURCE_MQTT,
} device_state_source_t;

typedef void (*device_state_observer_t)(
        device_endpoint_t endpoint,
        bool on,
        device_state_source_t source,
        void *ctx);

esp_err_t device_state_manager_init(void);
esp_err_t device_state_manager_register_observer(device_state_observer_t observer, void *ctx);
esp_err_t device_state_manager_set_power(device_endpoint_t endpoint, bool on, device_state_source_t source);
esp_err_t device_state_manager_toggle(device_endpoint_t endpoint, device_state_source_t source);
bool device_state_manager_get_power(device_endpoint_t endpoint);
const char *device_state_manager_endpoint_name(device_endpoint_t endpoint);
bool device_state_manager_endpoint_from_name(const char *name, device_endpoint_t *endpoint);

#ifdef __cplusplus
}
#endif

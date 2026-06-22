#include "device_state_manager.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "STATE_MANAGER";

#define RELAY_OFF_LEVEL             1
#define RELAY_ON_LEVEL              0
#define SWITCH_DEBOUNCE_MS          50
#define SWITCH_TASK_STACK_SIZE      3072
#define SWITCH_TASK_PRIORITY        6
#define MAX_STATE_OBSERVERS         4

typedef struct {
    const char *name;
    gpio_num_t relay_gpio;
    gpio_num_t switch_gpio;
} endpoint_hw_t;

typedef struct {
    device_endpoint_t endpoint;
} switch_event_t;

typedef struct {
    device_state_observer_t cb;
    void *ctx;
} observer_slot_t;

static const endpoint_hw_t s_hw[DEVICE_ENDPOINT_COUNT] = {
    [DEVICE_ENDPOINT_LIGHT1] = { .name = "light1", .relay_gpio = GPIO_NUM_13, .switch_gpio = GPIO_NUM_34 },
    [DEVICE_ENDPOINT_LIGHT2] = { .name = "light2", .relay_gpio = GPIO_NUM_26, .switch_gpio = GPIO_NUM_35 },
    [DEVICE_ENDPOINT_FAN]    = { .name = "fan",    .relay_gpio = GPIO_NUM_14, .switch_gpio = GPIO_NUM_36 },
    [DEVICE_ENDPOINT_PLUG]   = { .name = "plug",   .relay_gpio = GPIO_NUM_27, .switch_gpio = GPIO_NUM_39 },
};

static bool s_power[DEVICE_ENDPOINT_COUNT];
static int s_switch_stable_level[DEVICE_ENDPOINT_COUNT];
static observer_slot_t s_observers[MAX_STATE_OBSERVERS];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_switch_queue;
static bool s_initialized;

static bool endpoint_valid(device_endpoint_t endpoint)
{
    return endpoint >= 0 && endpoint < DEVICE_ENDPOINT_COUNT;
}

static void notify_observers(device_endpoint_t endpoint, bool on, device_state_source_t source)
{
    observer_slot_t snapshot[MAX_STATE_OBSERVERS];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(snapshot, s_observers, sizeof(snapshot));
    xSemaphoreGive(s_lock);

    for (size_t i = 0; i < MAX_STATE_OBSERVERS; i++) {
        if (snapshot[i].cb) {
            snapshot[i].cb(endpoint, on, source, snapshot[i].ctx);
        }
    }
}

static void apply_relay_level(device_endpoint_t endpoint, bool on)
{
    gpio_set_level(s_hw[endpoint].relay_gpio, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

static esp_err_t configure_relay_output(gpio_num_t gpio)
{
    gpio_reset_pin(gpio);

    /*
     * Active-low relay boards can click if an output briefly floats low.
     * Latch the output register HIGH before switching the pad into output mode.
     */
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio, RELAY_OFF_LEVEL), TAG, "Failed to pre-latch relay GPIO %d", gpio);
    ESP_RETURN_ON_ERROR(gpio_set_direction(gpio, GPIO_MODE_OUTPUT), TAG, "Failed to set relay GPIO %d output", gpio);
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio, RELAY_OFF_LEVEL), TAG, "Failed to keep relay GPIO %d off", gpio);
    return ESP_OK;
}

static esp_err_t configure_switch_input(gpio_num_t gpio)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    return gpio_config(&io_conf);
}

static void IRAM_ATTR switch_isr_handler(void *arg)
{
    const device_endpoint_t endpoint = (device_endpoint_t)(intptr_t)arg;
    switch_event_t event = { .endpoint = endpoint };
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (s_switch_queue) {
        xQueueSendFromISR(s_switch_queue, &event, &higher_priority_task_woken);
    }
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void switch_task(void *arg)
{
    switch_event_t event;

    while (true) {
        if (xQueueReceive(s_switch_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!endpoint_valid(event.endpoint)) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SWITCH_DEBOUNCE_MS));

        const int level = gpio_get_level(s_hw[event.endpoint].switch_gpio);
        if (level == s_switch_stable_level[event.endpoint]) {
            continue;
        }

        s_switch_stable_level[event.endpoint] = level;
        ESP_LOGI(TAG, "Physical switch edge on %s, stable level=%d",
                 s_hw[event.endpoint].name, level);
        device_state_manager_toggle(event.endpoint, DEVICE_STATE_SOURCE_PHYSICAL_SWITCH);
    }
}

esp_err_t device_state_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_switch_queue = xQueueCreate(16, sizeof(switch_event_t));
    if (!s_switch_queue) {
        return ESP_ERR_NO_MEM;
    }

    for (device_endpoint_t endpoint = 0; endpoint < DEVICE_ENDPOINT_COUNT; endpoint++) {
        s_power[endpoint] = false;
        ESP_RETURN_ON_ERROR(configure_relay_output(s_hw[endpoint].relay_gpio), TAG,
                            "Relay init failed for %s", s_hw[endpoint].name);
    }

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to install GPIO ISR service");
    }

    for (device_endpoint_t endpoint = 0; endpoint < DEVICE_ENDPOINT_COUNT; endpoint++) {
        ESP_RETURN_ON_ERROR(configure_switch_input(s_hw[endpoint].switch_gpio), TAG,
                            "Switch init failed for %s", s_hw[endpoint].name);
        s_switch_stable_level[endpoint] = gpio_get_level(s_hw[endpoint].switch_gpio);
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_hw[endpoint].switch_gpio, switch_isr_handler,
                            (void *)(intptr_t)endpoint), TAG,
                            "Switch ISR add failed for %s", s_hw[endpoint].name);
    }

    BaseType_t task_ok = xTaskCreate(switch_task, "switch_edges", SWITCH_TASK_STACK_SIZE,
                                     NULL, SWITCH_TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Relay board initialized off; switch inputs expect external pull-ups");
    return ESP_OK;
}

esp_err_t device_state_manager_register_observer(device_state_observer_t observer, void *ctx)
{
    if (!observer || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < MAX_STATE_OBSERVERS; i++) {
        if (!s_observers[i].cb) {
            s_observers[i].cb = observer;
            s_observers[i].ctx = ctx;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t device_state_manager_set_power(device_endpoint_t endpoint, bool on, device_state_source_t source)
{
    if (!endpoint_valid(endpoint) || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_power[endpoint] != on) {
        s_power[endpoint] = on;
        apply_relay_level(endpoint, on);
        changed = true;
    }
    xSemaphoreGive(s_lock);

    if (changed) {
        ESP_LOGI(TAG, "%s -> %s", s_hw[endpoint].name, on ? "on" : "off");
        notify_observers(endpoint, on, source);
    }
    return ESP_OK;
}

esp_err_t device_state_manager_toggle(device_endpoint_t endpoint, device_state_source_t source)
{
    if (!endpoint_valid(endpoint) || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    bool new_state;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    new_state = !s_power[endpoint];
    xSemaphoreGive(s_lock);

    return device_state_manager_set_power(endpoint, new_state, source);
}

bool device_state_manager_get_power(device_endpoint_t endpoint)
{
    if (!endpoint_valid(endpoint) || !s_lock) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool on = s_power[endpoint];
    xSemaphoreGive(s_lock);
    return on;
}

const char *device_state_manager_endpoint_name(device_endpoint_t endpoint)
{
    if (!endpoint_valid(endpoint)) {
        return "unknown";
    }
    return s_hw[endpoint].name;
}

bool device_state_manager_endpoint_from_name(const char *name, device_endpoint_t *endpoint)
{
    if (!name || !endpoint) {
        return false;
    }

    for (device_endpoint_t i = 0; i < DEVICE_ENDPOINT_COUNT; i++) {
        if (strcmp(name, s_hw[i].name) == 0) {
            *endpoint = i;
            return true;
        }
    }
    return false;
}

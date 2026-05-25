#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "cJSON.h"

static const char *TAG = "ESP_CORE";

#define BUTTON_GPIO             GPIO_NUM_32  // Rarely conflicts, input/output capable
#define FIRMWARE_VERSION        "1.0.0"
#define OTA_CHECK_URL           "https://ota.hellum.dev/smart_switch/check"
#define PROV_DEVICE_NAME_PREFIX "PROV_"
#define PROV_POP_NAMESPACE      "prov"
#define PROV_POP_KEY            "pop"
#define PROV_POP_LEN            13
#define PROV_SERVICE_NAME_LEN   20

/*
 * Product provisioning service UUID (canonical form):
 * 55cc035e-fb27-4f80-be02-3c60828b7451
 *
 * ESP-IDF expects the UUID as raw bytes in LSB -> MSB order.
 */
static const uint8_t PROV_SERVICE_UUID[16] = {
    0x51, 0x74, 0x8b, 0x82, 0x60, 0x3c, 0x02, 0xbe,
    0x80, 0x4f, 0x27, 0xfb, 0x5e, 0x03, 0xcc, 0x55
};
static const char *PROV_SERVICE_UUID_STR = "55cc035e-fb27-4f80-be02-3c60828b7451";

// --- Global State ---
static bool is_provisioned = false;
static wifi_config_t backup_wifi_config;
static bool has_backup_config = false;
static uint8_t button_press_count = 0;
static TimerHandle_t button_timer = NULL;

// Provided by CMake/build system for HTTPS verification
extern const uint8_t server_cert_pem_start[] asm("_binary_server_root_ca_pem_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_server_root_ca_pem_end");

// --- Prototypes ---
void check_ota_task(void *pvParameter);
void start_ble_provisioning(void);
static esp_err_t get_or_create_pop(char *out_pop, size_t out_len);
static void build_service_name(char *out_name, size_t out_len, const uint8_t mac[6]);
static void generate_random_pop(char *out_pop, size_t out_len);

// --- Button Fallback Logic ---
static void button_timer_cb(TimerHandle_t xTimer) {
    // Reset press count if 5 presses didn't happen within the timeout (3 seconds)
    button_press_count = 0;
}

static void IRAM_ATTR button_isr_handler(void* arg) {
    static uint32_t last_isr_time = 0;
    uint32_t current_time = xTaskGetTickCountFromISR();

    // 200ms debounce
    if (current_time - last_isr_time > pdMS_TO_TICKS(200)) {
        button_press_count++;
        last_isr_time = current_time;

        BaseType_t higherPriorityTaskWoken = pdFALSE;
        xTimerResetFromISR(button_timer, &higherPriorityTaskWoken);

        if (button_press_count >= 5) {
            button_press_count = 0;
            // Trigger Network Reset
            esp_wifi_get_config(WIFI_IF_STA, &backup_wifi_config);
            has_backup_config = true;
            ESP_DRAM_LOGI(TAG, "5 presses detected. Triggering BLE Prov. Creds backed up to RAM.");

            // Disconnect and wipe NVS config to force provisioning mode
            esp_wifi_disconnect();
            wifi_prov_mgr_reset_provisioning();
            esp_restart(); // Restart cleanly into provisioning mode
        }
        if (higherPriorityTaskWoken) portYIELD_FROM_ISR();
    }
}

static void build_service_name(char *out_name, size_t out_len, const uint8_t mac[6]) {
    snprintf(
        out_name,
        out_len,
        "%s%02X%02X%02X",
        PROV_DEVICE_NAME_PREFIX,
        mac[3],
        mac[4],
        mac[5]
    );
}

static void generate_random_pop(char *out_pop, size_t out_len) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const size_t pop_len = PROV_POP_LEN - 1;
    const size_t alphabet_len = sizeof(alphabet) - 1;

    if (out_len < PROV_POP_LEN) {
        return;
    }

    for (size_t i = 0; i < pop_len; i++) {
        out_pop[i] = alphabet[esp_random() % alphabet_len];
    }
    out_pop[pop_len] = '\0';
}

static esp_err_t get_or_create_pop(char *out_pop, size_t out_len) {
    if (!out_pop || out_len < PROV_POP_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PROV_POP_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_len = out_len;
    err = nvs_get_str(handle, PROV_POP_KEY, out_pop, &required_len);
    if (err == ESP_OK) {
        nvs_close(handle);
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    generate_random_pop(out_pop, out_len);
    err = nvs_set_str(handle, PROV_POP_KEY, out_pop);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// --- WiFi & Provisioning Events ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started. Connect via BLE.");
                break;
            case WIFI_PROV_CRED_RECV:
                ESP_LOGI(TAG, "Received Wi-Fi credentials");
                break;
            case WIFI_PROV_CRED_FAIL:
                ESP_LOGE(TAG, "Failed to connect to provided AP");
                if (has_backup_config) {
                    ESP_LOGW(TAG, "New creds failed. Restoring from RAM fallback...");
                    esp_wifi_set_config(WIFI_IF_STA, &backup_wifi_config);
                    has_backup_config = false;
                    esp_wifi_connect();
                } else {
                    wifi_prov_mgr_reset_provisioning();
                }
                break;
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                has_backup_config = false; // Clear backup, new creds are good
                break;
            case WIFI_PROV_END:
                wifi_prov_mgr_deinit();
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // --- ANTI-BRICKING VALIDATION ---
        // If we reach here, the network works. Cancel rollback.
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGI(TAG, "First boot after OTA successful! Confirming partition.");
                esp_ota_mark_app_valid_cancel_rollback();
            }
        }

        // Start OTA Check
        xTaskCreate(&check_ota_task, "ota_task", 8192, NULL, 5, NULL);
    }
}

void start_ble_provisioning(void) {
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));

    static char service_name[PROV_SERVICE_NAME_LEN];
    static char pop[PROV_POP_LEN];

    build_service_name(service_name, sizeof(service_name), mac);

    ESP_ERROR_CHECK(wifi_prov_scheme_ble_set_service_uuid((uint8_t *)PROV_SERVICE_UUID));

    esp_err_t pop_err = get_or_create_pop(pop, sizeof(pop));
    if (pop_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load persisted PoP (%s). Using fallback PoP from MAC suffix.", esp_err_to_name(pop_err));
        snprintf(pop, sizeof(pop), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
    }

    ESP_LOGI(TAG, "Provisioning identity: name=%s, service_uuid=%s", service_name, PROV_SERVICE_UUID_STR);
    ESP_LOGI(TAG, "Provisioning PoP: %s", pop);

    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    wifi_prov_mgr_start_provisioning(security, pop, service_name, NULL);
}

// --- Secure OTA Implementation ---
void check_ota_task(void *pvParameter) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    char check_url[200];
    snprintf(check_url, sizeof(check_url),
             "%s?mac=%02x:%02x:%02x:%02x:%02x:%02x&ver=%s",
             OTA_CHECK_URL,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             FIRMWARE_VERSION);

    esp_http_client_config_t config = {
        .url = check_url,
        .cert_pem = (const char *)server_cert_pem_start,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_fetch_headers(client);

    #define OTA_CHECK_BUF_SIZE 1024
    static char response_buf[OTA_CHECK_BUF_SIZE];
    int total_read = 0;
    int read_len = 0;

    while (total_read < (OTA_CHECK_BUF_SIZE - 1)) {
        read_len = esp_http_client_read(
            client,
            response_buf + total_read,
            OTA_CHECK_BUF_SIZE - 1 - total_read
        );
        if (read_len <= 0) {
            break;
        }
        total_read += read_len;
    }
    response_buf[total_read] = '\0';

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "OTA check HTTP %d, body: %s", status, response_buf);

    if (status == 200 && total_read > 0) {
        cJSON *json = cJSON_Parse(response_buf);
        if (json != NULL) {
            cJSON *update_av = cJSON_GetObjectItem(json, "update_available");
            if (cJSON_IsTrue(update_av)) {
                cJSON *fw_url = cJSON_GetObjectItem(json, "firmware_url");
                if (fw_url && fw_url->valuestring) {
                    ESP_LOGI(TAG, "Update available! Downloading from %s", fw_url->valuestring);

                    esp_http_client_config_t ota_client_config = {
                        .url = fw_url->valuestring,
                        .cert_pem = (const char *)server_cert_pem_start,
                        .keep_alive_enable = true,
                        .timeout_ms = 60000,
                    };
                    esp_https_ota_config_t ota_config = {
                        .http_config = &ota_client_config,
                    };

                    ESP_LOGI(TAG, "Starting OTA...");
                    esp_err_t ota_ret = esp_https_ota(&ota_config);
                    if (ota_ret == ESP_OK) {
                        ESP_LOGI(TAG, "OTA success. Rebooting...");
                        esp_http_client_cleanup(client);
                        esp_restart();
                    } else {
                        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ota_ret));
                    }
                }
            } else {
                ESP_LOGI(TAG, "Firmware is up to date.");
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON response");
        }
    } else {
        ESP_LOGE(TAG, "Bad HTTP response: status=%d bytes_read=%d", status, total_read);
    }

    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

void app_main(void) {
    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Setup Netif & Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. Register Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 4. Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 5. Check if Provisioned
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&is_provisioned));

    // 6. Setup Hardware Reset Button (GPIO 32)
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    button_timer = xTimerCreate("btn_tmr", pdMS_TO_TICKS(3000), pdFALSE, NULL, button_timer_cb);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    // 7. Branch Logic
    if (!is_provisioned) {
        ESP_LOGI(TAG, "Starting BLE Provisioning");
        start_ble_provisioning();
    } else {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi");
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }
}

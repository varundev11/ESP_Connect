idf_component_register(SRCS "homekit_bridge.c" "mqtt_bridge.c" "device_state_manager.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi wifi_provisioning esp_http_client esp_https_ota json driver app_update mqtt esp-tls esp_hap_core esp_hap_apple_profiles
                    EMBED_TXTFILES "../server_certs/server_root_ca.pem")

idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi wifi_provisioning esp_http_client esp_https_ota json esp_driver_gpio app_update
                    EMBED_TXTFILES "../server_certs/server_root_ca.pem")

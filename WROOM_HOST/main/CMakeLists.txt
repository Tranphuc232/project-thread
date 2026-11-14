idf_component_register(SRCS "esp_ot_br.c" "sensor_client.c" "actuator_client.c" "device_registry.c"
                       PRIV_REQUIRES esp_psram openthread esp_coex esp_wifi nvs_flash esp_timer freertos json
                       INCLUDE_DIRS ".")

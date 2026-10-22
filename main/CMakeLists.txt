idf_component_register(SRCS "esp_ot_br.c"
                       PRIV_REQUIRES esp_psram openthread esp_coex esp_wifi nvs_flash esp_netif esp_event mdns vfs ot_examples_br ot_examples_common
                       INCLUDE_DIRS ".")

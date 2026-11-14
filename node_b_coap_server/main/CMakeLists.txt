idf_component_register(SRCS "node_b_main.c"
                       PRIV_REQUIRES openthread esp_netif esp_event nvs_flash vfs esp_driver_gpio driver esp_timer ot_examples_common
                       INCLUDE_DIRS ".")

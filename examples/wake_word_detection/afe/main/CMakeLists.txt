set(requires
    hardware_driver
    player
    esp_wifi
    esp_event
    esp_netif
    nvs_flash
    lwip
    )

idf_component_register(SRCS main.c
                       REQUIRES ${requires})


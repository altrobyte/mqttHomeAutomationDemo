idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                        REQUIRES esp_wifi esp_event nvs_flash lwip mqtt driver esp_adc
                    )

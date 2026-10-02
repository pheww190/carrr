idf_component_register(
    SRCS "main.c" "motor.c" "control.c" "ble_srv.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver esp_timer
)

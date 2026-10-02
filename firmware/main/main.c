/*
 * main.c  —  BLE Rover (ESP-IDF)
 *
 * Boot order:
 *   nvs -> motor driver -> control loop -> BLE GATT server
 *
 * The control task owns the motors; the BLE layer only feeds it commands.
 */
#include "motor.h"
#include "control.h"
#include "ble_srv.h"

#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "ROVER";

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    motor_init();       /* motors off, pins configured          */
    control_init();     /* starts the 50 Hz control task        */

    ESP_LOGI(TAG, "BLE Rover starting");
    ble_srv_init();     /* starts advertising as "RoverBLE"     */

    ESP_LOGI(TAG, "ready - connect with the Rover app");
}

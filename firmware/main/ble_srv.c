/*
 * ble_srv.c
 * ---------
 * Bluedroid GATT server for the rover.
 *
 *   Service a1b2c3d4-0001-...   (advertised so the app can filter)
 *     CMD  a1b2c3d4-0002-...  Write / Write-No-Response  4-byte control frame
 *     TEL  a1b2c3d4-0003-...  Notify                     6-byte telemetry
 *     CFG  a1b2c3d4-0004-...  Write                      6-byte tuning block
 *
 * No Wi-Fi, no HTTP: BLE only, so the radio cost is minimal.
 */
#include "ble_srv.h"
#include "control.h"
#include "rover_proto.h"

#include <string.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"          /* esp_bt_dev_get_address() */
#include "esp_bt_defs.h"
#include "esp_random.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG         "BLE_SRV"
#define DEVICE_NAME "RoverBLE"

/* Fixed pairing passkey. 6 digits, and it never changes, so you don't need
 * the serial monitor to read it. Type this on the phone when it asks. */
#define PAIRING_PASSKEY 123654U

/* ---------------- attribute table indices ---------------- */
enum {
    IDX_SVC,
    IDX_CMD_CHAR,
    IDX_CMD_VAL,
    IDX_TEL_CHAR,
    IDX_TEL_VAL,
    IDX_TEL_CCC,
    IDX_CFG_CHAR,
    IDX_CFG_VAL,
    IDX_NB,
};

/* ---------------- UUIDs ---------------- */
static const uint8_t svc_uuid[16] = { ROVER_SVC_UUID };
static const uint8_t cmd_uuid[16] = { ROVER_CMD_UUID };
static const uint8_t tel_uuid[16] = { ROVER_TEL_UUID };
static const uint8_t cfg_uuid[16] = { ROVER_CFG_UUID };

static uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static uint16_t char_declare_uuid    = ESP_GATT_UUID_CHAR_DECLARE;
static uint16_t ccc_uuid             = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static uint8_t prop_write  = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static uint8_t prop_notify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;

/* ---------------- values ---------------- */
static uint8_t cmd_val[sizeof(rover_cmd_t)] = { 0 };
static uint8_t tel_val[sizeof(rover_tel_t)] = { 0 };
static uint8_t cfg_val[sizeof(rover_cfg_t)] = { 0 };
static uint8_t ccc_val[2]                   = { 0, 0 };
static uint32_t pairing_passkey;

/* ---------------- attribute database ----------------
 * NOTE: for every row, max_length must be >= length. The service
 * declaration carries a 128-bit UUID, so both are 16 (not 2).
 */
static const esp_gatts_attr_db_t gatts_db[IDX_NB] = {
    [IDX_SVC] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
          sizeof(svc_uuid), sizeof(svc_uuid), (uint8_t *)svc_uuid }
    },

    [IDX_CMD_CHAR] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
          sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&prop_write }
    },
    [IDX_CMD_VAL] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_128, (uint8_t *)cmd_uuid, ESP_GATT_PERM_WRITE_ENC_MITM,
          sizeof(rover_cmd_t), sizeof(rover_cmd_t), cmd_val }
    },

    [IDX_TEL_CHAR] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
          sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&prop_notify }
    },
    [IDX_TEL_VAL] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_128, (uint8_t *)tel_uuid, ESP_GATT_PERM_READ,
          sizeof(rover_tel_t), sizeof(rover_tel_t), tel_val }
    },
    [IDX_TEL_CCC] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&ccc_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
          sizeof(uint16_t), sizeof(ccc_val), ccc_val }
    },

    [IDX_CFG_CHAR] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
          sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&prop_write }
    },
    [IDX_CFG_VAL] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_128, (uint8_t *)cfg_uuid, ESP_GATT_PERM_WRITE_ENC_MITM,
          sizeof(rover_cfg_t), sizeof(rover_cfg_t), cfg_val }
    },
};

/* ---------------- advertising payloads ----------------
 * Legacy advertising is capped at 31 bytes. flags(3) + 128-bit service
 * UUID(18) = 21 bytes; adding the 8-char name pushed it over the limit and
 * the controller silently truncated the packet, so the device was invisible
 * to scanners. Fix: ADV carries flags + UUID only, and the name rides in the
 * SCAN RESPONSE.
 *
 * The ADV packet uses the structured helper; the scan response uses the
 * RAW helper (in this IDF the structured scan-rsp helper is not declared,
 * only esp_ble_gap_config_scan_rsp_data_raw()).
 */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp        = false,
    .include_name        = false,   /* the name goes in the scan response */
    .include_txpower     = false,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x0000,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = sizeof(svc_uuid),
    .p_service_uuid      = (uint8_t *)svc_uuid,
    .flag                = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static uint8_t scan_rsp_raw[] = {
    0x09, 0x09,                 /* Complete Local Name, 9 bytes */
    'R', 'o', 'v', 'e', 'r', 'B', 'L', 'E'
};

/* ---------------- advertising params ---------------- */
static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x20,      /* 20 ms */
    .adv_int_max       = 0x40,      /* 40 ms */
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ---------------- runtime state ---------------- */
static esp_gatt_if_t gatts_if_   = ESP_GATT_IF_NONE;
static uint16_t      handles_[IDX_NB];
static uint16_t      conn_id_    = 0;
static bool          connected_  = false;
static bool          notify_on_  = false;
static uint16_t      tel_handle_ = 0;
static uint32_t      last_cmd_log_ms = 0;

/* ---------------- local MAC ---------------- */
static void log_local_mac(void)
{
    uint8_t mac[ESP_BD_ADDR_LEN] = { 0 };
    const void *p = (const void *)esp_bt_dev_get_address();
    if (p == NULL) {
        return;
    }
    memcpy(mac, p, ESP_BD_ADDR_LEN);
    ESP_LOGI(TAG, "BLE MAC %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---------------- GAP ---------------- */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "pairing passkey: %06u",
                 (unsigned)param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "BLE pairing complete");
        } else {
            ESP_LOGE(TAG, "BLE pairing failed: 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        /* ADV data set; now set the scan response that carries the name */
        esp_ble_gap_config_scan_rsp_data_raw(scan_rsp_raw,
                                             (uint8_t)sizeof(scan_rsp_raw));
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&adv_params);
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "advertising start failed");
        } else {
            ESP_LOGI(TAG, "advertising as \"%s\"", DEVICE_NAME);
            log_local_mac();
        }
        break;

    default:
        break;
    }
}

/* ---------------- GATTS ---------------- */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTS_REG_EVT:
        gatts_if_ = gatts_if;
        esp_ble_gap_set_device_name(DEVICE_NAME);
        esp_ble_gap_config_adv_data(&adv_data);
        esp_ble_gatts_create_attr_tab(gatts_db, gatts_if, IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "create attr tab failed: 0x%x", param->add_attr_tab.status);
            break;
        }
        if (param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "unexpected handle count %d", param->add_attr_tab.num_handle);
            break;
        }
        memcpy(handles_, param->add_attr_tab.handles, sizeof(handles_));
        tel_handle_ = handles_[IDX_TEL_VAL];
        esp_ble_gatts_start_service(handles_[IDX_SVC]);
        break;

    case ESP_GATTS_CONNECT_EVT: {
        conn_id_   = param->connect.conn_id;
        connected_ = true;
        notify_on_ = false;
        control_set_link(true);

        /* ask for a fast connection interval for low control latency */
        esp_ble_conn_update_params_t cp = {
            .min_int = 0x0006,   /* 7.5 ms */
            .max_int = 0x0010,   /* 20 ms  */
            .latency = 0,
            .timeout = 400,      /* 4 s    */
            .bda     = { 0 },
        };
        memcpy(cp.bda, param->connect.remote_bda, sizeof(cp.bda));
        esp_ble_gap_update_conn_params(&cp);

        ESP_LOGI(TAG, "connected");
        break;
    }

    case ESP_GATTS_DISCONNECT_EVT:
        connected_ = false;
        notify_on_ = false;
        control_set_link(false);
        esp_ble_gap_start_advertising(&adv_params);
        ESP_LOGI(TAG, "disconnected");
        break;

    case ESP_GATTS_WRITE_EVT: {
        uint16_t h = param->write.handle;

        if (h == handles_[IDX_CMD_VAL] && param->write.len == sizeof(rover_cmd_t)) {
            rover_cmd_t c;
            memcpy(&c, param->write.value, sizeof(c));
            control_set_cmd(&c);

            /* diagnostic: prove the control frames are actually arriving */
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - last_cmd_log_ms >= 1000) {
                last_cmd_log_ms = now;
                ESP_LOGI(TAG, "cmd rx: thr=%d steer=%d flags=0x%02x",
                         c.throttle, c.steering, c.flags);
            }

        } else if (h == handles_[IDX_CFG_VAL] && param->write.len == sizeof(rover_cfg_t)) {
            rover_cfg_t c;
            memcpy(&c, param->write.value, sizeof(c));
            control_set_cfg(&c);
            ESP_LOGI(TAG, "config updated");

        } else if (h == handles_[IDX_TEL_CCC] && param->write.len == 2) {
            notify_on_ = (param->write.value[0] & 0x01) != 0;
        }

        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK, NULL);
        }
        break;
    }

    default:
        break;
    }
}

/* ---------------- telemetry notifier (10 Hz) ---------------- */
static void tel_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (connected_ && notify_on_) {
            rover_tel_t t;
            control_get_tel(&t);
            esp_ble_gatts_send_indicate(gatts_if_, conn_id_, tel_handle_,
                                        sizeof(t), (uint8_t *)&t, false);
        }
    }
}

/* ---------------- init ---------------- */
void ble_srv_init(void)
{
    /* BLE only -> give back the Classic BT controller memory */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    pairing_passkey = PAIRING_PASSKEY;   /* fixed: 123654 */
    /* Secure Connections + bonding + MITM. Modern Android strongly prefers
     * LE Secure Connections; with legacy pairing it will often report the
     * pairing as successful but not persist the bond, which breaks both the
     * reconnect and the "paired devices" list. */
    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;   /* SC + MITM + bonding */
    uint8_t io_cap = ESP_IO_CAP_OUT;
    uint8_t key_size = 16;
    uint8_t key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                                    &auth_req, sizeof(auth_req)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                                    &io_cap, sizeof(io_cap)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                                    &key_size, sizeof(key_size)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
                                                    &key_mask, sizeof(key_mask)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
                                                    &key_mask, sizeof(key_mask)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY,
                                                    &pairing_passkey,
                                                    sizeof(pairing_passkey)));
    ESP_LOGI(TAG, "pairing passkey (fixed): %06u", (unsigned)pairing_passkey);

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));

    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(247));

    log_local_mac();

    xTaskCreate(tel_task, "ble_tel", 3072, NULL, 4, NULL);
}

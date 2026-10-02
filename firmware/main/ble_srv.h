/*
 * ble_srv.h
 * ---------
 * Bluedroid GATT server: exposes CMD (write), TEL (notify) and CFG (write).
 * Brings up the controller, registers the service and starts advertising.
 */
#pragma once

void ble_srv_init(void);

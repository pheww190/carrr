/*
 * rover_proto.h
 * ---------------
 * Wire protocol shared between the ESP32 firmware and the Android app.
 * Everything is fixed-size and packed so the frame can be memcpy'd
 * straight out of the GATT write buffer.
 */
#pragma once

#include <stdint.h>

/* =====================================================
 * BLE UUIDs  (128-bit, stored little-endian / LSB first,
 * which is the order ESP-IDF expects for raw 128-bit UUIDs)
 *
 *   Service : a1b2c3d4-0001-4a5b-8c7d-1e2f3a4b5c6d
 *   CMD     : a1b2c3d4-0002-4a5b-8c7d-1e2f3a4b5c6d
 *   TEL     : a1b2c3d4-0003-4a5b-8c7d-1e2f3a4b5c6d
 *   CFG     : a1b2c3d4-0004-4a5b-8c7d-1e2f3a4b5c6d
 * ===================================================== */
#define ROVER_SVC_UUID  0x6D,0x5C,0x4B,0x3A,0x2F,0x1E,0x7D,0x8C,0x5B,0x4A,0x01,0x00,0xD4,0xC3,0xB2,0xA1
#define ROVER_CMD_UUID  0x6D,0x5C,0x4B,0x3A,0x2F,0x1E,0x7D,0x8C,0x5B,0x4A,0x02,0x00,0xD4,0xC3,0xB2,0xA1
#define ROVER_TEL_UUID  0x6D,0x5C,0x4B,0x3A,0x2F,0x1E,0x7D,0x8C,0x5B,0x4A,0x03,0x00,0xD4,0xC3,0xB2,0xA1
#define ROVER_CFG_UUID  0x6D,0x5C,0x4B,0x3A,0x2F,0x1E,0x7D,0x8C,0x5B,0x4A,0x04,0x00,0xD4,0xC3,0xB2,0xA1

/* =====================================================
 * Control frame  (phone -> rover)  4 bytes
 * ===================================================== */
#define ROVER_CMD_FLAG_BRAKE  0x01   /* request a hard stop (fast decel) */
#define ROVER_CMD_FLAG_ESTOP  0x02   /* emergency stop, immediate neutral */

typedef struct __attribute__((packed)) {
    int8_t  throttle;   /* -100 .. +100   ( + = forward )   */
    int8_t  steering;   /* -100 .. +100   ( + = right   )   */
    uint8_t flags;      /* ROVER_CMD_FLAG_*                 */
    uint8_t seq;        /* rolling counter, echoed in telemetry */
} rover_cmd_t;

/* =====================================================
 * Telemetry  (rover -> phone, notify)  6 bytes
 * ===================================================== */
typedef enum {
    ROVER_STATE_IDLE     = 0,
    ROVER_STATE_DRIVING  = 1,
    ROVER_STATE_BRAKING  = 2,
    ROVER_STATE_FAILSAFE = 3,
} rover_state_t;

typedef struct __attribute__((packed)) {
    int16_t left;       /* actual output, -255 .. +255 */
    int16_t right;      /* actual output, -255 .. +255 */
    uint8_t state;      /* rover_state_t               */
    uint8_t seq;        /* echo of last accepted cmd   */
} rover_tel_t;

/* =====================================================
 * Tunable config  (phone -> rover, CFG characteristic)
 * Sent once from the app; stored in RAM.
 * ===================================================== */
#define ROVER_CFG_INVERT_L     0x01  /* flip left motor direction   */
#define ROVER_CFG_INVERT_R     0x02  /* flip right motor direction  */
#define ROVER_CFG_INVERT_STEER 0x04  /* flip steering left/right    */
#define ROVER_CFG_SWAP_LR      0x08  /* swap left/right outputs     */

typedef struct __attribute__((packed)) {
    uint8_t max_speed;   /* 0..255  straight-line PWM ceiling            */
    uint8_t turn_cap;    /* 0..255  max contribution of the steering term */
    uint8_t accel;       /* PWM units per 20 ms tick, speeding up         */
    uint8_t decel;       /* PWM units per 20 ms tick, slowing / reversing */
    uint8_t expo;        /* 0..100  response-curve strength (0 = linear)  */
    uint8_t flags;       /* ROVER_CFG_*                                    */
} rover_cfg_t;

/* Sensible defaults */
#define ROVER_CFG_DEFAULT_MAX_SPEED  220
#define ROVER_CFG_DEFAULT_TURN_CAP   140
#define ROVER_CFG_DEFAULT_ACCEL       8
#define ROVER_CFG_DEFAULT_DECEL      14
#define ROVER_CFG_DEFAULT_EXPO       35

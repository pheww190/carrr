/*
 * control.c
 * ---------
 * Open-loop differential-drive controller. Pipeline, once per 20 ms tick:
 *
 *   command -> failsafe -> expo curve -> mix (throttle/steer) ->
 *   limits -> invert/swap -> per-wheel slew limiter -> motor output
 *
 * The per-wheel slew limiter is what fixes the "switch direction and it
 * lurches at full power" problem: a wheel's output can only change by
 * `accel` (speeding up) or `decel` (slowing / reversing) units per tick,
 * so a direction change is forced to pass through zero smoothly.
 */
#include "control.h"
#include "motor.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#define TICK_MS        20      /* 50 Hz control loop           */
#define FAILSAFE_MS    400     /* no command for this long -> stop */

/* ---------------- shared state ---------------- */
static rover_cmd_t       s_cmd;
static rover_cfg_t       s_cfg;
static SemaphoreHandle_t s_lock;

static volatile uint32_t s_last_cmd_ms;
static volatile bool     s_link_up;

static int16_t           s_out[2];
static rover_state_t     s_state;

/* ---------------- helpers ---------------- */
static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Response curve: blend linear and cubic so the centre of the stick is
 * gentle and the ends are still full authority.  expo 0..100. */
static int16_t apply_expo(int16_t x, uint8_t expo)
{
    float e = (float)expo / 100.0f;
    float v = (float)x / 100.0f;
    float r = (1.0f - e) * v + e * v * v * v;
    return (int16_t)(r * 100.0f);
}

/* Slew-rate limiter. Grows toward the target at `accel` per tick while it
 * is heading the same way and getting bigger; uses the slower `decel`
 * rate whenever it is shrinking OR crossing through zero to reverse. */
static int16_t slew(int16_t cur, int16_t target, uint8_t accel, uint8_t decel)
{
    int32_t d       = (int32_t)target - (int32_t)cur;
    bool    growing = (target * cur >= 0) && (abs(target) >= abs(cur));
    int32_t rate    = growing ? (int32_t)accel : (int32_t)decel;

    if (d >  rate) d =  rate;
    if (d < -rate) d = -rate;

    return (int16_t)clamp32((int32_t)cur + d, -255, 255);
}

/* ---------------- control task ---------------- */
static void control_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));

        /* snapshot shared state */
        rover_cmd_t cmd;
        rover_cfg_t cfg;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cmd = s_cmd;
        cfg = s_cfg;
        xSemaphoreGive(s_lock);

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* ---------------- failsafe ---------------- */
        bool stale = (uint32_t)(now_ms - s_last_cmd_ms) > FAILSAFE_MS;
        bool fs    = (!s_link_up) || stale || (cmd.flags & ROVER_CMD_FLAG_ESTOP);

        int32_t lt = 0, rt = 0;

        if (!fs) {
            int16_t thr = apply_expo(cmd.throttle, cfg.expo);
            int16_t st  = apply_expo(cmd.steering, cfg.expo);

            if (cfg.flags & ROVER_CFG_INVERT_STEER) st = -st;

            /* mix: throttle +/- steering, steering scaled by turn_cap */
            int32_t base = (int32_t)thr * cfg.max_speed / 100;
            int32_t turn = (int32_t)st  * cfg.turn_cap  / 100;

            if (cmd.flags & ROVER_CMD_FLAG_BRAKE) {
                base = 0;
                turn = 0;
            }

            lt = clamp32(base + turn, -cfg.max_speed, cfg.max_speed);
            rt = clamp32(base - turn, -cfg.max_speed, cfg.max_speed);
        }

        /* ---------------- invert / swap ---------------- */
        if (cfg.flags & ROVER_CFG_INVERT_L) lt = -lt;
        if (cfg.flags & ROVER_CFG_INVERT_R) rt = -rt;
        if (cfg.flags & ROVER_CFG_SWAP_LR)  { int32_t t = lt; lt = rt; rt = t; }

        /* ---------------- slew limit ---------------- */
        s_out[0] = slew(s_out[0], (int16_t)lt, cfg.accel, cfg.decel);
        s_out[1] = slew(s_out[1], (int16_t)rt, cfg.accel, cfg.decel);

        /* ---------------- output ---------------- */
        motor_write(0, s_out[0]);
        motor_write(1, s_out[1]);

        /* ---------------- state ---------------- */
        if (fs) {
            s_state = ROVER_STATE_FAILSAFE;
        } else if ((s_out[0] == 0) && (s_out[1] == 0) &&
                   (lt == 0) && (rt == 0)) {
            s_state = ROVER_STATE_IDLE;
        } else if (((lt == 0) || (rt == 0)) &&
                   ((s_out[0] != 0) || (s_out[1] != 0))) {
            s_state = ROVER_STATE_BRAKING;
        } else {
            s_state = ROVER_STATE_DRIVING;
        }

    }
}

/* ---------------- public API ---------------- */
void control_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cfg.max_speed = ROVER_CFG_DEFAULT_MAX_SPEED;
    s_cfg.turn_cap  = ROVER_CFG_DEFAULT_TURN_CAP;
    s_cfg.accel     = ROVER_CFG_DEFAULT_ACCEL;
    s_cfg.decel     = ROVER_CFG_DEFAULT_DECEL;
    s_cfg.expo      = ROVER_CFG_DEFAULT_EXPO;
    s_cfg.flags     = 0;

    s_out[0] = s_out[1] = 0;
    s_state  = ROVER_STATE_IDLE;
    s_link_up     = false;
    s_last_cmd_ms = (uint32_t)(esp_timer_get_time() / 1000);

    xTaskCreate(control_task, "control", 4096, NULL, 6, NULL);
}

void control_set_cmd(const rover_cmd_t *cmd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cmd = *cmd;
    xSemaphoreGive(s_lock);
    s_last_cmd_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

void control_set_cfg(const rover_cfg_t *cfg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_lock);
}

void control_get_cfg(rover_cfg_t *cfg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *cfg = s_cfg;
    xSemaphoreGive(s_lock);
}

void control_set_link(bool up)
{
    s_link_up = up;
    if (!up) {
        /* drop straight to neutral on disconnect */
        rover_cmd_t z;
        memset(&z, 0, sizeof(z));
        z.flags = ROVER_CMD_FLAG_ESTOP;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cmd = z;
        xSemaphoreGive(s_lock);
    }
}

void control_get_tel(rover_tel_t *tel)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tel->left  = s_out[0];
    tel->right = s_out[1];
    tel->state = (uint8_t)s_state;
    tel->seq   = s_cmd.seq;
    xSemaphoreGive(s_lock);
}

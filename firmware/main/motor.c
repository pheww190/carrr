/*
 * motor.c
 * -------
 * TB6612 / L298N style driver:  EN (PWM) + IN1/IN2 per motor.
 *
 * Pins match the existing wiring. NOTE: GPIO12 (IN3) is a strapping pin;
 * it is fine once booted, but if the board ever fails to boot with the
 * driver connected, move IN3 to GPIO32 and ENB to GPIO33 here.
 */
#include "motor.h"

#include "driver/ledc.h"
#include "driver/gpio.h"

/* ---------------- pins ---------------- */
#define PIN_ENA  25
#define PIN_IN1  26
#define PIN_IN2  27

#define PIN_ENB  14
#define PIN_IN3  12
#define PIN_IN4  13

/* ---------------- LEDC ---------------- */
#define LEDC_MODE    LEDC_LOW_SPEED_MODE
#define LEDC_TIMER   LEDC_TIMER_0
#define LEDC_RES     LEDC_TIMER_8_BIT      /* duty 0..255 */
#define LEDC_FREQ_HZ 20000                 /* above audible range */
#define LEDC_CH_L    LEDC_CHANNEL_0
#define LEDC_CH_R    LEDC_CHANNEL_1

static void set_pwm(ledc_channel_t ch, int duty)
{
    if (duty < 0)   duty = 0;
    if (duty > 255) duty = 255;
    ledc_set_duty(LEDC_MODE, ch, (uint32_t)duty);
    ledc_update_duty(LEDC_MODE, ch);
}

void motor_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .duty_resolution = LEDC_RES,
        .timer_num       = LEDC_TIMER,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_ENA,
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CH_L,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    ch.gpio_num = PIN_ENB;
    ch.channel  = LEDC_CH_R;
    ledc_channel_config(&ch);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_IN1) | (1ULL << PIN_IN2) |
                        (1ULL << PIN_IN3) | (1ULL << PIN_IN4),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    motor_stop();
}

static void drive(ledc_channel_t ch, int pin_fwd, int pin_rev, int16_t out)
{
    if (out >= 0) {
        gpio_set_level(pin_fwd, 0);
        gpio_set_level(pin_rev, 1);
        set_pwm(ch, out);
    } else {
        gpio_set_level(pin_fwd, 1);
        gpio_set_level(pin_rev, 0);
        set_pwm(ch, -out);
    }
}

void motor_write(int idx, int16_t out)
{
    if (idx == 0) {
        drive(LEDC_CH_L, PIN_IN1, PIN_IN2, out);
    } else {
        drive(LEDC_CH_R, PIN_IN3, PIN_IN4, out);
    }
}

void motor_stop(void)
{
    set_pwm(LEDC_CH_L, 0);
    set_pwm(LEDC_CH_R, 0);
}

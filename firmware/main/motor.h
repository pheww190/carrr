/*
 * motor.h
 * -------
 * Low-level motor output: LEDC PWM on the enable pins + plain GPIO on the
 * direction pins. Sign of `out` selects direction, magnitude is the duty.
 */
#pragma once

#include <stdint.h>

/* Configure LEDC + GPIOs and force both motors off. */
void motor_init(void);

/* idx: 0 = left, 1 = right.  out: -255 .. +255 (0 = coast). */
void motor_write(int idx, int16_t out);

/* Cut both channels immediately (used by failsafe / boot). */
void motor_stop(void);

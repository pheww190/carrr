/*
 * control.h
 * ---------
 * The control loop. Runs at a fixed 50 Hz and owns the only path to the
 * motors. The BLE layer pushes commands in; the loop shapes them and
 * writes the outputs.
 */
#pragma once

#include <stdbool.h>
#include "rover_proto.h"

void control_init(void);

/* Called from the BLE write handler with a freshly received frame. */
void control_set_cmd(const rover_cmd_t *cmd);

/* Called from the BLE write handler when new config arrives. */
void control_set_cfg(const rover_cfg_t *cfg);
void control_get_cfg(rover_cfg_t *cfg);

/* Link state, driven by BLE connect/disconnect. */
void control_set_link(bool up);

/* Latest outputs + state, for the telemetry notifier. */
void control_get_tel(rover_tel_t *tel);

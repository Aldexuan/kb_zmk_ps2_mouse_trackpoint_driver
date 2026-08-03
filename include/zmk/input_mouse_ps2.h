/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

int zmk_mouse_ps2_settings_log();
int zmk_mouse_ps2_settings_reset();

int zmk_mouse_ps2_tp_sensitivity_change(int amount);
int zmk_mouse_ps2_tp_neg_inertia_change(int amount);
int zmk_mouse_ps2_tp_value6_upper_plateau_speed_change(int amount);
int zmk_mouse_ps2_tp_pts_threshold_change(int amount);

/*
 * Idle power-saving API
 *
 * These functions are called by the idle-PM module when ZMK activity state
 * transitions between IDLE and ACTIVE. They are safe to call from a work
 * queue but MUST NOT be called from an ISR (they sleep for hundreds of ms
 * while running the TrackPoint Power-On-Reset sequence).
 *
 * They are no-ops (return 0) if CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING
 * is not enabled.
 */
int zmk_mouse_ps2_power_down(void);
int zmk_mouse_ps2_power_up(void);

/*
 * Full device reset: power-cycles the TrackPoint (VCC off → on) and
 * re-runs the complete initialization sequence (POR, device detection,
 * settings re-apply). Equivalent to physically unplugging and re-plugging
 * the keyboard. Use this to recover from any TP anomaly (drift, desync,
 * no response). Runs on a work queue; safe to call from a key behavior.
 */
int zmk_mouse_ps2_reset_device(void);

/*
 * Slow mode control: reduces cursor movement speed by half when enabled.
 * Useful for precise positioning tasks (e.g., pixel-perfect alignment).
 * These functions are safe to call from a key behavior.
 */
int zmk_mouse_ps2_slow_mode_toggle(void);
int zmk_mouse_ps2_slow_mode_set(bool enable);


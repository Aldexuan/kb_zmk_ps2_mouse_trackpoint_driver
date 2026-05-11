/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Glue module: listens to ZMK activity state transitions and drives
 * the PS/2 mouse / TrackPoint driver into / out of its low-power state.
 *
 * The actual heavy lifting (600ms POR, device re-identification, TP
 * setting re-apply) lives in zmk_mouse_ps2_power_up/down().
 * Those calls sleep, so we must never invoke them from the event
 * listener itself (which runs in a restricted context). Instead we
 * schedule work items on the system work queue.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>

#include <zmk/input_mouse_ps2.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static void mouse_ps2_power_down_work_cb(struct k_work *work);
static void mouse_ps2_power_up_work_cb(struct k_work *work);

static K_WORK_DEFINE(mouse_ps2_power_down_work, mouse_ps2_power_down_work_cb);
static K_WORK_DEFINE(mouse_ps2_power_up_work, mouse_ps2_power_up_work_cb);

/* Tracks whether the driver is currently "down" so we don't re-enter
 * the down/up sequence on duplicate ACTIVE->ACTIVE transitions. */
static bool mouse_ps2_is_down = false;

static void mouse_ps2_power_down_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    if (mouse_ps2_is_down) {
        return;
    }
    int err = zmk_mouse_ps2_power_down();
    if (err == 0) {
        mouse_ps2_is_down = true;
    } else {
        LOG_WRN("mouse ps2 power_down failed: %d", err);
    }
}

static void mouse_ps2_power_up_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    if (!mouse_ps2_is_down) {
        return;
    }
    int err = zmk_mouse_ps2_power_up();
    /* Whether or not power_up() succeeded, mark ourselves as "up" so
     * future IDLE->ACTIVE transitions don't short-circuit out. If it
     * failed the user can still hit a key which will re-trigger. */
    mouse_ps2_is_down = false;
    if (err) {
        LOG_WRN("mouse ps2 power_up failed: %d", err);
    }
}

static int mouse_ps2_activity_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        k_work_submit(&mouse_ps2_power_up_work);
        break;
    case ZMK_ACTIVITY_IDLE:
    case ZMK_ACTIVITY_SLEEP:
        /* In SLEEP, ZMK will cut the whole board anyway via the LDO
         * deep sleep path. We still queue power_down so that the
         * driver's internal state (packet buffer, reporting flag)
         * is reset and on resume we go through the full POR cycle. */
        k_work_submit(&mouse_ps2_power_down_work);
        break;
    default:
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(mouse_ps2_idle_pm, mouse_ps2_activity_listener);
ZMK_SUBSCRIPTION(mouse_ps2_idle_pm, zmk_activity_state_changed);

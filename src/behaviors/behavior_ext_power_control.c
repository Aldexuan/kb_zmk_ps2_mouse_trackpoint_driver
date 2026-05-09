/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_ext_power_control

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <dt-bindings/zmk/ext_power.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// External power device reference
static const struct device *ext_power_dev;

// Track if ext_power is currently enabled
static bool ext_power_enabled = true;

// Behavior binding handler for manual control
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (!ext_power_dev) {
        LOG_ERR("Ext power device not found");
        return -ENODEV;
    }

    switch (binding->param1) {
    case EP_ON:
        LOG_INF("Enabling external power (TrackPoint)");
        ext_power_enable(ext_power_dev);
        ext_power_enabled = true;
        break;
    case EP_OFF:
        LOG_INF("Disabling external power (TrackPoint)");
        ext_power_disable(ext_power_dev);
        ext_power_enabled = false;
        break;
    default:
        return -ENOTSUP;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

// Activity state change listener
static int on_activity_state_changed(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (!ext_power_dev) {
        return 0;
    }

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        // Keyboard is active - enable TrackPoint power
        if (!ext_power_enabled) {
            LOG_INF("Keyboard active, enabling TrackPoint power");
            ext_power_enable(ext_power_dev);
            ext_power_enabled = true;
        }
        break;
    case ZMK_ACTIVITY_IDLE:
    case ZMK_ACTIVITY_SLEEP:
        // Keyboard is idle/sleeping - disable TrackPoint power to save battery
        if (ext_power_enabled) {
            LOG_INF("Keyboard idle/sleep, disabling TrackPoint power");
            ext_power_disable(ext_power_dev);
            ext_power_enabled = false;
        }
        break;
    }

    return 0;
}

ZMK_LISTENER(ext_power_ctrl, on_activity_state_changed);
ZMK_SUBSCRIPTION(ext_power_ctrl, zmk_activity_state_changed);

// Initialization Function
static int zmk_behavior_ext_power_control_init(const struct device *dev) {
    // Get the ext_power device
    ext_power_dev = device_get_binding("EXT_POWER");
    if (!ext_power_dev) {
        LOG_WRN("Ext power device not found, auto-control disabled");
        return 0;
    }

    LOG_INF("Ext power control initialized, TrackPoint power management enabled");
    return 0;
}

static const struct behavior_driver_api zmk_behavior_ext_power_control_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, zmk_behavior_ext_power_control_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &zmk_behavior_ext_power_control_driver_api);

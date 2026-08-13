/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_listener_ps2
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/dt-bindings/input/input-event-codes.h>

#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/pointing.h>
#include <zmk/hid.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/event_manager.h>

#define ZMK_MOUSE_HID_NUM_BUTTONS 5

#define ONE_IF_DEV_OK(n)                                                                           \
    COND_CODE_1(DT_NODE_HAS_STATUS(DT_INST_PHANDLE(n, device), okay), (1 +), (0 +))

#define VALID_LISTENER_COUNT (DT_INST_FOREACH_STATUS_OKAY(ONE_IF_DEV_OK) 0)

#if VALID_LISTENER_COUNT > 0

/* ============================================================================
 * ⭐⭐⭐ Global state variables for zero-latency mode control
 * ============================================================================
 * These variables are directly updated by Position Changed events, providing
 * instant state synchronization without any delay.
 * 
 * Architecture:
 * - ps2_scroll_mode_key_pressed: True when scroll mode key is held
 * - ps2_snipe_mode_key_pressed: True when snipe mode key is held
 * - ps2_global_config: Reference to config for event listener access
 * 
 * Thread Safety:
 * - Position Changed events are single-threaded in ZMK event system
 * - No mutex needed, reads/writes are atomic bool operations
 */
static bool ps2_scroll_mode_key_pressed = false;
static bool ps2_snipe_mode_key_pressed = false;
static const struct input_listener_ps2_config *ps2_global_config = NULL;

/* ============================================================================
 * ⭐⭐⭐ Auto-Mouse Layer using Kernel Timer (PMW3610-style)
 * ============================================================================
 * Automatically activates a layer when mouse movement is detected,
 * and deactivates after a timeout period of inactivity.
 * 
 * Implementation uses Zephyr kernel timer for efficient, precise timing.
 * 
 * Configuration (DTS):
 * - automouse-layer: Layer index to activate (-1 = disabled)
 * - automouse-timeout-ms: Timeout before deactivation (default 250ms)
 * - automouse-threshold: Movement threshold for activation (default 5)
 */

#define AUTOMOUSE_LAYER (DT_INST_PROP(0, automouse_layer))
#define AUTOMOUSE_TIMEOUT_MS (DT_INST_PROP(0, automouse_timeout_ms))
#define AUTOMOUSE_THRESHOLD (DT_INST_PROP(0, automouse_threshold))

#if AUTOMOUSE_LAYER >= 0

/* Global state */
static bool ps2_automouse_triggered = false;

/* Timer callback: deactivate layer after timeout */
static void ps2_deactivate_automouse_layer(struct k_timer *timer) {
    /* ⭐ Sticky Key Protection: Check if any keys are currently pressed
     * 
     * Problem: If a key is pressed when the layer deactivates, the key release
     * event will be interpreted on the wrong layer, causing the key to stick.
     * 
     * Solution: Don't deactivate if there was recent input activity.
     * Wait for keys to be released before closing the layer.
     */
    
    // Check if there was any input in the last 100ms
    // (This gives time for key press/release cycles to complete)
    extern int64_t zmk_last_input_activity_time(void);  // ZMK internal function
    int64_t now = k_uptime_get();
    int64_t last_activity = zmk_last_input_activity_time();
    
    if (last_activity > 0 && (now - last_activity) < 100) {
        // Recent input detected, delay deactivation to avoid key sticking
        LOG_DBG("PS/2: Delaying auto-mouse layer deactivation (recent input: %lldms ago)",
                now - last_activity);
        
        // Restart timer for another 100ms
        k_timer_start(&ps2_automouse_timer, K_MSEC(100), K_NO_WAIT);
        return;
    }
    
    // Safe to deactivate now
    ps2_automouse_triggered = false;
    zmk_keymap_layer_deactivate(AUTOMOUSE_LAYER);
    LOG_INF("PS/2: Auto-mouse layer %d deactivated (timeout)", AUTOMOUSE_LAYER);
}

/* Define kernel timer (this creates the timer object automatically) */
K_TIMER_DEFINE(ps2_automouse_timer, ps2_deactivate_automouse_layer, NULL);

/* Activate layer and start/restart timer */
static void ps2_activate_automouse_layer(void) {
    if (!ps2_automouse_triggered) {
        LOG_INF("PS/2: Auto-mouse layer %d activated", AUTOMOUSE_LAYER);
    }
    
    ps2_automouse_triggered = true;
    zmk_keymap_layer_activate(AUTOMOUSE_LAYER);
    
    /* Start/restart timer (automatically cancels previous timer) */
    k_timer_start(&ps2_automouse_timer, K_MSEC(AUTOMOUSE_TIMEOUT_MS), K_NO_WAIT);
}

#endif // AUTOMOUSE_LAYER >= 0

enum input_listener_ps2_xy_data_mode {
    INPUT_LISTENER_XY_DATA_MODE_NONE,
    INPUT_LISTENER_XY_DATA_MODE_REL,
    INPUT_LISTENER_XY_DATA_MODE_ABS,
};

struct input_listener_ps2_xy_data {
    enum input_listener_ps2_xy_data_mode mode;
    int16_t x;
    int16_t y;
};

struct input_listener_ps2_data {
    const struct device *dev;

    union {
        struct {
            struct input_listener_ps2_xy_data data;
            struct input_listener_ps2_xy_data wheel_data;

            uint8_t button_set;
            uint8_t button_clear;
        } mouse;
    };

    int64_t last_scroll_report_time;

    // Scroll accumulator for smooth scrolling with dynamic divisor
    int16_t scroll_residue_x;
    int16_t scroll_residue_y;

    // Watchdog: timestamp of last scroll activity, for residue timeout
    int64_t scroll_last_activity_ms;

    // Slow mode: when enabled, cursor movement speed is halved for precise positioning
    bool slow_mode_enabled;
    
    // Runtime adjustable scroll speed (stored as divisor adjustment)
    // Positive = slower scroll, negative = faster scroll
    // Range: -50 to +50, default: 0
    int16_t scroll_speed_adjustment;
    
    // Track previous layer for detecting scroll layer entry (smooth scrolling)
    int prev_scroll_layer;
    
    // ⭐ Sticky key protection: track last keyboard input time
    int64_t last_keyboard_input_time;
};

struct input_listener_ps2_config {
    bool xy_swap;
    bool x_invert;
    bool y_invert;
    uint16_t scale_multiplier;
    uint16_t scale_divisor;
    
    /* Key-position based mode control (recommended, zero-latency) */
    int scroll_mode_key_position;  // -1 = disabled, >= 0 = key position
    int snipe_mode_key_position;   // -1 = disabled, >= 0 = key position
    
    /* Layer-based mode control (fallback) */
    int scroll_layer;
    int scroll_speed_num;
    int scroll_speed_den;
    int scroll_deadzone;
    int scroll_divisor_slow;
    int scroll_divisor_fast;
    int scroll_input_max;
    bool scroll_dominant_axis;
    
    /* Snipe (slow precision) mode */
    int snipe_layer;
    int snipe_divisor;
};

static char *get_input_code_name(struct input_event *evt) {
    switch (evt->code) {
    case INPUT_REL_X:
        return "INPUT_REL_X";
    case INPUT_REL_Y:
        return "INPUT_REL_Y";
    case INPUT_REL_WHEEL:
        return "INPUT_REL_WHEEL";
    case INPUT_REL_HWHEEL:
        return "INPUT_REL_HWHEEL";
    case INPUT_BTN_0:
        return "INPUT_BTN_0";
    case INPUT_BTN_1:
        return "INPUT_BTN_1";
    case INPUT_BTN_2:
        return "INPUT_BTN_2";
    case INPUT_BTN_3:
        return "INPUT_BTN_3";
    case INPUT_BTN_4:
        return "INPUT_BTN_4";
    default:
        return "UNKNOWN";
    }
}

static void handle_rel_code(struct input_listener_ps2_data *data, struct input_event *evt) {
    switch (evt->code) {
    case INPUT_REL_X:
        data->mouse.data.mode = INPUT_LISTENER_XY_DATA_MODE_REL;
        data->mouse.data.x += evt->value;
        break;
    case INPUT_REL_Y:
        data->mouse.data.mode = INPUT_LISTENER_XY_DATA_MODE_REL;
        data->mouse.data.y += evt->value;
        break;
    case INPUT_REL_WHEEL:
        data->mouse.wheel_data.mode = INPUT_LISTENER_XY_DATA_MODE_REL;
        data->mouse.wheel_data.y += evt->value;
        break;
    case INPUT_REL_HWHEEL:
        data->mouse.wheel_data.mode = INPUT_LISTENER_XY_DATA_MODE_REL;
        data->mouse.wheel_data.x += evt->value;
        break;
    default:
        break;
    }
}

static void handle_abs_code(const struct input_listener_ps2_config *config,
                            struct input_listener_ps2_data *data, struct input_event *evt) {}

static void handle_key_code(const struct input_listener_ps2_config *config,
                            struct input_listener_ps2_data *data, struct input_event *evt) {
    int8_t btn;

    switch (evt->code) {
    case INPUT_BTN_0:
    case INPUT_BTN_1:
    case INPUT_BTN_2:
    case INPUT_BTN_3:
    case INPUT_BTN_4:
        btn = evt->code - INPUT_BTN_0;
        if (evt->value > 0) {
            WRITE_BIT(data->mouse.button_set, btn, 1);
        } else {
            WRITE_BIT(data->mouse.button_clear, btn, 1);
        }
        break;
    default:
        break;
    }
}

static void swap_xy(struct input_event *evt) {
    switch (evt->code) {
    case INPUT_REL_X:
        evt->code = INPUT_REL_Y;
        break;
    case INPUT_REL_Y:
        evt->code = INPUT_REL_X;
        break;
    }
}

static inline bool is_x_data(const struct input_event *evt) {
    return evt->type == INPUT_EV_REL && evt->code == INPUT_REL_X;
}

static inline bool is_y_data(const struct input_event *evt) {
    return evt->type == INPUT_EV_REL && evt->code == INPUT_REL_Y;
}

/* ============================================================================
 * ⭐⭐⭐ Position Changed Event Listener (Zero-Latency Mode Control)
 * ============================================================================
 * This listener directly subscribes to ZMK's position_state_changed events,
 * providing instant state updates when mode control keys are pressed/released.
 * 
 * Priority: This runs BEFORE layer system updates, ensuring zero-latency.
 * 
 * Event Flow:
 * 1. User presses key at position X
 * 2. ZMK fires position_state_changed event
 * 3. This listener updates global state variables (immediate)
 * 4. ZMK layer system processes the key (may have delay)
 * 5. Next mouse input reads global state (already updated!)
 * 
 * Thread Safety: ZMK event system is single-threaded, no race conditions.
 */
static int ps2_mode_key_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;  // Not our event, continue propagation
    }
    
    if (!ps2_global_config) {
        return ZMK_EV_EVENT_BUBBLE;  // Config not initialized yet, skip
    }
    
    /* ⭐ Scroll Mode Key Detection */
    if (ps2_global_config->scroll_mode_key_position >= 0 &&
        ev->position == ps2_global_config->scroll_mode_key_position) {
        
        bool old_state = ps2_scroll_mode_key_pressed;
        ps2_scroll_mode_key_pressed = ev->state;
        
        if (old_state != ev->state) {
            LOG_INF("PS/2: Scroll mode key %s (position %d)", 
                    ev->state ? "PRESSED" : "RELEASED",
                    ev->position);
        }
    }
    
    /* ⭐ Snipe Mode Key Detection */
    if (ps2_global_config->snipe_mode_key_position >= 0 &&
        ev->position == ps2_global_config->snipe_mode_key_position) {
        
        bool old_state = ps2_snipe_mode_key_pressed;
        ps2_snipe_mode_key_pressed = ev->state;
        
        if (old_state != ev->state) {
            LOG_INF("PS/2: Snipe mode key %s (position %d)", 
                    ev->state ? "PRESSED" : "RELEASED",
                    ev->position);
        }
    }
    
    return ZMK_EV_EVENT_BUBBLE;  // Continue event propagation (don't block)
}

/* ⭐⭐⭐ Register listener to ZMK event system */
ZMK_LISTENER(ps2_mode_key_listener, ps2_mode_key_listener);
ZMK_SUBSCRIPTION(ps2_mode_key_listener, zmk_position_state_changed);

static void filter_with_input_config(const struct input_listener_ps2_config *cfg,
                                     struct input_event *evt) {
    if (!evt->dev) {
        return;
    }

    if (cfg->xy_swap) {
        swap_xy(evt);
    }

    if ((cfg->x_invert && is_x_data(evt)) || (cfg->y_invert && is_y_data(evt))) {
        evt->value = -(evt->value);
    }

    evt->value = (int16_t)((evt->value * cfg->scale_multiplier) / cfg->scale_divisor);

    /* ============================================================================
     * ⭐⭐⭐ Snipe Mode: Slow down cursor for precise targeting
     * ============================================================================
     * Priority Logic:
     * 1. If snipe-mode-key-position is configured (>= 0): use global variable (RECOMMENDED)
     * 2. Else: fall back to snipe-layer check (legacy mode)
     * 
     * Applied after scale, before scroll conversion. Scroll mode takes precedence.
     */
    bool in_snipe_mode = false;
    
    // Priority 1: Key-position mode (zero-latency, recommended)
    if (cfg->snipe_mode_key_position >= 0) {
        in_snipe_mode = ps2_snipe_mode_key_pressed;
        LOG_DBG("Snipe mode check: using key-position, active=%d", in_snipe_mode);
    } 
    // Priority 2: Layer mode (legacy, fallback)
    else if (cfg->snipe_layer >= 0) {
        in_snipe_mode = (zmk_keymap_highest_layer_active() == cfg->snipe_layer);
        LOG_DBG("Snipe mode check: using layer %d, active=%d", cfg->snipe_layer, in_snipe_mode);
    }
    
    // Check scroll mode to give it precedence
    bool scroll_takes_precedence = false;
    if (cfg->scroll_mode_key_position >= 0) {
        scroll_takes_precedence = ps2_scroll_mode_key_pressed;
    } else if (cfg->scroll_layer >= 0) {
        scroll_takes_precedence = (zmk_keymap_highest_layer_active() == cfg->scroll_layer);
    }
    
    // Apply snipe divisor (only if not in scroll mode)
    if (in_snipe_mode && !scroll_takes_precedence) {
        int div = cfg->snipe_divisor > 0 ? cfg->snipe_divisor : 2;
        evt->value = (int16_t)(evt->value / div);
        LOG_DBG("Applied snipe divisor: %d, result=%d", div, evt->value);
    }

    /* ============================================================================
     * ⭐⭐⭐ Scroll Mode: Convert mouse movement to scroll wheel
     * ============================================================================
     * Priority Logic:
     * 1. If scroll-mode-key-position is configured (>= 0): use global variable (RECOMMENDED)
     * 2. Else: fall back to scroll-layer check (legacy mode)
     */
    bool in_scroll_mode = false;
    
    // Priority 1: Key-position mode (zero-latency, recommended)
    if (cfg->scroll_mode_key_position >= 0) {
        in_scroll_mode = ps2_scroll_mode_key_pressed;
        LOG_DBG("Scroll mode check: using key-position, active=%d", in_scroll_mode);
    }
    // Priority 2: Layer mode (legacy, fallback)
    else if (cfg->scroll_layer >= 0) {
        in_scroll_mode = (zmk_keymap_highest_layer_active() == cfg->scroll_layer);
        LOG_DBG("Scroll mode check: using layer %d, active=%d", cfg->scroll_layer, in_scroll_mode);
    }
    
    if (in_scroll_mode) {
        int16_t original_val = evt->value;

        /* Convert mouse axis to scroll axis */
        uint16_t scroll_code;
        int8_t dir_mult = 1;
        switch(evt->code) {
            case INPUT_REL_X:
                scroll_code = INPUT_REL_HWHEEL;
                dir_mult = cfg->xy_swap ? -1 : 1;
                break;
            case INPUT_REL_Y:
                scroll_code = INPUT_REL_WHEEL;
                dir_mult = cfg->xy_swap ? 1 : -1;
                break;
            default:
                evt->value = 0;
                return;
        }

        evt->code = scroll_code;

        /* Dominant axis locking: zero out the weaker axis to prevent
         * diagonal scrolling. Uses a 3:2 ratio threshold. */
        if (cfg->scroll_dominant_axis) {
            /* We can't see both axes here (events come one at a time),
             * so dominant axis locking is handled at the report stage.
             * For now, just pass through. The actual locking is done
             * in the sync block below via the accumulator approach. */
        }

        /* Apply direction multiplier but don't scale yet —
         * the accumulator + dynamic divisor handles speed. */
        evt->value = original_val * dir_mult;
    }

}

static void clear_xy_data(struct input_listener_ps2_xy_data *data) {
    data->x = data->y = 0;
    data->mode = INPUT_LISTENER_XY_DATA_MODE_NONE;
}

static void input_handler_ps2(const struct input_listener_ps2_config *config,
                              struct input_listener_ps2_data *data, struct input_event *evt) {
    // First, filter to update the event data as needed.
    filter_with_input_config(config, evt);

    LOG_DBG("Got input_handler_ps2 event: %s with value 0x%x", get_input_code_name(evt),
            evt->value);

    switch (evt->type) {
    case INPUT_EV_REL:
        handle_rel_code(data, evt);
        break;
    case INPUT_EV_ABS:
        handle_abs_code(config, data, evt);
        break;
    case INPUT_EV_KEY:
        handle_key_code(config, data, evt);
        break;
    }

    if (evt->sync) {
        /* ============================================================================
         * ⭐⭐⭐ Auto-Mouse Layer: Trigger on movement (PMW3610-style)
         * ============================================================================
         * Check for mouse movement and activate auto-mouse layer if threshold is met.
         * This happens BEFORE scroll mode processing.
         */
#if AUTOMOUSE_LAYER >= 0
        if (data->mouse.data.mode == INPUT_LISTENER_XY_DATA_MODE_REL) {
            int movement = abs(data->mouse.data.x) + abs(data->mouse.data.y);
            if (movement > AUTOMOUSE_THRESHOLD) {
                ps2_activate_automouse_layer();
            }
        }
#endif

        /* ⭐⭐⭐ Check scroll mode with priority logic */
        bool in_scroll_mode_sync = false;
        if (config->scroll_mode_key_position >= 0) {
            in_scroll_mode_sync = ps2_scroll_mode_key_pressed;
        } else if (config->scroll_layer >= 0) {
            in_scroll_mode_sync = (zmk_keymap_highest_layer_active() == config->scroll_layer);
        }
        
        if (in_scroll_mode_sync) {
            int16_t sx = data->mouse.wheel_data.x;
            int16_t sy = data->mouse.wheel_data.y;

            /* --- Clear residues when entering scroll mode for smooth start --- */
            // For key-position mode: detect state change via global variable
            // For layer mode: detect via prev_scroll_layer tracking
            bool just_entered_scroll_mode = false;
            
            if (config->scroll_mode_key_position >= 0) {
                // Key-position mode: simple edge detection
                // (Note: we could add a static bool to track previous state, but
                // residue watchdog already handles most cases. Keep it simple.)
                static bool prev_scroll_key_state = false;
                if (ps2_scroll_mode_key_pressed && !prev_scroll_key_state) {
                    just_entered_scroll_mode = true;
                    LOG_DBG("Entering scroll mode (key-position), clearing residues");
                }
                prev_scroll_key_state = ps2_scroll_mode_key_pressed;
            } else if (config->scroll_layer >= 0) {
                // Layer mode: use existing prev_scroll_layer tracking
                int current_layer = zmk_keymap_highest_layer_active();
                if (data->prev_scroll_layer != current_layer) {
                    if (current_layer == config->scroll_layer) {
                        just_entered_scroll_mode = true;
                        LOG_DBG("Entering scroll layer, clearing residues");
                    }
                    data->prev_scroll_layer = current_layer;
                }
            }
            
            if (just_entered_scroll_mode) {
                data->scroll_residue_x = 0;
                data->scroll_residue_y = 0;
            }

            /* --- Watchdog: clear residues if idle too long --- */
            int64_t now_ms = k_uptime_get();
            if (data->scroll_last_activity_ms > 0 &&
                (now_ms - data->scroll_last_activity_ms) > CONFIG_ZMK_INPUT_MOUSE_PS2_SCROLL_WDT_MS) {
                LOG_DBG("Scroll watchdog: clearing residues after %lldms idle",
                        now_ms - data->scroll_last_activity_ms);
                data->scroll_residue_x = 0;
                data->scroll_residue_y = 0;
            }
            data->scroll_last_activity_ms = now_ms;

            /* Dominant axis locking */
            if (config->scroll_dominant_axis) {
                int abs_sx = abs(sx);
                int abs_sy = abs(sy);
                /* 3:2 ratio: one axis must be 1.5x the other to "win" */
                if (abs_sy * 2 > abs_sx * 3) {
                    sx = 0; /* pure vertical */
                } else if (abs_sx * 2 > abs_sy * 3) {
                    sy = 0; /* pure horizontal */
                } else {
                    sx = 0; sy = 0; /* diagonal deadzone */
                }
            }

            /* Process each scroll axis with:
             *   deadzone → watchdog/damping → hybrid divisor → accumulator */
            int16_t scroll_out_x = 0;
            int16_t scroll_out_y = 0;
            
            /* Save actual divisor for accurate unit conversion in clamping */
            int actual_divisor_x = 1;
            int actual_divisor_y = 1;
            
            /* Calculate base divisor boundaries */
            int base_divisor_slow = config->scroll_divisor_slow + data->scroll_speed_adjustment;
            int base_divisor_fast = config->scroll_divisor_fast + data->scroll_speed_adjustment;
            
            /* Clamp divisor values to safe ranges */
            if (base_divisor_slow < 10) base_divisor_slow = 10;
            if (base_divisor_fast < 5) base_divisor_fast = 5;
            if (base_divisor_slow > 200) base_divisor_slow = 200;
            if (base_divisor_fast > 100) base_divisor_fast = 100;

            /* --- X axis (horizontal scroll) --- */
            {
                int abs_val = abs(sx);
                if (abs_val <= config->scroll_deadzone) {
                    /* Below deadzone: apply damping (3/4 decay) only when no input.
                     * This prevents residual value from triggering on noise. */
                    data->scroll_residue_x = (data->scroll_residue_x * 3) / 4;
                } else {
                    if (abs_val > config->scroll_input_max) {
                        abs_val = config->scroll_input_max;
                    }
                    
                    /* Hybrid divisor curve: 50% Linear + 50% Quadratic
                     * Reduces high-speed explosion while maintaining responsiveness */
                    int32_t max_val = config->scroll_input_max;
                    int32_t t_linear = abs_val;
                    int32_t t_quad = ((int32_t)abs_val * abs_val) / max_val;
                    int32_t t_hybrid = (t_linear + t_quad) / 2;
                    
                    actual_divisor_x = base_divisor_slow -
                        (int)(((int32_t)(base_divisor_slow - base_divisor_fast) * t_hybrid) / max_val);
                    if (actual_divisor_x < 1) actual_divisor_x = 1;

                    int16_t old_residue_x = data->scroll_residue_x;
                    data->scroll_residue_x += sx;
                    
                    /* Progressive zero-crossing protection:
                     * Large residue (>15): clear immediately for fast direction change
                     * Small residue (≤15): keep value to avoid noise-induced clearing */
                    if ((old_residue_x > 0 && data->scroll_residue_x < 0) ||
                        (old_residue_x < 0 && data->scroll_residue_x > 0)) {
                        if (abs(old_residue_x) > 15) {
                            LOG_DBG("X residue crossed zero (was %d, input %d), clearing",
                                    old_residue_x, sx);
                            data->scroll_residue_x = 0;
                        }
                        /* Small residue: keep value, let it evolve naturally */
                    }
                    
                    scroll_out_x = data->scroll_residue_x / actual_divisor_x;
                    if (scroll_out_x != 0) {
                        data->scroll_residue_x %= actual_divisor_x;
                    }
                    /* NOTE: No damping when there's input (KEY FIX for light-push responsiveness) */
                }
            }

            /* --- Y axis (vertical scroll) --- */
            {
                int abs_val = abs(sy);
                if (abs_val <= config->scroll_deadzone) {
                    /* Below deadzone: apply damping (3/4 decay) only when no input */
                    data->scroll_residue_y = (data->scroll_residue_y * 3) / 4;
                } else {
                    if (abs_val > config->scroll_input_max) {
                        abs_val = config->scroll_input_max;
                    }
                    
                    /* Hybrid divisor curve: 50% Linear + 50% Quadratic
                     * Reduces high-speed explosion while maintaining responsiveness */
                    int32_t max_val = config->scroll_input_max;
                    int32_t t_linear = abs_val;
                    int32_t t_quad = ((int32_t)abs_val * abs_val) / max_val;
                    int32_t t_hybrid = (t_linear + t_quad) / 2;
                    
                    actual_divisor_y = base_divisor_slow -
                        (int)(((int32_t)(base_divisor_slow - base_divisor_fast) * t_hybrid) / max_val);
                    if (actual_divisor_y < 1) actual_divisor_y = 1;

                    int16_t old_residue_y = data->scroll_residue_y;
                    data->scroll_residue_y += sy;
                    
                    /* Progressive zero-crossing protection:
                     * Large residue (>15): clear immediately for fast direction change
                     * Small residue (≤15): keep value to avoid noise-induced clearing */
                    if ((old_residue_y > 0 && data->scroll_residue_y < 0) ||
                        (old_residue_y < 0 && data->scroll_residue_y > 0)) {
                        if (abs(old_residue_y) > 15) {
                            LOG_DBG("Y residue crossed zero (was %d, input %d), clearing",
                                    old_residue_y, sy);
                            data->scroll_residue_y = 0;
                        }
                        /* Small residue: keep value, let it evolve naturally */
                    }
                    
                    scroll_out_y = data->scroll_residue_y / actual_divisor_y;
                    if (scroll_out_y != 0) {
                        data->scroll_residue_y %= actual_divisor_y;
                    }
                    /* NOTE: No damping when there's input (KEY FIX for light-push responsiveness) */
                }
            }

            /* --- Clamp output per frame for smooth scrolling with unit-accurate truncation --- */
            const int16_t MAX_SCROLL_PER_FRAME = 2;
            const int16_t MAX_RESIDUE_ALLOWED = 200;  /* Optimized: allows divisor≤50, excess≤4 */
            
            // X axis clamping with truncation (not discard)
            if (scroll_out_x > MAX_SCROLL_PER_FRAME) {
                int16_t excess = scroll_out_x - MAX_SCROLL_PER_FRAME;
                /* KEY FIX: Convert output units back to input units using actual_divisor_x */
                int32_t target_residue = (int32_t)data->scroll_residue_x + (excess * actual_divisor_x);
                
                /* Truncate to upper limit instead of silently discarding */
                if (target_residue > MAX_RESIDUE_ALLOWED) {
                    data->scroll_residue_x = MAX_RESIDUE_ALLOWED;
                    LOG_DBG("X residue truncated to %d (target was %d)", 
                            MAX_RESIDUE_ALLOWED, (int)target_residue);
                } else {
                    data->scroll_residue_x = (int16_t)target_residue;
                }
                scroll_out_x = MAX_SCROLL_PER_FRAME;
            } else if (scroll_out_x < -MAX_SCROLL_PER_FRAME) {
                int16_t excess = scroll_out_x + MAX_SCROLL_PER_FRAME;  /* negative */
                int32_t target_residue = (int32_t)data->scroll_residue_x + (excess * actual_divisor_x);
                
                if (target_residue < -MAX_RESIDUE_ALLOWED) {
                    data->scroll_residue_x = -MAX_RESIDUE_ALLOWED;
                    LOG_DBG("X residue truncated to %d (target was %d)", 
                            -MAX_RESIDUE_ALLOWED, (int)target_residue);
                } else {
                    data->scroll_residue_x = (int16_t)target_residue;
                }
                scroll_out_x = -MAX_SCROLL_PER_FRAME;
            }
            
            // Y axis clamping with truncation (not discard)
            if (scroll_out_y > MAX_SCROLL_PER_FRAME) {
                int16_t excess = scroll_out_y - MAX_SCROLL_PER_FRAME;
                /* KEY FIX: Convert output units back to input units using actual_divisor_y */
                int32_t target_residue = (int32_t)data->scroll_residue_y + (excess * actual_divisor_y);
                
                /* Truncate to upper limit instead of silently discarding */
                if (target_residue > MAX_RESIDUE_ALLOWED) {
                    data->scroll_residue_y = MAX_RESIDUE_ALLOWED;
                    LOG_DBG("Y residue truncated to %d (target was %d)", 
                            MAX_RESIDUE_ALLOWED, (int)target_residue);
                } else {
                    data->scroll_residue_y = (int16_t)target_residue;
                }
                scroll_out_y = MAX_SCROLL_PER_FRAME;
            } else if (scroll_out_y < -MAX_SCROLL_PER_FRAME) {
                int16_t excess = scroll_out_y + MAX_SCROLL_PER_FRAME;  /* negative */
                int32_t target_residue = (int32_t)data->scroll_residue_y + (excess * actual_divisor_y);
                
                if (target_residue < -MAX_RESIDUE_ALLOWED) {
                    data->scroll_residue_y = -MAX_RESIDUE_ALLOWED;
                    LOG_DBG("Y residue truncated to %d (target was %d)", 
                            -MAX_RESIDUE_ALLOWED, (int)target_residue);
                } else {
                    data->scroll_residue_y = (int16_t)target_residue;
                }
                scroll_out_y = -MAX_SCROLL_PER_FRAME;
            }

            if (scroll_out_x != 0 || scroll_out_y != 0) {
                zmk_hid_mouse_scroll_set(scroll_out_x, scroll_out_y);
                zmk_endpoints_send_mouse_report();
                zmk_hid_mouse_scroll_set(0, 0);
            }

            /* Clear all mouse data for this sync cycle */
            zmk_hid_mouse_movement_set(0, 0);
            clear_xy_data(&data->mouse.data);
            clear_xy_data(&data->mouse.wheel_data);
            data->mouse.button_set = data->mouse.button_clear = 0;
            return;
        }

        if (data->mouse.wheel_data.mode == INPUT_LISTENER_XY_DATA_MODE_REL) {
            zmk_hid_mouse_scroll_set(data->mouse.wheel_data.x, data->mouse.wheel_data.y);
        }

        if (data->mouse.data.mode == INPUT_LISTENER_XY_DATA_MODE_REL) {
            int16_t move_x = data->mouse.data.x;
            int16_t move_y = data->mouse.data.y;
            
            // Apply slow mode: halve the cursor speed when enabled
            if (data->slow_mode_enabled) {
                move_x /= 2;
                move_y /= 2;
            }
            
            zmk_hid_mouse_movement_set(move_x, move_y);
        }

        if (data->mouse.button_set != 0) {
            for (int i = 0; i < ZMK_MOUSE_HID_NUM_BUTTONS; i++) {
                if ((data->mouse.button_set & BIT(i)) != 0) {
                    zmk_hid_mouse_button_press(i);
                }
            }
        }

        if (data->mouse.button_clear != 0) {
            for (int i = 0; i < ZMK_MOUSE_HID_NUM_BUTTONS; i++) {
                if ((data->mouse.button_clear & BIT(i)) != 0) {
                    zmk_hid_mouse_button_release(i);
                }
            }
        }

        zmk_endpoints_send_mouse_report();
        zmk_hid_mouse_scroll_set(0, 0);
        zmk_hid_mouse_movement_set(0, 0);

        clear_xy_data(&data->mouse.data);
        clear_xy_data(&data->mouse.wheel_data);

        data->mouse.button_set = data->mouse.button_clear = 0;
    }
}

#endif // VALID_LISTENER_COUNT > 0

#define IL_INST(n)                                                                                 \
    COND_CODE_1(DT_NODE_HAS_STATUS(DT_INST_PHANDLE(n, device), okay),                              \
                (                                                                                  \
                    static const struct input_listener_ps2_config config_##n =                     \
                        {                                                                          \
                            .xy_swap = DT_INST_PROP(n, xy_swap),                                   \
                            .x_invert = DT_INST_PROP(n, x_invert),                                 \
                            .y_invert = DT_INST_PROP(n, y_invert),                                 \
                            .scale_multiplier = DT_INST_PROP(n, scale_multiplier),                 \
                            .scale_divisor = DT_INST_PROP(n, scale_divisor),                       \
                            /* Key-position based mode control */                                  \
                            .scroll_mode_key_position = DT_INST_PROP(n, scroll_mode_key_position), \
                            .snipe_mode_key_position = DT_INST_PROP(n, snipe_mode_key_position),   \
                            /* Layer-based mode control */                                         \
                            .scroll_layer = DT_INST_PROP(n, scroll_layer),                         \
                            .scroll_speed_num = DT_INST_PROP(n, scroll_speed_num),                 \
                            .scroll_speed_den = DT_INST_PROP(n, scroll_speed_den),                 \
                            .scroll_deadzone = DT_INST_PROP(n, scroll_deadzone),                   \
                            .scroll_divisor_slow = DT_INST_PROP(n, scroll_divisor_slow),           \
                            .scroll_divisor_fast = DT_INST_PROP(n, scroll_divisor_fast),           \
                            .scroll_input_max = DT_INST_PROP(n, scroll_input_max),                 \
                            .scroll_dominant_axis = DT_INST_PROP(n, scroll_dominant_axis),          \
                            .snipe_layer = DT_INST_PROP(n, snipe_layer),                           \
                            .snipe_divisor = DT_INST_PROP(n, snipe_divisor),                       \
                        };                                                                         \
                    static struct input_listener_ps2_data data_##n =                               \
                        {                                                                          \
                            .dev = DEVICE_DT_INST_GET(n),                                          \
                            .scroll_residue_x = 0,                                                 \
                            .scroll_residue_y = 0,                                                 \
                            .scroll_last_activity_ms = 0,                                          \
                            .slow_mode_enabled = false,                                            \
                            .scroll_speed_adjustment = 0,                                          \
                            .prev_scroll_layer = -1,                                               \
                        };                                                                         \
                    void input_handler_ps2_##n(struct input_event *evt) {                          \
                        input_handler_ps2(&config_##n, &data_##n, evt);                            \
                    } INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_INST_PHANDLE(n, device)),             \
                                            input_handler_ps2_##n);                                \
                                                                                                   \
                    static int zmk_input_listener_ps2_init_##n(const struct device *dev) {         \
                        struct input_listener_ps2_data *data = dev->data;                          \
                        const struct input_listener_ps2_config *config = dev->config;              \
                                                                                                   \
                        /* Save config to global variable for event listener access */             \
                        ps2_global_config = config;                                                \
                                                                                                   \
                        /* Log configuration mode */                                               \
                        if (config->scroll_mode_key_position >= 0) {                               \
                            LOG_INF("PS/2: Using scroll-mode-key-position = %d (zero-latency)",    \
                                    config->scroll_mode_key_position);                             \
                        } else if (config->scroll_layer >= 0) {                                    \
                            LOG_INF("PS/2: Using scroll-layer = %d (legacy mode)",                 \
                                    config->scroll_layer);                                         \
                        }                                                                          \
                        if (config->snipe_mode_key_position >= 0) {                                \
                            LOG_INF("PS/2: Using snipe-mode-key-position = %d (zero-latency)",     \
                                    config->snipe_mode_key_position);                              \
                        } else if (config->snipe_layer >= 0) {                                     \
                            LOG_INF("PS/2: Using snipe-layer = %d (legacy mode)",                  \
                                    config->snipe_layer);                                          \
                        }                                                                          \
                                                                                                   \
                        LOG_INF("PS/2: Auto-mouse layer configuration:");                          \
                        LOG_INF("  - Layer: %d (-1 = disabled)", AUTOMOUSE_LAYER);                 \
                        LOG_INF("  - Timeout: %dms", AUTOMOUSE_TIMEOUT_MS);                        \
                        LOG_INF("  - Threshold: %d", AUTOMOUSE_THRESHOLD);                         \
                                                                                                   \
                        return 0;                                                                  \
                    }                                                                              \
                                                                                                   \
                    DEVICE_DT_INST_DEFINE(n, &zmk_input_listener_ps2_init_##n, NULL, &data_##n,    \
                                          &config_##n, POST_KERNEL,                                \
                                          CONFIG_APPLICATION_INIT_PRIORITY, NULL);),               \
                ())

DT_INST_FOREACH_STATUS_OKAY(IL_INST)


/*
 * Slow mode control API
 */

#if VALID_LISTENER_COUNT > 0

// Get the first valid listener data (we only support one PS/2 mouse device)
static struct input_listener_ps2_data *get_listener_data(void) {
#define GET_DATA(n)                                                                                \
    COND_CODE_1(DT_NODE_HAS_STATUS(DT_INST_PHANDLE(n, device), okay), (return &data_##n;), ())
    DT_INST_FOREACH_STATUS_OKAY(GET_DATA)
#undef GET_DATA
    return NULL;
}

int zmk_mouse_ps2_slow_mode_toggle(void) {
    struct input_listener_ps2_data *data = get_listener_data();
    if (data == NULL) {
        LOG_ERR("No valid PS/2 input listener found");
        return -ENODEV;
    }
    
    data->slow_mode_enabled = !data->slow_mode_enabled;
    LOG_INF("Slow mode %s", data->slow_mode_enabled ? "enabled" : "disabled");
    return 0;
}

int zmk_mouse_ps2_slow_mode_set(bool enable) {
    struct input_listener_ps2_data *data = get_listener_data();
    if (data == NULL) {
        LOG_ERR("No valid PS/2 input listener found");
        return -ENODEV;
    }
    
    data->slow_mode_enabled = enable;
    LOG_INF("Slow mode %s", enable ? "enabled" : "disabled");
    return 0;
}

int zmk_mouse_ps2_scroll_speed_adjust(int amount) {
    struct input_listener_ps2_data *data = get_listener_data();
    if (data == NULL) {
        LOG_ERR("No valid PS/2 input listener found");
        return -ENODEV;
    }
    
    int16_t new_adjustment = data->scroll_speed_adjustment + amount;
    
    // Limit range to -50 to +50
    if (new_adjustment < -50) {
        new_adjustment = -50;
        LOG_WRN("Scroll speed adjustment reached minimum (-50)");
    } else if (new_adjustment > 50) {
        new_adjustment = 50;
        LOG_WRN("Scroll speed adjustment reached maximum (+50)");
    }
    
    data->scroll_speed_adjustment = new_adjustment;
    
    LOG_INF("Scroll speed adjustment: %+d (positive = slower, negative = faster)", 
            new_adjustment);
    
    return 0;
}

#else

// Stub implementations when no PS/2 listener is configured
int zmk_mouse_ps2_slow_mode_toggle(void) {
    return -ENOTSUP;
}

int zmk_mouse_ps2_slow_mode_set(bool enable) {
    return -ENOTSUP;
}

int zmk_mouse_ps2_scroll_speed_adjust(int amount) {
    return -ENOTSUP;
}

#endif // VALID_LISTENER_COUNT > 0

/*
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zmk_input_mouse_ps2

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ps2.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Settings
 */

// Delay mouse init to give the mouse time to send the init sequence.
#define ZMK_MOUSE_PS2_INIT_THREAD_DELAY_MS 1000

// How often the driver try to initialize a mouse before we give up.
#define MOUSE_PS2_INIT_ATTEMPTS 10

// Retry budget for the user-triggered reset (MS_TP_RESET). Much smaller than the
// boot budget: the reset runs on the PS/2 maintenance queue in response to a key
// press, so it has to finish in a second or two rather than sleep for ~25s while
// further presses coalesce into the already-queued work item.
#define MOUSE_PS2_RESET_INIT_ATTEMPTS 3

// Mouse activity packets are at least three bytes.
// This defines how much time between bytes can pass before
// we give up on the packet and start fresh.
#define MOUSE_PS2_TIMEOUT_ACTIVITY_PACKET K_MSEC(500)

// A single packet can encode a movement of up to ±255 counts, but a genuine
// TrackPoint report never gets anywhere near that at normal sample rates.
// Anything beyond this is almost certainly a byte-stream misalignment, so we
// drop the packet and resync instead of forwarding a cursor jump to the host.
#define MOUSE_PS2_MOVEMENT_SANITY_LIMIT 60

// Bytes within one PS/2 packet arrive back-to-back (~1ms apart at the PS/2
// clock rate), whereas consecutive packets are separated by the sample
// interval (~10ms at 100Hz). A gap larger than this therefore means the byte
// we just received starts a new packet rather than continuing the current one,
// which is the only reliable way to recover byte-level alignment: the 3-byte
// packet format has no framing, so a stream that is off by one byte stays off
// by one byte until a boundary is identified this way.
#define MOUSE_PS2_INTER_PACKET_GAP_MS CONFIG_ZMK_INPUT_MOUSE_PS2_INTER_PACKET_GAP_MS

/*
 * PS/2 Defines
 */

// According to the `IBM TrackPoint System Version 4.0 Engineering
// Specification`...
// "The POR shall be timed to occur 600 ms ± 20 % from the time power is
//  applied to the TrackPoint controller."
#define MOUSE_PS2_POWER_ON_RESET_TIME K_MSEC(CONFIG_ZMK_INPUT_MOUSE_PS2_POWER_ON_RESET_TIME)

// Common PS/2 Mouse commands
#define MOUSE_PS2_CMD_GET_DEVICE_ID "\xf2"
#define MOUSE_PS2_CMD_GET_DEVICE_ID_RESP_LEN 1

#define MOUSE_PS2_CMD_SET_SAMPLING_RATE "\xf3"
#define MOUSE_PS2_CMD_SET_SAMPLING_RATE_RESP_LEN 0
#define MOUSE_PS2_CMD_SET_SAMPLING_RATE_DEFAULT 100

#define MOUSE_PS2_CMD_ENABLE_REPORTING "\xf4"
#define MOUSE_PS2_CMD_ENABLE_REPORTING_RESP_LEN 0

#define MOUSE_PS2_CMD_DISABLE_REPORTING "\xf5"
#define MOUSE_PS2_CMD_DISABLE_REPORTING_RESP_LEN 0

#define MOUSE_PS2_CMD_RESEND "\xfe"
#define MOUSE_PS2_CMD_RESEND_RESP_LEN 0

#define MOUSE_PS2_CMD_RESET "\xff"
#define MOUSE_PS2_CMD_RESET_RESP_LEN 0

// Trackpoint Commands
// They can be found in the `IBM TrackPoint System Version 4.0 Engineering
// Specification` (YKT3Eext.pdf)...

#define MOUSE_PS2_CMD_TP_GET_SECONDARY_ID "\xe1"
#define MOUSE_PS2_CMD_TP_GET_SECONDARY_ID_RESP_LEN 2

#define MOUSE_PS2_CMD_TP_GET_ROM_ID "\xe2\x46"
#define MOUSE_PS2_CMD_TP_GET_ROM_ID_RESP_LEN 1

#define MOUSE_PS2_CMD_TP_GET_CONFIG_BYTE "\xe2\x80\x2c"
#define MOUSE_PS2_CMD_TP_GET_CONFIG_BYTE_RESP_LEN 1

#define MOUSE_PS2_CMD_TP_SET_CONFIG_BYTE "\xe2\x81\x2c"
#define MOUSE_PS2_CMD_TP_SET_CONFIG_BYTE_RESP_LEN 0

#define MOUSE_PS2_ST_TP_SENSITIVITY "tp_sensitivity"
#define MOUSE_PS2_CMD_TP_GET_SENSITIVITY "\xe2\x80\x4a"
#define MOUSE_PS2_CMD_TP_GET_SENSITIVITY_RESP_LEN 1

#define MOUSE_PS2_CMD_TP_SET_SENSITIVITY "\xe2\x81\x4a"
#define MOUSE_PS2_CMD_TP_SET_SENSITIVITY_RESP_LEN 0
#define MOUSE_PS2_CMD_TP_SET_SENSITIVITY_MIN 0
#define MOUSE_PS2_CMD_TP_SET_SENSITIVITY_MAX 255
#define MOUSE_PS2_CMD_TP_SET_SENSITIVITY_DEFAULT 128

#define MOUSE_PS2_ST_TP_NEG_INERTIA "tp_neg_inertia"
#define MOUSE_PS2_CMD_TP_GET_NEG_INERTIA "\xe2\x80\x4d"
#define MOUSE_PS2_CMD_TP_GET_NEG_INERTIA_RESP_LEN 1

#define MOUSE_PS2_CMD_TP_SET_NEG_INERTIA "\xe2\x81\x4d"
#define MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_RESP_LEN 0
#define MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_MIN 0
#define MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_MAX 255
#define MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_DEFAULT 0x06

#define MOUSE_PS2_ST_TP_VALUE6 "tp_value6"
#define MOUSE_PS2_CMD_TP_GET_VALUE6_UPPER_PLATEAU_SPEED "\xe2\x80\x60"
#define MOUSE_PS2_CMD_TP_GET_VALUE6_UPPER_PLATEAU_SPEED_RESP_LEN 1

#define MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED "\xe2\x81\x60"
#define MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_RESP_LEN 0
#define MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_MIN 0
#define MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_MAX 255
#define MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_DEFAULT 0x61

#define MOUSE_PS2_ST_TP_PTS_THRESHOLD "tp_pts_threshold"
#define MOUSE_PS2_CMD_TP_GET_PTS_THRESHOLD "\xe2\x80\x5c"
#define MOUSE_PS2_CMD_TP_GET_PTS_THRESHOLD_RESP_LEN 1

#define MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD "\xe2\x81\x5c"
#define MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_RESP_LEN 0
#define MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_MIN 0
#define MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_MAX 255
#define MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_DEFAULT 0x08

// Trackpoint Config Bits
#define MOUSE_PS2_TP_CONFIG_BIT_PRESS_TO_SELECT 0x00
#define MOUSE_PS2_TP_CONFIG_BIT_RESERVED 0x01
#define MOUSE_PS2_TP_CONFIG_BIT_BUTTON2 0x02
#define MOUSE_PS2_TP_CONFIG_BIT_INVERT_X 0x03
#define MOUSE_PS2_TP_CONFIG_BIT_INVERT_Y 0x04
#define MOUSE_PS2_TP_CONFIG_BIT_INVERT_Z 0x05
#define MOUSE_PS2_TP_CONFIG_BIT_SWAP_XY 0x06
#define MOUSE_PS2_TP_CONFIG_BIT_FORCE_TRANSPARENT 0x07

// Responses
#define MOUSE_PS2_RESP_SELF_TEST_PASS 0xaa
#define MOUSE_PS2_RESP_SELF_TEST_FAIL 0xfc

/*
 * ZMK Defines
 */

#define MOUSE_PS2_BUTTON_L_IDX 0
#define MOUSE_PS2_BUTTON_R_IDX 1
#define MOUSE_PS2_BUTTON_M_IDX 3

#define MOUSE_PS2_THREAD_STACK_SIZE 2048
#define MOUSE_PS2_THREAD_PRIORITY 10

/*
 * Global Variables
 */

#define MOUSE_PS2_SETTINGS_SUBTREE "mouse_ps2"

/* ============================================================================
 * ⭐⭐⭐ Async Adjustment Execution (Fix Sticky Key Issue)
 * ============================================================================
 * To prevent blocking the ZMK event system (10-20ms), sensitivity adjustments
 * are executed asynchronously in a system work queue.
 * 
 * Problem: PS/2 commands block for 10-20ms, causing:
 *   1. Auto-mouse layer timer to expire prematurely
 *   2. Key release events to be misinterpreted on wrong layer
 *   3. TrackPoint reporting pause to starve the auto-mouse timer
 * 
 * Solution: Submit adjustment requests to work queue, return immediately (<1ms).
 */

static int pending_sensitivity_change = 0;
static K_MUTEX_DEFINE(sensitivity_adjustment_mutex);

static void zmk_mouse_ps2_sensitivity_adjustment_work_handler(struct k_work *work);
static K_WORK_DEFINE(sensitivity_adjustment_work, zmk_mouse_ps2_sensitivity_adjustment_work_handler);

/* ============================================================================
 * ⭐⭐⭐ Dedicated work queue for blocking PS/2 maintenance work
 * ============================================================================
 * Every "heavy" PS/2 operation — idle power-up/down, full device reset and
 * runtime sensitivity/scroll adjustments — performs blocking k_sleep() and
 * blocking PS/2 read/write (each byte can take tens to hundreds of ms, and
 * the wake POR sequence sleeps 600ms+). These MUST NOT run on the Zephyr
 * system work queue, because ZMK's keyboard input pipeline (kscan -> position
 * event -> keymap -> HID) also runs there. If the system work queue is blocked
 * for ~1s during an idle->active wake, the *release* of the very key that woke
 * the board gets stuck behind it, the host never sees the key-up in time and
 * fires typematic auto-repeat (the "first key repeats/stuck on idle wake" bug).
 *
 * A single dedicated, single-threaded queue is used so all PS/2 maintenance
 * work is serialized (they all talk to the same device and must not overlap),
 * while leaving the system work queue free to service keyboard input.
 *
 * Priority is intentionally *below* the system work queue (which defaults to a
 * cooperative priority): these operations spend most of their time sleeping /
 * waiting for the device anyway, so a low preemptible priority guarantees that
 * key scanning always wins the CPU.
 */
#define MOUSE_PS2_WORK_QUEUE_STACK_SIZE 2048
#define MOUSE_PS2_WORK_QUEUE_PRIORITY 10

K_THREAD_STACK_DEFINE(zmk_mouse_ps2_work_queue_stack, MOUSE_PS2_WORK_QUEUE_STACK_SIZE);
static struct k_work_q zmk_mouse_ps2_work_queue;
static bool zmk_mouse_ps2_work_queue_ready = false;

/*
 * Submit a work item to the dedicated PS/2 maintenance queue.
 *
 * Falls back to the system work queue only if the dedicated queue has not been
 * started yet (should never happen in practice: the queue is started during
 * device init at POST_KERNEL, long before any activity-state change or user
 * reset can occur). Exposed via input_mouse_ps2.h so the idle-PM glue module
 * can share the same queue.
 */
int zmk_mouse_ps2_submit_work(struct k_work *work) {
    if (zmk_mouse_ps2_work_queue_ready) {
        return k_work_submit_to_queue(&zmk_mouse_ps2_work_queue, work);
    }

    LOG_WRN("PS/2 work queue not ready yet; falling back to system work queue");
    return k_work_submit(work);
}

typedef enum {
    MOUSE_PS2_PACKET_MODE_PS2_DEFAULT,
    MOUSE_PS2_PACKET_MODE_SCROLL,
} zmk_mouse_ps2_packet_mode;

struct zmk_mouse_ps2_config {
    const struct device *ps2_device;
    struct gpio_dt_spec rst_gpio;
    int rst_gpio_port_num;

    struct gpio_dt_spec vcc_gpio;
    int vcc_gpio_port_num;

    bool scroll_mode;
    bool disable_clicking;
    int sampling_rate;

    bool tp_press_to_select;
    int tp_press_to_select_threshold;
    int tp_sensitivity;
    int tp_neg_inertia;
    int tp_val6_upper_speed;
    bool tp_x_invert;
    bool tp_y_invert;
    bool tp_xy_swap;
};

struct zmk_mouse_ps2_packet {
    int16_t mov_x;
    int16_t mov_y;
    int8_t scroll;
    bool overflow_x;
    bool overflow_y;
    bool button_l;
    bool button_m;
    bool button_r;
};

struct zmk_mouse_ps2_data {
    const struct device *dev;
    struct gpio_dt_spec rst_gpio; /* GPIO used for Power-On-Reset line */

    K_THREAD_STACK_MEMBER(thread_stack, MOUSE_PS2_THREAD_STACK_SIZE);
    struct k_thread thread;

    zmk_mouse_ps2_packet_mode packet_mode;
    uint8_t packet_buffer[4];
    int packet_idx;
    struct zmk_mouse_ps2_packet prev_packet;
    struct k_work_delayable packet_buffer_timeout;

    bool button_l_is_held;
    bool button_m_is_held;
    bool button_r_is_held;

    bool activity_reporting_on;
    bool is_trackpoint;
    uint8_t manufacturer_id;
    uint8_t secondary_id;
    uint8_t rom_id;

    uint8_t sampling_rate;
    uint8_t tp_sensitivity;
    uint8_t tp_neg_inertia;
    uint8_t tp_value6;
    uint8_t tp_pts_threshold;

    // Power saving: Track wake-up packets to discard initial drift
    uint8_t wake_up_packets_to_discard;

    // Uptime (ms) at which the previous activity byte was handled. Used to
    // detect the inter-packet gap and recover byte-level alignment.
    uint32_t last_byte_time;

    // Post-wake drift suppression window. While uptime is before this, small
    // movements are treated as TrackPoint recalibration drift and dropped.
    // Zero means "not in a wake window".
    uint32_t wake_deadzone_until;
    uint32_t wake_deadzone_start;
};

static const struct zmk_mouse_ps2_config zmk_mouse_ps2_config = {
    .ps2_device = DEVICE_DT_GET(DT_INST_PHANDLE(0, ps2_device)),

#if DT_INST_NODE_HAS_PROP(0, rst_gpios)
    .rst_gpio = GPIO_DT_SPEC_INST_GET(0, rst_gpios),
    .rst_gpio_port_num = DT_PROP(DT_INST_PHANDLE(0, rst_gpios), port),
#else
    .rst_gpio =
        {
            .port = NULL,
            .pin = 0,
            .dt_flags = 0,
        },
    .rst_gpio_port_num = 0,
#endif

#if DT_INST_NODE_HAS_PROP(0, vcc_gpios)
    .vcc_gpio = GPIO_DT_SPEC_INST_GET(0, vcc_gpios),
    .vcc_gpio_port_num = DT_PROP(DT_INST_PHANDLE(0, vcc_gpios), port),
#else
    .vcc_gpio =
        {
            .port = NULL,
            .pin = 0,
            .dt_flags = 0,
        },
    .vcc_gpio_port_num = 0,
#endif

    .scroll_mode = DT_INST_PROP_OR(0, scroll_mode, false),
    .disable_clicking = DT_INST_PROP_OR(0, disable_clicking, false),
    .sampling_rate = DT_INST_PROP_OR(0, sampling_rate, MOUSE_PS2_CMD_SET_SAMPLING_RATE_DEFAULT),
    .tp_press_to_select = DT_INST_PROP_OR(0, tp_press_to_select, false),
    .tp_press_to_select_threshold = DT_INST_PROP_OR(0, tp_press_to_select_threshold, -1),
    .tp_sensitivity = DT_INST_PROP_OR(0, tp_sensitivity, -1),
    .tp_neg_inertia = DT_INST_PROP_OR(0, tp_neg_inertia, -1),
    .tp_val6_upper_speed = DT_INST_PROP_OR(0, tp_val6_upper_speed, -1),
    .tp_x_invert = DT_INST_PROP_OR(0, tp_x_invert, false),
    .tp_y_invert = DT_INST_PROP_OR(0, tp_y_invert, false),
    .tp_xy_swap = DT_INST_PROP_OR(0, tp_xy_swap, false),
};

static struct zmk_mouse_ps2_data zmk_mouse_ps2_data = {
    .packet_mode = MOUSE_PS2_PACKET_MODE_PS2_DEFAULT,
    .packet_idx = 0,
    .prev_packet =
        {
            .button_l = false,
            .button_r = false,
            .button_m = false,
            .overflow_x = 0,
            .overflow_y = 0,
            .mov_x = 0,
            .mov_y = 0,
            .scroll = 0,
        },

    .button_l_is_held = false,
    .button_m_is_held = false,
    .button_r_is_held = false,

    // Data reporting is disabled on init
    .activity_reporting_on = false,

    // Device Info
    .is_trackpoint = false,
    .manufacturer_id = 0x0,
    .secondary_id = 0x0,
    .rom_id = 0x0,

    // PS2 devices initialize with this rate
    .sampling_rate = MOUSE_PS2_CMD_SET_SAMPLING_RATE_DEFAULT,
    .tp_sensitivity = MOUSE_PS2_CMD_TP_SET_SENSITIVITY_DEFAULT,
    .tp_neg_inertia = MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_DEFAULT,
    .tp_value6 = MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_DEFAULT,
    .tp_pts_threshold = MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_DEFAULT,
};

static int allowed_sampling_rates[] = {
    10, 20, 40, 60, 80, 100, 200,
};

/*
 * Function Definitions
 */

int zmk_mouse_ps2_settings_save();
void zmk_mouse_ps2_apply_tp_settings(void);
int zmk_mouse_ps2_init_power_on_reset(void);
int zmk_mouse_ps2_init_wait_for_mouse(const struct device *dev);

/*
 * Helpers
 */

#define MOUSE_PS2_GET_BIT(data, bit_pos) ((data >> bit_pos) & 0x1)
#define MOUSE_PS2_SET_BIT(data, bit_val, bit_pos) (data |= (bit_val) << bit_pos)

/*
 * Mouse Activity Packet Reading
 */

void zmk_mouse_ps2_activity_process_cmd(zmk_mouse_ps2_packet_mode packet_mode, uint8_t packet_state,
                                        uint8_t packet_x, uint8_t packet_y, uint8_t packet_extra);
void zmk_mouse_ps2_activity_abort_cmd();
void zmk_mouse_ps2_activity_move_mouse(int16_t mov_x, int16_t mov_y);
void zmk_mouse_ps2_activity_scroll(int8_t scroll_y);
void zmk_mouse_ps2_activity_click_buttons(bool button_l, bool button_m, bool button_r);
void zmk_mouse_ps2_activity_reset_packet_buffer();
void zmk_mouse_ps2_request_wake_up_discard(uint8_t packets);
void zmk_mouse_ps2_release_all_buttons(void);
void zmk_mouse_ps2_activity_reset_prev_packet();
void zmk_mouse_ps2_start_wake_deadzone(void);
static void zmk_mouse_ps2_vcc_set(bool powered);
struct zmk_mouse_ps2_packet
zmk_mouse_ps2_activity_parse_packet_buffer(zmk_mouse_ps2_packet_mode packet_mode,
                                           uint8_t packet_state, uint8_t packet_x, uint8_t packet_y,
                                           uint8_t packet_extra);
void zmk_mouse_ps2_activity_toggle_layer();

// Called by the PS/2 driver whenver the mouse sends a byte and
// reporting is enabled through `zmk_mouse_ps2_activity_reporting_enable`.
void zmk_mouse_ps2_activity_callback(const struct device *ps2_device, uint8_t byte) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    k_work_cancel_delayable(&data->packet_buffer_timeout);

    // LOG_DBG("Received mouse movement data: 0x%x", byte);

    /* Byte-level resynchronisation.
     *
     * If we are mid-packet but a long gap has elapsed since the previous byte,
     * the bytes we already buffered belong to an earlier packet that never
     * completed, and this byte is the start of a fresh one. Re-anchoring here
     * is what actually fixes a one-byte misalignment: discarding whole packets
     * cannot, because it removes bytes in groups of three at the wrong offset
     * and preserves the misalignment exactly.
     *
     * We only re-anchor when this byte also passes the bit-3 test below, i.e.
     * when it is plausible as a first byte. Without that guard a merely slow
     * work queue could drop a perfectly good partial packet and create the very
     * misalignment we are trying to remove. The timestamp is taken here rather
     * than in the UART ISR, so the measured gap includes work-queue latency;
     * that is why the threshold is tunable and defaults well above the ~1ms
     * intra-packet spacing.
     */
    uint32_t now = k_uptime_get_32();
    if (data->packet_idx != 0 && (now - data->last_byte_time) >= MOUSE_PS2_INTER_PACKET_GAP_MS &&
        MOUSE_PS2_GET_BIT(byte, 3) == 1) {
        LOG_DBG("Inter-packet gap of %ums at idx=%d; re-anchoring packet start",
                now - data->last_byte_time, data->packet_idx);
        zmk_mouse_ps2_activity_reset_packet_buffer();
    }
    data->last_byte_time = now;

    data->packet_buffer[data->packet_idx] = byte;

    if (data->packet_idx == 0) {

        // Bit 3 of the first command byte should always be 1
        // If it is not, then we are definitely out of alignment.
        // So we ask the device to resend the entire 3-byte command
        // again.
        int alignment_bit = MOUSE_PS2_GET_BIT(byte, 3);
        if (alignment_bit != 1) {

            zmk_mouse_ps2_activity_abort_cmd("Bit 3 of packet is 0 instead of 1");
            return;
        }
    } else if (data->packet_idx == 1) {
        // Do nothing
    } else if ((data->packet_mode == MOUSE_PS2_PACKET_MODE_PS2_DEFAULT && data->packet_idx == 2) ||
               (data->packet_mode == MOUSE_PS2_PACKET_MODE_SCROLL && data->packet_idx == 3)) {

        zmk_mouse_ps2_activity_process_cmd(data->packet_mode, data->packet_buffer[0],
                                           data->packet_buffer[1], data->packet_buffer[2],
                                           data->packet_buffer[3]);
        zmk_mouse_ps2_activity_reset_packet_buffer();
        return;
    }

    data->packet_idx += 1;

    k_work_schedule(&data->packet_buffer_timeout, MOUSE_PS2_TIMEOUT_ACTIVITY_PACKET);
}

void zmk_mouse_ps2_activity_abort_cmd(char *reason) {
    // const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;

    // LOG_ERR("PS/2 Mouse cmd buffer is out of aligment. Requesting resend: %s", reason);
    // ps2_write(ps2_device, MOUSE_PS2_CMD_RESEND[0]);
    LOG_ERR(
        "PS/2 Mouse cmd buffer is out of alignment. igoring..."); // resend somehow make it worse

    zmk_mouse_ps2_activity_reset_packet_buffer();
}

#if IS_ENABLED(CONFIG_ZMK_INPUT_MOUSE_PS2_ENABLE_PS2_RESEND_CALLBACK)

// Called if the PS/2 driver encounters a transmission error and asks the
// device to resend the packet.
// The device will resend all bytes of the packet. So we need to reset our
// buffer.
void zmk_mouse_ps2_activity_resend_callback(const struct device *ps2_device) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    LOG_WRN("Mouse movement cmd had transmission error on idx=%d", data->packet_idx);

    zmk_mouse_ps2_activity_reset_packet_buffer();
}

#endif /* IS_ENABLED(CONFIG_ZMK_INPUT_MOUSE_PS2_ENABLE_PS2_RESEND_CALLBACK) */

// Called if no new byte arrives within
// MOUSE_PS2_TIMEOUT_ACTIVITY_PACKET
void zmk_mouse_ps2_activity_packet_timout(struct k_work *item) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    LOG_DBG("Mouse movement cmd timed out on idx=%d", data->packet_idx);

    // Reset the cmd buffer in case we are out of alignment.
    // This way if the mouse ever gets out of alignment, the user
    // can reset it by just not moving it for a second.
    zmk_mouse_ps2_activity_reset_packet_buffer();

    // The stream has stopped, so the delta baseline is stale. Drop it too, or
    // the first packet after the pause gets measured against an arbitrarily old
    // one and can be discarded as a bogus jump.
    zmk_mouse_ps2_activity_reset_prev_packet();
}

void zmk_mouse_ps2_activity_reset_packet_buffer() {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    data->packet_idx = 0;
    memset(data->packet_buffer, 0x0, sizeof(data->packet_buffer));
}

/*
 * Drop the delta baseline used by the movement-jump heuristic.
 *
 * prev_packet only means anything for packets that are adjacent in time. After
 * an inter-packet timeout the next packet may be minutes newer, so comparing
 * against the stale one is meaningless — and actively harmful, because the
 * x_delta/y_delta check would reject the user's first real movement after a
 * pause as a "malformed packet". Zeroing it makes the first packet after a gap
 * compare against a neutral baseline instead.
 */
void zmk_mouse_ps2_activity_reset_prev_packet() {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    memset(&data->prev_packet, 0x0, sizeof(data->prev_packet));
}

/*
 * Request that the next `packets` activity packets be dropped.
 *
 * This only ever *raises* the counter. Several stages of a single wake
 * sequence each ask for a discard window (POR, power_up, reporting_enable),
 * and they run in an order that is not fully fixed. A plain assignment let a
 * later stage silently shrink an earlier stage's larger request, so the
 * effective window was whatever happened to run last rather than what any
 * caller asked for. Taking the maximum makes the widest request win no matter
 * how the stages are ordered.
 */
void zmk_mouse_ps2_request_wake_up_discard(uint8_t packets) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    if (packets > data->wake_up_packets_to_discard) {
        data->wake_up_packets_to_discard = packets;
    }
}

/*
 * Open the post-wake drift-suppression window.
 *
 * A TrackPoint re-establishes its strain-gauge zero baseline every time it goes
 * through power-on reset. If anything is loading the stick while that happens
 * the baseline latches slightly off, and the device then emits a steady stream
 * of small non-zero deltas until its internal drift correction re-converges —
 * roughly one to two seconds of the cursor sliding on its own.
 *
 * Discarding whole packets cannot fix this well: the drift lasts far longer
 * than a sensible discard window, and a window wide enough to cover it also
 * swallows the first movement the user actually intends. A magnitude deadzone
 * separates the two instead, because drift and intent differ in amplitude
 * (drift is a couple of counts, a deliberate push is much larger).
 *
 * The threshold decays linearly to zero across the window rather than being
 * switched off at the end, so sensitivity is restored gradually instead of
 * changing abruptly under the user's finger.
 */
void zmk_mouse_ps2_start_wake_deadzone(void) {
#if CONFIG_ZMK_INPUT_MOUSE_PS2_WAKE_DEADZONE_MS > 0
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    data->wake_deadzone_start = k_uptime_get_32();
    data->wake_deadzone_until =
        data->wake_deadzone_start + CONFIG_ZMK_INPUT_MOUSE_PS2_WAKE_DEADZONE_MS;

    LOG_DBG("Wake drift deadzone active for %dms (threshold %d)",
            CONFIG_ZMK_INPUT_MOUSE_PS2_WAKE_DEADZONE_MS,
            CONFIG_ZMK_INPUT_MOUSE_PS2_WAKE_DEADZONE_THRESHOLD);
#endif
}

/*
 * Current deadzone threshold, or 0 when the window is closed / disabled.
 * Ramps from the configured value down to 0 over the window.
 */
static int zmk_mouse_ps2_wake_deadzone_threshold(void) {
#if CONFIG_ZMK_INPUT_MOUSE_PS2_WAKE_DEADZONE_MS > 0
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    if (data->wake_deadzone_until == 0) {
        return 0;
    }

    uint32_t now = k_uptime_get_32();

    /* Signed comparison so this still terminates correctly if uptime wraps. */
    if ((int32_t)(now - data->wake_deadzone_until) >= 0) {
        data->wake_deadzone_until = 0;
        LOG_DBG("Wake drift deadzone expired");
        return 0;
    }

    uint32_t elapsed = now - data->wake_deadzone_start;
    uint32_t total = data->wake_deadzone_until - data->wake_deadzone_start;
    int full = CONFIG_ZMK_INPUT_MOUSE_PS2_WAKE_DEADZONE_THRESHOLD;

    /* Linear ramp: full at the start of the window, 0 at its end. */
    return (int)((full * (total - elapsed)) / total);
#else
    return 0;
#endif
}

void zmk_mouse_ps2_activity_process_cmd(zmk_mouse_ps2_packet_mode packet_mode, uint8_t packet_state,
                                        uint8_t packet_x, uint8_t packet_y, uint8_t packet_extra) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    // Discard initial packets after wake-up to avoid drift/desync
    if (data->wake_up_packets_to_discard > 0) {
        data->wake_up_packets_to_discard--;
        LOG_DBG("Discarding wake-up packet (%d remaining)", data->wake_up_packets_to_discard);
        return;
    }

    struct zmk_mouse_ps2_packet packet;
    packet = zmk_mouse_ps2_activity_parse_packet_buffer(packet_mode, packet_state, packet_x,
                                                        packet_y, packet_extra);

    int x_delta = abs(data->prev_packet.mov_x - packet.mov_x);
    int y_delta = abs(data->prev_packet.mov_y - packet.mov_y);

    LOG_DBG("Got mouse activity cmd "
            "(mov_x=%d, mov_y=%d, o_x=%d, o_y=%d, scroll=%d, "
            "b_l=%d, b_m=%d, b_r=%d) and ("
            "x_delta=%d, y_delta=%d)",
            packet.mov_x, packet.mov_y, packet.overflow_x, packet.overflow_y, packet.scroll,
            packet.button_l, packet.button_m, packet.button_r, x_delta, y_delta);

    /* Overflow on both axes at once is not something a TrackPoint reports in
     * normal operation; it is the fingerprint of a protocol byte that leaked
     * into the packet stream and got decoded as a state byte.
     *
     * The concrete case: ps2_uart's write path waits up to 300ms for a command
     * ACK (PS2_UART_TIMEOUT_WRITE_AWAIT_RESPONSE) and then clears
     * write_awaits_resp regardless of whether the ACK arrived. An ACK that
     * shows up after that deadline is no longer recognised as a write response,
     * so it is delivered to us as ordinary data. 0xFA decodes as
     * button_r=1, both sign bits set and both overflow bits set — a stuck right
     * button plus a large negative jump on both axes — and its bit 3 is 1, so
     * the alignment check waves it through. It also shifts every following
     * packet by one byte.
     *
     * This check is deliberately outside ENABLE_ERROR_MITIGATION (which
     * defaults to n): the stray-ACK path above exists regardless of which PS/2
     * transport is in use, so gating the only cheap detector for it behind an
     * off-by-default option leaves the failure completely unguarded. */
    if (packet.overflow_x == 1 && packet.overflow_y == 1) {
        LOG_WRN("Detected overflow in both x and y. "
                "Probably mistransmission. Aborting...");

        /* Clear anything a previous misaligned packet may have latched, for the
         * same reason as in the movement sanity check below. */
        zmk_mouse_ps2_release_all_buttons();

        zmk_mouse_ps2_activity_abort_cmd("Overflow in both x and y");
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_INPUT_MOUSE_PS2_ENABLE_ERROR_MITIGATION)
    // If the mouse exceeds the allowed threshold of movement, it's probably
    // a mistransmission or misalignment.
    // But we only do this check if there was prior movement that wasn't
    // reset in `zmk_mouse_ps2_activity_packet_timout`.
    if ((packet.mov_x != 0 && packet.mov_y != 0) && (x_delta > 150 || y_delta > 150)) {
        LOG_WRN("Detected malformed packet with "
                "(mov_x=%d, mov_y=%d, o_x=%d, o_y=%d, scroll=%d, "
                "b_l=%d, b_m=%d, b_r=%d) and ("
                "x_delta=%d, y_delta=%d)",
                packet.mov_x, packet.mov_y, packet.overflow_x, packet.overflow_y, packet.scroll,
                packet.button_l, packet.button_m, packet.button_r, x_delta, y_delta);
        zmk_mouse_ps2_activity_abort_cmd("Exceeds movement threshold.");
        return;
    }
#endif

    // Safety check: if movement is unreasonably large, it's likely a desync
    // artifact. This must run BEFORE we report anything, otherwise the bogus
    // jump has already reached the host and resyncing afterwards is pointless.
    if (abs(packet.mov_x) > MOUSE_PS2_MOVEMENT_SANITY_LIMIT ||
        abs(packet.mov_y) > MOUSE_PS2_MOVEMENT_SANITY_LIMIT) {
        LOG_WRN("Detected abnormal movement (x=%d, y=%d), forcing resync.", packet.mov_x,
                packet.mov_y);

        // Force reset the packet buffer to regain synchronization
        zmk_mouse_ps2_activity_reset_packet_buffer();

        /* A packet this malformed means the byte stream is misaligned, and a
         * misaligned state byte decodes random bits as button presses. If we
         * simply returned here, a phantom press latched by an earlier packet
         * would never see its release: every packet in the desync burst gets
         * rejected at this very check, so click_buttons() is never reached and
         * the host stays stuck in a drag for the whole burst.
         *
         * Movement from this packet is untrustworthy, but "release" is the
         * safe direction regardless of alignment, so we always let it through.
         * We never latch a *press* from an untrusted packet. */
        zmk_mouse_ps2_release_all_buttons();

        // Ask the device to retransmit the last packet
        const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;
        ps2_write(config->ps2_device, 0xFE); // RESEND command

        // Deliberately do not store this packet as prev_packet: it is garbage,
        // and using it as the delta baseline would make the next legitimate
        // packet look like a huge jump too.
        return;
    }

    /* Post-wake drift suppression. Only movement is gated: buttons stay fully
     * live, because a click is unambiguous intent that must never be swallowed,
     * and drift never produces one.
     *
     * The test requires *both* axes to be drift-sized before either is zeroed,
     * rather than gating each axis on its own. Independent per-axis gating would
     * turn a slow diagonal push of e.g. (2,3) into (0,3) and visibly skew the
     * direction; treating the packet as a unit keeps the vector intact and only
     * drops it when the whole movement is within drift amplitude. */
    int deadzone = zmk_mouse_ps2_wake_deadzone_threshold();
    if (deadzone > 0) {
        if (abs(packet.mov_x) <= deadzone && abs(packet.mov_y) <= deadzone) {
            LOG_DBG("Wake deadzone (%d) suppressed drift (x=%d, y=%d)", deadzone, packet.mov_x,
                    packet.mov_y);
            packet.mov_x = 0;
            packet.mov_y = 0;
        } else {
            /* Real movement means the user has taken over; the baseline
             * question is moot from here on, so close the window early rather
             * than keep attenuating what they are doing. */
            LOG_DBG("Wake deadzone released early by real movement (x=%d, y=%d)", packet.mov_x,
                    packet.mov_y);
            data->wake_deadzone_until = 0;
        }
    }

    zmk_mouse_ps2_activity_move_mouse(packet.mov_x, packet.mov_y);
    zmk_mouse_ps2_activity_click_buttons(packet.button_l, packet.button_m, packet.button_r);

    data->prev_packet = packet;
}

struct zmk_mouse_ps2_packet
zmk_mouse_ps2_activity_parse_packet_buffer(zmk_mouse_ps2_packet_mode packet_mode,
                                           uint8_t packet_state, uint8_t packet_x, uint8_t packet_y,
                                           uint8_t packet_extra) {
    struct zmk_mouse_ps2_packet packet;

    packet.button_l = MOUSE_PS2_GET_BIT(packet_state, 0);
    packet.button_r = MOUSE_PS2_GET_BIT(packet_state, 1);
    packet.button_m = MOUSE_PS2_GET_BIT(packet_state, 2);
    packet.overflow_x = MOUSE_PS2_GET_BIT(packet_state, 6);
    packet.overflow_y = MOUSE_PS2_GET_BIT(packet_state, 7);
    packet.scroll = 0;

    // The coordinates are delivered as a signed 9bit integers.
    // But a PS/2 packet is only 8 bits, so the most significant
    // bit with the sign is stored inside the state packet.
    //
    // Since we are converting the uint8_t into a int16_t
    // we must pad the unused most significant bits with
    // the sign bit.
    //
    // Example:
    //                              ↓ x sign bit
    //  - State: 0x18 (          0001 1000)
    //                             ↑ y sign bit
    //  - X:     0xfd (          1111 1101) / decimal 253
    //  - New X:      (1111 1111 1111 1101) / decimal -3
    //
    //  - Y:     0x02 (          0000 0010) / decimal 2
    //  - New Y:      (0000 0000 0000 0010) / decimal 2
    //
    // The code below creates a signed int and is from...
    // https://wiki.osdev.org/PS/2_Mouse
    packet.mov_x = packet_x - ((packet_state << 4) & 0x100);
    packet.mov_y = packet_y - ((packet_state << 3) & 0x100);

    // If packet mode scroll or scroll+5 buttons is used,
    // then the first 4 bit of the extra byte are used for the
    // scroll wheel. It is a signed number with the rango of
    // -8 to +7.
    if (packet_mode == MOUSE_PS2_PACKET_MODE_SCROLL) {
        MOUSE_PS2_SET_BIT(packet.scroll, MOUSE_PS2_GET_BIT(packet_extra, 0), 0);
        MOUSE_PS2_SET_BIT(packet.scroll, MOUSE_PS2_GET_BIT(packet_extra, 1), 1);
        MOUSE_PS2_SET_BIT(packet.scroll, MOUSE_PS2_GET_BIT(packet_extra, 2), 2);
        packet.scroll = packet_extra - ((packet.scroll << 3) & 0x100);
    }

    return packet;
}

/*
 * Mouse Moving and Clicking
 */

static bool zmk_mouse_ps2_is_non_zero_1d_movement(int16_t speed) { return speed != 0; }

void zmk_mouse_ps2_activity_move_mouse(int16_t mov_x, int16_t mov_y) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    int ret = 0;

    /* Deadzone filter: TrackPoints produce occasional 1-unit noise packets
     * even when completely untouched. If we report these to ZMK, the
     * activity timer resets and the keyboard never enters idle/sleep.
     * Drop packets where BOTH axes are within the deadzone. */
    const int deadzone = 1;
    if (abs(mov_x) <= deadzone && abs(mov_y) <= deadzone) {
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL)
    /* MCU-side non-linear acceleration.
     *
     * Inspired by the exponential curve in trackpoint_0x15.c (I2C driver).
     * That driver uses expf(speed * k); we replicate the effect using pure
     * integer arithmetic since nRF52 has no hardware FPU.
     *
     * Formula: mult_pct = 100 + dist * FACTOR
     *   dist     = |dx| + |dy|  (Manhattan length of the packet vector)
     *   FACTOR   = CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL_FACTOR   (default 8)
     *   cap      = CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL_MAX_MULT_PCT (default 250)
     *
     * Example (FACTOR=8, cap=250):
     *   dist= 2 (very light) → 1.16x
     *   dist= 5 (light)      → 1.40x
     *   dist=10 (medium)     → 1.80x
     *   dist=19 (strong)     → 2.52x → capped at 2.50x
     *
     * Scaling is applied to the integer values directly; the >>7 shift
     * (divide-by-128) keeps the multiply-accumulate in 32-bit range. */
    {
        int dist = abs((int)mov_x) + abs((int)mov_y);
        int mult_pct = 100 + dist * CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL_FACTOR;
        if (mult_pct > CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL_MAX_MULT_PCT) {
            mult_pct = CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL_MAX_MULT_PCT;
        }
        /* Apply multiplier: val * mult_pct / 100 using 32-bit intermediates */
        mov_x = (int16_t)(((int32_t)mov_x * mult_pct) / 100);
        mov_y = (int16_t)(((int32_t)mov_y * mult_pct) / 100);
    }
#endif /* CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL */

    bool have_x = zmk_mouse_ps2_is_non_zero_1d_movement(mov_x);
    bool have_y = zmk_mouse_ps2_is_non_zero_1d_movement(mov_y);

    if (have_x) {
        ret = input_report_rel(data->dev, INPUT_REL_X, mov_x, !have_y, K_NO_WAIT);
    }
    if (have_y) {
        ret = input_report_rel(data->dev, INPUT_REL_Y, mov_y, true, K_NO_WAIT);
    }
}

/*
 * Unconditionally release every button we currently believe is held.
 *
 * This deliberately bypasses zmk_mouse_ps2_activity_click_buttons(): that
 * function treats "more than one button changed at once" as a transmission
 * error and drops the whole update, which is exactly wrong when we are trying
 * to clear multiple stuck buttons. Releasing is always the safe direction, so
 * it is never filtered.
 *
 * Used on the wake / reset paths and whenever a packet is rejected as garbage,
 * so a phantom press decoded from a misaligned byte stream cannot leave the
 * host stuck in a drag.
 */
void zmk_mouse_ps2_release_all_buttons(void) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    if (data->button_l_is_held) {
        LOG_INF("Force-releasing button_l");
        input_report_key(data->dev, INPUT_BTN_0, 0, true, K_FOREVER);
        data->button_l_is_held = false;
    }
    if (data->button_r_is_held) {
        LOG_INF("Force-releasing button_r");
        input_report_key(data->dev, INPUT_BTN_1, 0, true, K_FOREVER);
        data->button_r_is_held = false;
    }
    if (data->button_m_is_held) {
        LOG_INF("Force-releasing button_m");
        input_report_key(data->dev, INPUT_BTN_2, 0, true, K_FOREVER);
        data->button_m_is_held = false;
    }
}

void zmk_mouse_ps2_activity_click_buttons(bool button_l, bool button_m, bool button_r) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;

    // TODO: Integrate this with the proper button mask instead
    // of hardcoding the mouse button indeces.
    // Check hid.c and zmk_hid_mouse_buttons_press() for more info.

    int buttons_pressed = 0;
    int buttons_released = 0;

    // First we check which mouse button press states have changed
    bool button_l_pressed = false;
    bool button_l_released = false;
    if (button_l == true && data->button_l_is_held == false) {
        LOG_INF("Pressed button_l");

        button_l_pressed = true;
        buttons_pressed++;
    } else if (button_l == false && data->button_l_is_held == true) {
        LOG_INF("Releasing button_l");

        button_l_released = true;
        buttons_released++;
    }

    bool button_m_released = false;
    bool button_m_pressed = false;
    if (button_m == true && data->button_m_is_held == false) {
        LOG_INF("Pressing button_m");

        button_m_pressed = true;
        buttons_pressed++;
    } else if (button_m == false && data->button_m_is_held == true) {
        LOG_INF("Releasing button_m");

        button_m_released = true;
        buttons_released++;
    }

    bool button_r_released = false;
    bool button_r_pressed = false;
    if (button_r == true && data->button_r_is_held == false) {
        LOG_INF("Pressing button_r");

        button_r_pressed = true;
        buttons_pressed++;
    } else if (button_r == false && data->button_r_is_held == true) {
        LOG_INF("Releasing button_r");

        button_r_released = true;
        buttons_released++;
    }

    // Then we check if this is likely a transmission error
    if (buttons_pressed > 1 || buttons_released > 1) {
        LOG_WRN("Ignoring button presses: Received %d button presses "
                "and %d button releases in one packet. "
                "Probably tranmission error.",
                buttons_pressed, buttons_released);

        zmk_mouse_ps2_activity_abort_cmd("Multiple button presses");
        return;
    }

    if (config->disable_clicking != true) {
        // If it wasn't, we actually send the events.
        if (buttons_pressed > 0 || buttons_released > 0) {

            int buttons_need_reporting = buttons_pressed + buttons_released;

            // Left button
            if (button_l_pressed) {

                input_report_key(data->dev, INPUT_BTN_0, 1,
                                 buttons_need_reporting == 1 ? true : false, K_FOREVER);
                data->button_l_is_held = true;
            } else if (button_l_released) {

                input_report_key(data->dev, INPUT_BTN_0, 0,
                                 buttons_need_reporting == 1 ? true : false, K_FOREVER);
                data->button_l_is_held = false;
            }

            buttons_need_reporting--;

            // Right button
            if (button_r_pressed) {

                input_report_key(data->dev, INPUT_BTN_1, 1,
                                 buttons_need_reporting == 1 ? true : false, K_FOREVER);
                data->button_r_is_held = true;
            } else if (button_r_released) {

                input_report_key(data->dev, INPUT_BTN_1, 0,
                                 buttons_need_reporting == 1 ? true : false, K_FOREVER);
                data->button_r_is_held = false;
            }

            buttons_need_reporting--;

            // Middle Button
            if (button_m_pressed) {

                input_report_key(data->dev, INPUT_BTN_2, 1,
                                 buttons_need_reporting == 1 ? true : false, K_FOREVER);
                data->button_m_is_held = true;
            } else if (button_m_released) {

                input_report_key(data->dev, INPUT_BTN_2, 0,
                                 buttons_need_reporting == 1 ? true : false, K_FOREVER);
                data->button_m_is_held = false;
            }
        }
    }
}

/*
 * PS/2 Command Sending Wrapper
 */
int zmk_mouse_ps2_activity_reporting_enable();
int zmk_mouse_ps2_activity_reporting_disable();

struct zmk_mouse_ps2_send_cmd_resp {
    int err;
    char err_msg[80];
    uint8_t resp_buffer[8];
    int resp_len;
};

struct zmk_mouse_ps2_send_cmd_resp zmk_mouse_ps2_send_cmd(char *cmd, int cmd_len, uint8_t *arg,
                                                          int resp_len, bool pause_reporting) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;
    const struct device *ps2_device = config->ps2_device;
    int err = 0;
    bool prev_activity_reporting_on = data->activity_reporting_on;

    struct zmk_mouse_ps2_send_cmd_resp resp = {
        .err = 0,
        .err_msg = "",
        .resp_len = 0,
    };
    memset(resp.resp_buffer, 0x0, sizeof(resp.resp_buffer));

    // Don't send the string termination NULL byte
    int cmd_bytes = cmd_len - 1;
    if (cmd_bytes < 1) {
        resp.err = -10;
        snprintf(resp.err_msg, sizeof(resp.err_msg),
                 "Cannot send cmd with less than 1 byte length");

        return resp;
    }

    if (resp_len > sizeof(resp.resp_buffer)) {
        resp.err = -11;
        snprintf(resp.err_msg, sizeof(resp.err_msg),
                 "Response can't be longer than the resp_buffer (%d)", sizeof(resp.err_msg));

        return resp;
    }

    if (pause_reporting == true && data->activity_reporting_on == true) {
        LOG_DBG("Disabling mouse activity reporting...");

        resp.err = zmk_mouse_ps2_activity_reporting_disable();
        if (resp.err) {
            snprintf(resp.err_msg, sizeof(resp.err_msg), "Could not disable data reporting (%d)",
                     err);
        }
    }

    if (resp.err == 0) {
        LOG_DBG("Sending cmd...");

        for (int i = 0; i < cmd_bytes; i++) {
            resp.err = ps2_write(ps2_device, cmd[i]);
            if (resp.err) {
                snprintf(resp.err_msg, sizeof(resp.err_msg), "Could not send cmd byte %d/%d (%d)",
                         i + 1, cmd_bytes, err);
                break;
            }
        }
    }

    if (resp.err == 0 && arg != NULL) {
        LOG_DBG("Sending arg...");
        resp.err = ps2_write(ps2_device, *arg);
        if (resp.err) {
            snprintf(resp.err_msg, sizeof(resp.err_msg), "Could not send arg (%d)", err);
        }
    }

    if (resp.err == 0 && resp_len > 0) {
        LOG_DBG("Reading response...");
        for (int i = 0; i < resp_len; i++) {
            resp.err = ps2_read(ps2_device, &resp.resp_buffer[i]);
            if (resp.err) {
                snprintf(resp.err_msg, sizeof(resp.err_msg),
                         "Could not read response cmd byte %d/%d (%d)", i + 1, resp_len, err);
                break;
            }
        }
    }

    if (pause_reporting == true && prev_activity_reporting_on == true) {
        LOG_DBG("Enabling mouse activity reporting...");

        err = zmk_mouse_ps2_activity_reporting_enable();
        if (err) {
            // Don' overwrite existing error
            if (resp.err == 0) {
                resp.err = err;
                snprintf(resp.err_msg, sizeof(resp.err_msg),
                         "Could not re-enable data reporting (%d)", err);
            }
        }
    }

    return resp;
}

int zmk_mouse_ps2_activity_reporting_enable() {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;
    const struct device *ps2_device = config->ps2_device;

    if (data->activity_reporting_on == true) {
        return 0;
    }

    uint8_t cmd = MOUSE_PS2_CMD_ENABLE_REPORTING[0];
    int err = ps2_write(ps2_device, cmd);
    if (err) {
        LOG_ERR("Could not enable data reporting: %d", err);
        return err;
    }

    err = ps2_enable_callback(ps2_device);
    if (err) {
        LOG_ERR("Could not enable ps2 callback: %d", err);
        return err;
    }

    data->activity_reporting_on = true;

    // CRITICAL: Clear buffer AFTER enabling reporting to discard any "race condition" packets
    // that arrived while the system was still booting/connecting.
    zmk_mouse_ps2_activity_reset_packet_buffer();
    // Discard ~200ms of initial noise. If a caller (e.g. the wake path) already
    // requested a wider window, that request is preserved.
    zmk_mouse_ps2_request_wake_up_discard(20);

    return 0;
}

/*
 * Force the reporting state, ignoring the cached activity_reporting_on flag.
 *
 * The normal enable/disable pair is guarded by that flag so repeated calls are
 * cheap, and it only updates the flag after ps2_write() succeeds. Together those
 * two properties can wedge the driver permanently:
 *
 *   1. Something goes wrong on the wire (a desynced TrackPoint, a dropped or
 *      late ACK) while sending 0xF5. The device *does* stop reporting, but
 *      ps2_write() reports failure, so disable() returns early with the flag
 *      still true.
 *   2. Any later enable() sees the flag already true and returns 0 without ever
 *      putting 0xF4 on the wire.
 *
 * The driver now believes reporting is on while the device has it off. No amount
 * of retrying escapes it, because every retry takes the same two early exits —
 * which is exactly why a wedged TrackPoint stayed dead until a deep-sleep or
 * power-cycle reinitialised the flag from its static initialiser.
 *
 * This variant always writes the command and always syncs the flag to the
 * requested state, even on write failure: once a write has failed the device's
 * true state is unknown, and assuming it took effect is strictly better than
 * leaving a stale flag that blocks all future attempts. Used only by the
 * user-triggered reset path, where "recover no matter what" beats "avoid a
 * redundant byte".
 */
static int zmk_mouse_ps2_reporting_force_resync(bool enable) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;
    const struct device *ps2_device = config->ps2_device;

    uint8_t cmd = enable ? MOUSE_PS2_CMD_ENABLE_REPORTING[0] : MOUSE_PS2_CMD_DISABLE_REPORTING[0];

    int err = ps2_write(ps2_device, cmd);
    if (err) {
        LOG_WRN("Force-resync: 0x%02x write failed (%d); syncing flag anyway", cmd, err);
    }

    if (enable) {
        int cb_err = ps2_enable_callback(ps2_device);
        if (cb_err) {
            LOG_WRN("Force-resync: could not enable ps2 callback (%d)", cb_err);
        }
    } else {
        int cb_err = ps2_disable_callback(ps2_device);
        if (cb_err) {
            LOG_WRN("Force-resync: could not disable ps2 callback (%d)", cb_err);
        }
    }

    data->activity_reporting_on = enable;

    return err;
}

int zmk_mouse_ps2_activity_reporting_disable() {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;
    const struct device *ps2_device = config->ps2_device;

    if (data->activity_reporting_on == false) {
        return 0;
    }

    uint8_t cmd = MOUSE_PS2_CMD_DISABLE_REPORTING[0];
    int err = ps2_write(ps2_device, cmd);
    if (err) {
        LOG_ERR("Could not disable data reporting: %d", err);
        return err;
    }

    err = ps2_disable_callback(ps2_device);
    if (err) {
        LOG_ERR("Could not disable ps2 callback: %d", err);
        return err;
    }

    data->activity_reporting_on = false;

    return 0;
}

/*
 * PS/2 Command Helpers
 */

int zmk_mouse_ps2_array_get_elem_index(int elem, int *array, size_t array_size) {
    int elem_index = -1;
    for (int i = 0; i < array_size; i++) {
        if (array[i] == elem) {
            elem_index = i;
            break;
        }
    }

    return elem_index;
}

int zmk_mouse_ps2_array_get_next_elem(int elem, int *array, size_t array_size) {
    int elem_index = zmk_mouse_ps2_array_get_elem_index(elem, array, array_size);
    if (elem_index == -1) {
        return -1;
    }

    int next_index = elem_index + 1;
    if (next_index >= array_size) {
        return -1;
    }

    return array[next_index];
}

int zmk_mouse_ps2_array_get_prev_elem(int elem, int *array, size_t array_size) {
    int elem_index = zmk_mouse_ps2_array_get_elem_index(elem, array, array_size);
    if (elem_index == -1) {
        return -1;
    }

    int prev_index = elem_index - 1;
    if (prev_index < 0 || prev_index >= array_size) {
        return -1;
    }

    return array[prev_index];
}

/*
 * PS/2 Commands
 */

int zmk_mouse_ps2_reset(const struct device *ps2_device) {
    struct zmk_mouse_ps2_send_cmd_resp resp =
        zmk_mouse_ps2_send_cmd(MOUSE_PS2_CMD_RESET, sizeof(MOUSE_PS2_CMD_RESET), NULL,
                               MOUSE_PS2_CMD_RESET_RESP_LEN, false);
    if (resp.err) {
        LOG_ERR("Could not send reset cmd");
    }

    return resp.err;
}

int zmk_mouse_ps2_set_sampling_rate(uint8_t sampling_rate) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    int rate_idx = zmk_mouse_ps2_array_get_elem_index(sampling_rate, allowed_sampling_rates,
                                                      sizeof(allowed_sampling_rates));
    if (rate_idx == -1) {
        LOG_ERR("Requested to set illegal sampling rate: %d", sampling_rate);
        return -1;
    }

    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_SET_SAMPLING_RATE, sizeof(MOUSE_PS2_CMD_SET_SAMPLING_RATE), &sampling_rate,
        MOUSE_PS2_CMD_SET_SAMPLING_RATE_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not set sample rate to %d", sampling_rate);
        return resp.err;
    }

    data->sampling_rate = sampling_rate;

    LOG_INF("Successfully set sampling rate to %d", sampling_rate);

    return resp.err;
}

int zmk_mouse_ps2_get_device_id(uint8_t *device_id) {
    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_GET_DEVICE_ID, sizeof(MOUSE_PS2_CMD_GET_DEVICE_ID), NULL, 1, true);
    if (resp.err) {
        LOG_ERR("Could not get device id");
        return resp.err;
    }

    *device_id = resp.resp_buffer[0];

    return 0;
}

int zmk_mouse_ps2_set_packet_mode(zmk_mouse_ps2_packet_mode mode) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    if (mode == MOUSE_PS2_PACKET_MODE_PS2_DEFAULT) {
        // Do nothing. Mouse devices enable this by
        // default.
        return 0;
    }

    bool prev_activity_reporting_on = data->activity_reporting_on;
    zmk_mouse_ps2_activity_reporting_disable();

    // Setting a mouse mode is a bit like using a cheat code
    // in a video game.
    // You have to send a specific sequence of sampling rates.
    if (mode == MOUSE_PS2_PACKET_MODE_SCROLL) {

        zmk_mouse_ps2_set_sampling_rate(200);
        zmk_mouse_ps2_set_sampling_rate(100);
        zmk_mouse_ps2_set_sampling_rate(80);
    }

    // Scroll mouse + 5 buttons mode can be enabled with the
    // following sequence, but since I don't have a mouse to
    // test it, I am commenting it out for now.
    // else if(mode == MOUSE_PS2_PACKET_MODE_SCROLL_5_BUTTONS) {

    //     zmk_mouse_ps2_set_sampling_rate(200);
    //     zmk_mouse_ps2_set_sampling_rate(200);
    //     zmk_mouse_ps2_set_sampling_rate(80);
    // }

    uint8_t device_id;
    int err = zmk_mouse_ps2_get_device_id(&device_id);
    if (err) {
        LOG_ERR("Could not enable packet mode %d. Failed to get device id with "
                "error %d",
                mode, err);
    } else {
        if (device_id == 0x00) {
            LOG_ERR("Could not enable packet mode %d. The device does not "
                    "support it",
                    mode);

            data->packet_mode = MOUSE_PS2_PACKET_MODE_PS2_DEFAULT;
            err = 1;
        } else if (device_id == 0x03 || device_id == 0x04) {
            LOG_INF("Successfully activated packet mode %d. Mouse returned "
                    "device id: %d",
                    mode, device_id);

            data->packet_mode = MOUSE_PS2_PACKET_MODE_SCROLL;
            err = 0;
        }
        // else if(device_id == 0x04) {
        //     LOG_INF(
        //         "Successfully activated packet mode %d. Mouse returned device "
        //         "id: %d", mode, device_id
        //     );

        //     data->packet_mode = MOUSE_PS2_PACKET_MODE_SCROLL_5_BUTTONS;
        //     err = 0;
        // }
        else {
            LOG_ERR("Could not enable packet mode %d. Received an invalid "
                    "device id: %d",
                    mode, device_id);

            data->packet_mode = MOUSE_PS2_PACKET_MODE_PS2_DEFAULT;
            err = 1;
        }
    }

    // Restore sampling rate to prev value
    zmk_mouse_ps2_set_sampling_rate(data->sampling_rate);

    if (prev_activity_reporting_on == true) {
        zmk_mouse_ps2_activity_reporting_enable();
    }

    return err;
}

/*
 * Trackpoint Commands
 */

int zmk_mouse_ps2_tp_get_secondary_id(uint8_t *manufacturer_id, uint8_t *secondary_id) {
    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_GET_SECONDARY_ID, sizeof(MOUSE_PS2_CMD_TP_GET_SECONDARY_ID), NULL,
        MOUSE_PS2_CMD_TP_GET_SECONDARY_ID_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not get secondary id");
        return resp.err;
    }

    *manufacturer_id = resp.resp_buffer[0];
    *secondary_id = resp.resp_buffer[1];

    return 0;
}

int zmk_mouse_ps2_tp_get_rom_id(uint8_t *rom_id) {
    struct zmk_mouse_ps2_send_cmd_resp resp =
        zmk_mouse_ps2_send_cmd(MOUSE_PS2_CMD_TP_GET_ROM_ID, sizeof(MOUSE_PS2_CMD_TP_GET_ROM_ID),
                               NULL, MOUSE_PS2_CMD_TP_GET_ROM_ID_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not get secondary id");
        return resp.err;
    }

    *rom_id = resp.resp_buffer[0];

    return 0;
}

char *zmk_mouse_ps2_get_manufacturer_str(uint8_t manufacturer_id) {

    switch (manufacturer_id) {
    case 0x1:
        return "IBM";
    case 0x2:
        return "Alps";
    case 0x3:
        return "Elan";
    case 0x4:
        return "NXP";
    case 0x5:
        return "JYT Synaptics";
    case 0x6:
        return "Synaptics";
    }

    return "Unknown";
}

// On non-trackpoints this command is not supported and returns nothing.
//
// On trackpoints it returns the manufacturer id and firmware id.
//
// Trackpoints from IBM/Lenovo laptops up until aproximately 2016 used IBM
// trackpoints. After that they started to use other manufacturers.
//
// Page 19 of the IBM TP spec describes the features of different firmware ids.
int zmk_mouse_ps2_tp_get_device_info(bool *is_tp, uint8_t *tp_manufacturer_id,
                                     uint8_t *tp_secondary_id, uint8_t *tp_rom_id, char *device_str,
                                     int device_str_size) {

    int err = zmk_mouse_ps2_tp_get_secondary_id(tp_manufacturer_id, tp_secondary_id);
    if (err) {
        // Only TPs implement this command. So, if it fails, it means the
        // device is not a TP.

        *is_tp = false;
        *tp_manufacturer_id = 0x0;
        *tp_secondary_id = 0x0;
        *tp_rom_id = 0x0;

        snprintf(device_str, device_str_size, "Generic PS/2 Mouse");

        return 0;
    }

    *is_tp = true;

    err = zmk_mouse_ps2_tp_get_rom_id(tp_rom_id);
    if (err) {
        LOG_ERR("Could not determine TP rom id: %d", err);
        *tp_rom_id = 0x0;
        err = -1;
    }

    char *manufacturer_str = zmk_mouse_ps2_get_manufacturer_str(*tp_manufacturer_id);

    snprintf(device_str, device_str_size,
             "Trackpoint by %s (0x%02X); Secondary ID: 0x%02X; Rom Version: %02X", manufacturer_str,
             *tp_manufacturer_id, *tp_secondary_id, *tp_rom_id);

    return err;
}

int zmk_mouse_ps2_tp_get_config_byte(uint8_t *config_byte) {
    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_GET_CONFIG_BYTE, sizeof(MOUSE_PS2_CMD_TP_GET_CONFIG_BYTE), NULL,
        MOUSE_PS2_CMD_TP_GET_CONFIG_BYTE_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not read trackpoint config byte");
        return resp.err;
    }

    *config_byte = resp.resp_buffer[0];

    return 0;
}

int zmk_mouse_ps2_tp_set_config_option(int config_bit, bool enabled, char *descr) {
    uint8_t config_byte;
    int err = zmk_mouse_ps2_tp_get_config_byte(&config_byte);
    if (err) {
        return err;
    }

    bool is_enabled = MOUSE_PS2_GET_BIT(config_byte, config_bit);

    if (is_enabled == enabled) {
        LOG_DBG("Trackpoint %s was already %s... not doing anything.", descr,
                is_enabled ? "enabled" : "disabled");
        return 0;
    }

    LOG_DBG("Setting trackpoint %s: %s", descr, enabled ? "enabled" : "disabled");

    MOUSE_PS2_SET_BIT(config_byte, enabled, config_bit);

    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_SET_CONFIG_BYTE, sizeof(MOUSE_PS2_CMD_TP_SET_CONFIG_BYTE), &config_byte,
        MOUSE_PS2_CMD_TP_SET_CONFIG_BYTE_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not set trackpoint %s to %s", descr, enabled ? "enabled" : "disabled");
        return resp.err;
    }

    LOG_INF("Successfully set config option %s to %s", descr, enabled ? "enabled" : "disabled");

    return 0;
}

int zmk_mouse_ps2_tp_press_to_select_set(bool enabled) {
    int err = zmk_mouse_ps2_tp_set_config_option(MOUSE_PS2_TP_CONFIG_BIT_PRESS_TO_SELECT, enabled,
                                                 "Press To Select");

    return err;
}

int zmk_mouse_ps2_tp_invert_x_set(bool enabled) {
    int err =
        zmk_mouse_ps2_tp_set_config_option(MOUSE_PS2_TP_CONFIG_BIT_INVERT_X, enabled, "Invert X");

    return err;
}

int zmk_mouse_ps2_tp_invert_y_set(bool enabled) {
    int err =
        zmk_mouse_ps2_tp_set_config_option(MOUSE_PS2_TP_CONFIG_BIT_INVERT_Y, enabled, "Invert Y");

    return err;
}

int zmk_mouse_ps2_tp_swap_xy_set(bool enabled) {
    int err =
        zmk_mouse_ps2_tp_set_config_option(MOUSE_PS2_TP_CONFIG_BIT_SWAP_XY, enabled, "Swap XY");

    return err;
}

int zmk_mouse_ps2_tp_sensitivity_get(uint8_t *sensitivity) {
    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_GET_SENSITIVITY, sizeof(MOUSE_PS2_CMD_TP_GET_SENSITIVITY), NULL,
        MOUSE_PS2_CMD_TP_GET_SENSITIVITY_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not get trackpoint sensitivity");
        return resp.err;
    }

    // Convert uint8_t to float
    // 0x80 (128) represents 1.0
    uint8_t sensitivity_int = resp.resp_buffer[0];
    *sensitivity = sensitivity_int;

    LOG_DBG("Trackpoint sensitivity is %d", sensitivity_int);

    return 0;
}

int zmk_mouse_ps2_tp_sensitivity_set(int sensitivity) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    if (sensitivity < MOUSE_PS2_CMD_TP_SET_SENSITIVITY_MIN ||
        sensitivity > MOUSE_PS2_CMD_TP_SET_SENSITIVITY_MAX) {
        LOG_ERR("Invalid sensitivity value %d. Min: %d; Max: %d", sensitivity,
                MOUSE_PS2_CMD_TP_SET_SENSITIVITY_MIN, MOUSE_PS2_CMD_TP_SET_SENSITIVITY_MAX);
        return 1;
    }

    uint8_t arg = sensitivity;

    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_SET_SENSITIVITY, sizeof(MOUSE_PS2_CMD_TP_SET_SENSITIVITY), &arg,
        MOUSE_PS2_CMD_TP_SET_SENSITIVITY_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not set sensitivity to %d", sensitivity);
        return resp.err;
    }

    data->tp_sensitivity = sensitivity;

    LOG_INF("Successfully set TP sensitivity to %d", sensitivity);

    return 0;
}

/* ============================================================================
 * Async Sensitivity Adjustment Work Handler
 * ============================================================================
 * Executes on the dedicated PS/2 maintenance work queue, blocking is allowed
 * here (and kept off the system work queue that drives keyboard input).
 */
static void zmk_mouse_ps2_sensitivity_adjustment_work_handler(struct k_work *work) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    int amount;
    
    // Read and clear pending adjustment atomically
    k_mutex_lock(&sensitivity_adjustment_mutex, K_FOREVER);
    amount = pending_sensitivity_change;
    pending_sensitivity_change = 0;
    k_mutex_unlock(&sensitivity_adjustment_mutex);
    
    if (amount == 0) {
        return;  // Nothing to do
    }
    
    int new_val = data->tp_sensitivity + amount;
    
    LOG_INF("Async: Setting trackpoint sensitivity to %d (delta: %+d)", new_val, amount);
    int err = zmk_mouse_ps2_tp_sensitivity_set(new_val);
    if (err == 0) {
        zmk_mouse_ps2_settings_save();
    } else {
        LOG_ERR("Async: Failed to set sensitivity: %d", err);
    }
}

int zmk_mouse_ps2_tp_sensitivity_change(int amount) {
    k_mutex_lock(&sensitivity_adjustment_mutex, K_FOREVER);
    
    // Accumulate adjustment (supports rapid key presses)
    pending_sensitivity_change += amount;
    int total_pending = pending_sensitivity_change;  // Save for logging
    
    k_mutex_unlock(&sensitivity_adjustment_mutex);
    
    // Submit to the dedicated PS/2 maintenance queue (returns immediately).
    // This blocks on PS/2 writes (10-20ms), so it must not run on the system
    // work queue where it would compete with keyboard input processing.
    zmk_mouse_ps2_submit_work(&sensitivity_adjustment_work);
    
    LOG_DBG("Submitted sensitivity adjustment: %+d (async, total pending: %+d)", 
            amount, total_pending);
    return 0;
}

int zmk_mouse_ps2_tp_negative_inertia_get(uint8_t *neg_inertia) {
    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_GET_NEG_INERTIA, sizeof(MOUSE_PS2_CMD_TP_GET_NEG_INERTIA), NULL,
        MOUSE_PS2_CMD_TP_GET_NEG_INERTIA_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not get trackpoint negative inertia");
        return resp.err;
    }

    uint8_t neg_inertia_int = resp.resp_buffer[0];
    *neg_inertia = neg_inertia_int;

    LOG_DBG("Trackpoint negative inertia is %d", neg_inertia_int);

    return 0;
}

int zmk_mouse_ps2_tp_neg_inertia_set(int neg_inertia) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    if (neg_inertia < MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_MIN ||
        neg_inertia > MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_MAX) {
        LOG_ERR("Invalid negative inertia value %d. Min: %d; Max: %d", neg_inertia,
                MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_MIN, MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_MAX);
        return 1;
    }

    uint8_t arg = neg_inertia;

    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_SET_NEG_INERTIA, sizeof(MOUSE_PS2_CMD_TP_SET_NEG_INERTIA), &arg,
        MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not set negative inertia to %d", neg_inertia);
        return resp.err;
    }

    data->tp_neg_inertia = neg_inertia;

    LOG_INF("Successfully set TP negative inertia to %d", neg_inertia);

    return 0;
}

int zmk_mouse_ps2_tp_neg_inertia_change(int amount) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    int new_val = data->tp_neg_inertia + amount;

    LOG_INF("Setting negative inertia to %d", new_val);
    int err = zmk_mouse_ps2_tp_neg_inertia_set(new_val);
    if (err == 0) {

        zmk_mouse_ps2_settings_save();
    }

    return err;
}

int zmk_mouse_ps2_tp_value6_upper_plateau_speed_get(uint8_t *value6) {
    struct zmk_mouse_ps2_send_cmd_resp resp =
        zmk_mouse_ps2_send_cmd(MOUSE_PS2_CMD_TP_GET_VALUE6_UPPER_PLATEAU_SPEED,
                               sizeof(MOUSE_PS2_CMD_TP_GET_VALUE6_UPPER_PLATEAU_SPEED), NULL,
                               MOUSE_PS2_CMD_TP_GET_VALUE6_UPPER_PLATEAU_SPEED_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not get trackpoint value6 upper plateau speed");
        return resp.err;
    }

    uint8_t value6_int = resp.resp_buffer[0];
    *value6 = value6_int;

    LOG_DBG("Trackpoint value6 upper plateau speed is %d", value6_int);

    return 0;
}

int zmk_mouse_ps2_tp_value6_upper_plateau_speed_set(int value6) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    if (value6 < MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_MIN ||
        value6 > MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_MAX) {
        LOG_ERR("Invalid value6 upper plateau speed value %d. Min: %d; Max: %d", value6,
                MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_MIN,
                MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_MAX);
        return 1;
    }

    uint8_t arg = value6;

    struct zmk_mouse_ps2_send_cmd_resp resp =
        zmk_mouse_ps2_send_cmd(MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED,
                               sizeof(MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED), &arg,
                               MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not set value6 upper plateau speed to %d", value6);
        return resp.err;
    }

    data->tp_value6 = value6;

    LOG_INF("Successfully set TP value6 upper plateau speed to %d", value6);

    return 0;
}

int zmk_mouse_ps2_tp_value6_upper_plateau_speed_change(int amount) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    int new_val = data->tp_value6 + amount;

    LOG_INF("Setting value6 upper plateau speed to %d", new_val);
    int err = zmk_mouse_ps2_tp_value6_upper_plateau_speed_set(new_val);
    if (err == 0) {

        zmk_mouse_ps2_settings_save();
    }

    return err;
}

int zmk_mouse_ps2_tp_pts_threshold_get(uint8_t *pts_threshold) {
    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_GET_PTS_THRESHOLD, sizeof(MOUSE_PS2_CMD_TP_GET_PTS_THRESHOLD), NULL,
        MOUSE_PS2_CMD_TP_GET_PTS_THRESHOLD_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not get trackpoint press-to-select threshold");
        return resp.err;
    }

    uint8_t pts_threshold_int = resp.resp_buffer[0];
    *pts_threshold = pts_threshold_int;

    LOG_DBG("Trackpoint press-to-select threshold is %d", pts_threshold_int);

    return 0;
}

int zmk_mouse_ps2_tp_pts_threshold_set(int pts_threshold) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    if (pts_threshold < MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_MIN ||
        pts_threshold > MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_MAX) {
        LOG_ERR("Invalid press-to-select threshold value %d. Min: %d; Max: %d", pts_threshold,
                MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_MIN, MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_MAX);
        return 1;
    }

    uint8_t arg = pts_threshold;

    struct zmk_mouse_ps2_send_cmd_resp resp = zmk_mouse_ps2_send_cmd(
        MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD, sizeof(MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD), &arg,
        MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_RESP_LEN, true);
    if (resp.err) {
        LOG_ERR("Could not set press-to-select threshold to %d", pts_threshold);
        return resp.err;
    }

    data->tp_pts_threshold = pts_threshold;

    LOG_INF("Successfully set TP press-to-select threshold to %d", pts_threshold);

    return 0;
}

int zmk_mouse_ps2_tp_pts_threshold_change(int amount) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    int new_val = data->tp_pts_threshold + amount;

    LOG_INF("Setting press-to-select threshold to %d", new_val);
    int err = zmk_mouse_ps2_tp_pts_threshold_set(new_val);
    if (err == 0) {

        zmk_mouse_ps2_settings_save();
    }

    return err;
}

/*
 * State Saving
 */

/* Debounce window for flushing PS/2 mouse settings to flash.
 *
 * Deliberately NOT CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE. That is a global ZMK
 * setting whose upstream default is 60s, which is long enough for the board to
 * enter deep sleep before the write ever lands. When that happens the MCU
 * powers down, the pending work item dies with the RAM it lived in, and every
 * runtime adjustment made through `&mms` is silently lost: the new value is
 * live in the TrackPoint until the next boot, then reverts to whatever is in
 * flash. Raising the global would change the save cadence of every other ZMK
 * subsystem, so this driver keeps its own shorter window instead.
 *
 * A short window is safe here: these saves happen only when the user actively
 * adjusts a setting, and each one writes four single-byte values. */
#define MOUSE_PS2_SETTINGS_SAVE_DEBOUNCE_MS 10000

#if IS_ENABLED(CONFIG_SETTINGS)

struct k_work_delayable zmk_mouse_ps2_save_work;

int zmk_mouse_ps2_settings_save_setting(char *setting_name, const void *value, size_t val_len) {
    char setting_path[40];
    snprintf(setting_path, sizeof(setting_path), "%s/%s", MOUSE_PS2_SETTINGS_SUBTREE, setting_name);

    LOG_DBG("Saving setting to `%s`", setting_path);
    int err = settings_save_one(setting_path, value, val_len);
    if (err) {
        LOG_ERR("Could not save setting to `%s`: %d", setting_path, err);
    }

    return err;
}

int zmk_mouse_ps2_settings_reset_setting(char *setting_name) {
    char setting_path[40];
    snprintf(setting_path, sizeof(setting_path), "%s/%s", MOUSE_PS2_SETTINGS_SUBTREE, setting_name);

    LOG_DBG("Reseting setting `%s`", setting_path);
    int err = settings_delete(setting_path);
    if (err) {
        LOG_ERR("Could not reset setting `%s`", setting_path);
    }

    return err;
}

static void zmk_mouse_ps2_settings_save_work(struct k_work *work) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    LOG_INF("Saving PS/2 Mouse Settings.");

    zmk_mouse_ps2_settings_save_setting(MOUSE_PS2_ST_TP_SENSITIVITY, &data->tp_sensitivity,
                                        sizeof(data->tp_sensitivity));
    zmk_mouse_ps2_settings_save_setting(MOUSE_PS2_ST_TP_NEG_INERTIA, &data->tp_neg_inertia,
                                        sizeof(data->tp_neg_inertia));
    zmk_mouse_ps2_settings_save_setting(MOUSE_PS2_ST_TP_VALUE6, &data->tp_value6,
                                        sizeof(data->tp_value6));
    zmk_mouse_ps2_settings_save_setting(MOUSE_PS2_ST_TP_PTS_THRESHOLD, &data->tp_pts_threshold,
                                        sizeof(data->tp_pts_threshold));
}
#endif

int zmk_mouse_ps2_settings_save() {
    LOG_DBG("");

#if IS_ENABLED(CONFIG_SETTINGS)
    int ret =
        k_work_reschedule(&zmk_mouse_ps2_save_work, K_MSEC(MOUSE_PS2_SETTINGS_SAVE_DEBOUNCE_MS));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_mouse_ps2_settings_reset() {

    LOG_INF("Deleting runtime settings...");
    zmk_mouse_ps2_settings_reset_setting(MOUSE_PS2_ST_TP_SENSITIVITY);
    zmk_mouse_ps2_settings_reset_setting(MOUSE_PS2_ST_TP_NEG_INERTIA);
    zmk_mouse_ps2_settings_reset_setting(MOUSE_PS2_ST_TP_VALUE6);
    zmk_mouse_ps2_settings_reset_setting(MOUSE_PS2_ST_TP_PTS_THRESHOLD);

    LOG_INF("Restoring default settings to TP..");
    zmk_mouse_ps2_tp_sensitivity_set(MOUSE_PS2_CMD_TP_SET_SENSITIVITY_DEFAULT);

    zmk_mouse_ps2_tp_neg_inertia_set(MOUSE_PS2_CMD_TP_SET_NEG_INERTIA_DEFAULT);

    zmk_mouse_ps2_tp_value6_upper_plateau_speed_set(
        MOUSE_PS2_CMD_TP_SET_VALUE6_UPPER_PLATEAU_SPEED_DEFAULT);

    zmk_mouse_ps2_tp_pts_threshold_set(MOUSE_PS2_CMD_TP_SET_PTS_THRESHOLD_DEFAULT);

    return 0;
}

int zmk_mouse_ps2_settings_log() {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    char settings_str[250];

    snprintf(settings_str, sizeof(settings_str), " \n\
&mouse_ps2_conf = { \n\
    tp-sensitivity = <%d>; \n\
    tp-neg-inertia = <%d>; \n\
    tp-val6-upper-speed = <%d>; \n\
    tp-tp-press-to-select-threshold = <%d>; \n\
}",
             data->tp_sensitivity, data->tp_neg_inertia, data->tp_value6, data->tp_pts_threshold);

    LOG_INF("Current settings... %s", settings_str);

    return 0;
}

// This function is called when settings are loaded from flash by
// `settings_load_subtree`.
// It's called once for each PS/2 mouse setting that has been stored.
static int zmk_mouse_ps2_settings_restore(const char *name, size_t len, settings_read_cb read_cb,
                                          void *cb_arg) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;

    uint8_t setting_val;

    if (len != sizeof(setting_val)) {
        LOG_ERR("Could not restore settings %s: Len mismatch", name);

        return -EINVAL;
    }

    int rc = read_cb(cb_arg, &setting_val, sizeof(setting_val));
    if (rc <= 0) {
        LOG_ERR("Could not restore setting %s: %d", name, rc);
        return -EINVAL;
    }

    if (data->is_trackpoint == false) {
        LOG_INF("Mouse device is not a trackpoint. Not restoring setting %s.", name);

        return 0;
    }

    LOG_INF("Restoring setting %s with value: %d", name, setting_val);

    if (strcmp(name, MOUSE_PS2_ST_TP_SENSITIVITY) == 0) {

        if (config->tp_sensitivity != -1) {
            LOG_WRN("Not restoring runtime settings for %s with value %d, because deviceconfig "
                    "defines the setting with value %d",
                    name, setting_val, config->tp_sensitivity);

            return 0;
        }

        return zmk_mouse_ps2_tp_sensitivity_set(setting_val);
    } else if (strcmp(name, MOUSE_PS2_ST_TP_NEG_INERTIA) == 0) {
        if (config->tp_neg_inertia != -1) {
            LOG_WRN("Not restoring runtime settings for %s with value %d, because deviceconfig "
                    "defines the setting with value %d",
                    name, setting_val, config->tp_neg_inertia);

            return 0;
        }

        return zmk_mouse_ps2_tp_neg_inertia_set(setting_val);
    } else if (strcmp(name, MOUSE_PS2_ST_TP_VALUE6) == 0) {
        if (config->tp_val6_upper_speed != -1) {
            LOG_WRN("Not restoring runtime settings for %s with value %d, because deviceconfig "
                    "defines the setting with value %d",
                    name, setting_val, config->tp_val6_upper_speed);

            return 0;
        }

        return zmk_mouse_ps2_tp_value6_upper_plateau_speed_set(setting_val);
    } else if (strcmp(name, MOUSE_PS2_ST_TP_PTS_THRESHOLD) == 0) {
        if (config->tp_press_to_select_threshold != -1) {
            LOG_WRN("Not restoring runtime settings for %s with value %d, because deviceconfig "
                    "defines the setting with value %d",
                    name, setting_val, config->tp_press_to_select_threshold);

            return 0;
        }

        return zmk_mouse_ps2_tp_pts_threshold_set(setting_val);
    }

    return -EINVAL;
}

struct settings_handler zmk_mouse_ps2_settings_conf = {
    .name = MOUSE_PS2_SETTINGS_SUBTREE,
    .h_set = zmk_mouse_ps2_settings_restore,
};

int zmk_mouse_ps2_settings_init() {
#if IS_ENABLED(CONFIG_SETTINGS)
    LOG_DBG("");

    settings_subsys_init();

    int err = settings_register(&zmk_mouse_ps2_settings_conf);
    if (err) {
        LOG_ERR("Failed to register the PS/2 mouse settings handler (err %d)", err);
        return err;
    }

    k_work_init_delayable(&zmk_mouse_ps2_save_work, zmk_mouse_ps2_settings_save_work);

    // This will load the settings and then call
    // `zmk_mouse_ps2_settings_restore`, which will set the settings
    settings_load_subtree(MOUSE_PS2_SETTINGS_SUBTREE);
#endif

    return 0;
}

/*
 * Init
 */

static void zmk_mouse_ps2_init_thread(int dev_ptr, int unused);

static int zmk_mouse_ps2_init(const struct device *dev) {
    LOG_DBG("Inside zmk_mouse_ps2_init");

    /* Start the dedicated PS/2 maintenance work queue before anything can
     * submit to it. Runs at POST_KERNEL, well ahead of any activity-state
     * change or user-triggered reset. */
    k_work_queue_start(&zmk_mouse_ps2_work_queue, zmk_mouse_ps2_work_queue_stack,
                       K_THREAD_STACK_SIZEOF(zmk_mouse_ps2_work_queue_stack),
                       MOUSE_PS2_WORK_QUEUE_PRIORITY, NULL);
    k_thread_name_set(&zmk_mouse_ps2_work_queue.thread, "mouse_ps2_wq");
    zmk_mouse_ps2_work_queue_ready = true;

    LOG_DBG("Creating mouse_ps2 init thread.");
    k_thread_create(&zmk_mouse_ps2_data.thread, zmk_mouse_ps2_data.thread_stack,
                    MOUSE_PS2_THREAD_STACK_SIZE, (k_thread_entry_t)zmk_mouse_ps2_init_thread,
                    (struct device *)dev, 0, NULL, K_PRIO_COOP(MOUSE_PS2_THREAD_PRIORITY), 0,
                    K_MSEC(ZMK_MOUSE_PS2_INIT_THREAD_DELAY_MS));

    return 0;
}

static void zmk_mouse_ps2_init_thread(int dev_ptr, int unused) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    int err;

    data->dev = INT_TO_POINTER(dev_ptr);

    const struct zmk_mouse_ps2_config *config = data->dev->config;

    /* Assert VCC before doing anything else.
     *
     * Without this, startup depends on the vcc-gpios pin already happening to
     * be in a state that powers the TrackPoint, and there are two ways it is
     * not. On a true cold boot the pin is hi-Z, and on an nRF52 deep-sleep wake
     * (System OFF, which resumes by resetting the SoC and re-running init) the
     * pin was explicitly driven inactive by power_down() on the way into sleep.
     * In both cases init_wait_for_mouse() below then talks to an unpowered
     * device, burns all MOUSE_PS2_INIT_ATTEMPTS and gives up permanently — the
     * pointer stays dead for the whole session while the keys and encoder,
     * being unrelated subsystems, work fine. Asserting VCC here makes init
     * self-sufficient instead of dependent on leftover pin state.
     *
     * No-op on boards without a vcc-gpios switch. */
    zmk_mouse_ps2_vcc_set(true);
    k_sleep(K_MSEC(CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING_WAKE_DELAY_MS));

    zmk_mouse_ps2_init_power_on_reset();

    LOG_INF("Waiting for mouse to connect...");
    err = zmk_mouse_ps2_init_wait_for_mouse(data->dev);
    if (err) {
        LOG_ERR("Could not init a mouse in %d attempts. Giving up. "
                "Power cycle the mouse and reset zmk to try again.",
                MOUSE_PS2_INIT_ATTEMPTS);
        return;
    }

    if (config->sampling_rate != MOUSE_PS2_CMD_SET_SAMPLING_RATE_DEFAULT) {

        LOG_INF("Setting sample rate to %d...", config->sampling_rate);
        zmk_mouse_ps2_set_sampling_rate(config->sampling_rate);
        if (err) {
            LOG_ERR("Could not set sampling rate to %d: %d", config->sampling_rate, err);
            return;
        }
    }

    char device_descr[64] = "undetermined device";
    zmk_mouse_ps2_tp_get_device_info(&data->is_trackpoint, &data->manufacturer_id,
                                     &data->secondary_id, &data->rom_id, device_descr,
                                     sizeof(device_descr));

    LOG_INF("Connected device is a %s", device_descr);

    zmk_mouse_ps2_apply_tp_settings();

    if (config->scroll_mode) {
        LOG_INF("Enabling scroll mode.");
        zmk_mouse_ps2_set_packet_mode(MOUSE_PS2_PACKET_MODE_SCROLL);
    }

    zmk_mouse_ps2_settings_init();

    // Configure read callback
    LOG_DBG("Configuring ps2 callback...");
#if IS_ENABLED(CONFIG_ZMK_INPUT_MOUSE_PS2_ENABLE_PS2_RESEND_CALLBACK)

    err = ps2_config(config->ps2_device, &zmk_mouse_ps2_activity_callback,
                     &zmk_mouse_ps2_activity_resend_callback);

#else

    err = ps2_config(config->ps2_device, &zmk_mouse_ps2_activity_callback);

#endif /* IS_ENABLED(CONFIG_ZMK_INPUT_MOUSE_PS2_ENABLE_PS2_RESEND_CALLBACK) */

    if (err) {
        LOG_ERR("Could not configure ps2 interface: %d", err);
        return;
    }

    LOG_INF("Enabling data reporting and ps2 callback...");
    err = zmk_mouse_ps2_activity_reporting_enable();
    if (err) {
        LOG_ERR("Could not activate ps2 callback: %d", err);
    } else {
        LOG_DBG("Successfully activated ps2 callback");
    }

    k_work_init_delayable(&data->packet_buffer_timeout, zmk_mouse_ps2_activity_packet_timout);

    return;
}

// Power-On-Reset for trackpoints (and possibly other devices).
// From the `IBM TrackPoint System Version 4.0 Engineering
// Specification`...
// "The TrackPoint logic shall execute a Power On Reset (POR) when power is
//  applied to the device. The POR shall be timed to occur 600 ms ± 20 % from
//  the time power is applied to the TrackPoint controller. Activity on the
//  clock and data lines is ignored prior to the completion of the diagnostic
//  sequence. (See RESET mode of operation.)"
int zmk_mouse_ps2_init_power_on_reset() {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;

    // Check if the optional rst-gpios setting was set
    if (config->rst_gpio.port == NULL) {
        return 0;
    }

    LOG_INF("Performing Power-On-Reset on pin P%d.%02d...", config->rst_gpio_port_num,
            config->rst_gpio.pin);

    if (data->rst_gpio.port == NULL) {
        data->rst_gpio = config->rst_gpio;

        // Overwrite any user-provided flags from the devicetree
        data->rst_gpio.dt_flags = 0;
    }

    //  Set reset pin low...
    int err = gpio_pin_configure_dt(&data->rst_gpio, (GPIO_OUTPUT_HIGH));
    if (err) {
        LOG_ERR("Failed Power-On-Reset: Failed to configure RST GPIO pin to "
                "output low (err %d)",
                err);
        return err;
    }

    // Wait 600ms
    k_sleep(MOUSE_PS2_POWER_ON_RESET_TIME);

    // Set pin high
    err = gpio_pin_set_dt(&data->rst_gpio, 0);
    if (err) {
        LOG_ERR("Failed Power-On-Reset: Failed to set RST GPIO pin to "
                "low (err %d)",
                err);
        return err;
    }

    LOG_DBG("Finished Power-On-Reset successfully...");

    // CRITICAL: Clear any accumulated data in the buffer from movement during sleep/wake
    // This prevents protocol desynchronization.
    zmk_mouse_ps2_activity_reset_packet_buffer();

    // Widen the post-wake discard window to cover more potential misalignment.
    // Increased from 10 to 30 packets (~300ms at 100Hz) to better handle cases
    // where stale bytes or protocol desync cause phantom left drift on wake.
    zmk_mouse_ps2_request_wake_up_discard(30);

    return 0;
}

/*
 * Wait for a PS/2 mouse to identify itself, giving up after `attempts` rounds.
 *
 * The retry budget is a parameter because the two callers have opposite needs.
 * Boot can afford to be patient (the device may still be powering up, and there
 * is nothing else to do). The user-triggered reset cannot: every failing round
 * costs a blocking ps2_read timeout plus, on odd rounds, a 5-second sleep, and
 * all of it runs on the single-threaded PS/2 maintenance queue. At the boot
 * budget of 10 that is ~25s of sleeping alone, during which the queue is
 * occupied and further MS_TP_RESET presses coalesce into the already-queued work
 * item and appear to do nothing — the "pressing it repeatedly doesn't help"
 * symptom. A short budget keeps the reset interactive.
 */
int zmk_mouse_ps2_init_wait_for_mouse_attempts(const struct device *dev, int attempts) {
    const struct zmk_mouse_ps2_config *config = dev->config;
    int err;

    uint8_t read_val;

    for (int i = 0; i < attempts; i++) {

        LOG_INF("Trying to initialize mouse device (attempt %d / %d)", i + 1, attempts);

        // PS/2 Devices do a self-test and send the result when they power up.

        err = ps2_read(config->ps2_device, &read_val);
        if (err == 0) {
            if (read_val != MOUSE_PS2_RESP_SELF_TEST_PASS) {
                LOG_WRN("Got invalid PS/2 self-test result: 0x%x", read_val);

                LOG_INF("Trying to reset PS2 device...");
                zmk_mouse_ps2_reset(config->ps2_device);

                continue;
            }

            LOG_INF("PS/2 Device passed self-test: 0x%x", read_val);

            // Read device id
            LOG_INF("Reading PS/2 device id...");
            err = ps2_read(config->ps2_device, &read_val);
            if (err) {
                LOG_WRN("Could not read PS/2 device id: %d", err);
            } else {
                if (read_val == 0) {
                    LOG_INF("Connected PS/2 device is a mouse...");
                    return 0;
                } else {
                    LOG_WRN("PS/2 device is not a mouse: 0x%x", read_val);
                    return 1;
                }
            }
        } else {
            LOG_WRN("Could not read PS/2 device self-test result: %d. ", err);
        }

        // But when a zmk device is reset, it doesn't cut the power to external
        // devices. So the device acts as if it was never disconnected.
        // So we try sending the reset command.
        if (i % 2 == 0) {
            LOG_INF("Trying to reset PS2 device...");
            zmk_mouse_ps2_reset(config->ps2_device);
            continue;
        }

        /* Only the patient (boot) budget sleeps between rounds. The short
         * budget used by the reset path must stay responsive. */
        if (attempts >= MOUSE_PS2_INIT_ATTEMPTS) {
            k_sleep(K_SECONDS(5));
        }
    }

    return 1;
}

int zmk_mouse_ps2_init_wait_for_mouse(const struct device *dev) {
    return zmk_mouse_ps2_init_wait_for_mouse_attempts(dev, MOUSE_PS2_INIT_ATTEMPTS);
}

// Depends on the UART and PS2 init priorities, which are 55 and 45 by default
#define ZMK_MOUSE_PS2_INIT_PRIORITY 90

/*
 * ------------------------------------------------------------------
 * Idle / Active Power Control
 * ------------------------------------------------------------------
 *
 * When CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING is enabled, a small
 * "idle-pm" glue module subscribes to ZMK activity state changes and
 * calls zmk_mouse_ps2_power_down() / zmk_mouse_ps2_power_up() from a
 * work queue. Both functions are no-ops if the feature is disabled.
 *
 * The sequence was designed to survive the following failure modes
 * observed on nice!nano v2 + TrackPoint:
 *
 *   1. Cutting VCC alone re-starts TP internal state (reporting=off,
 *      config lost). The host must re-run POR, re-enable reporting,
 *      and re-apply all TP tunables (sensitivity, inertia, ...).
 *   2. The PS/2 self-test bytes (0xAA 0x00) emitted after VCC is
 *      restored will otherwise poison the mouse packet parser.
 *   3. While VCC is cut, the MCU's SCL/SDA GPIOs would back-power the
 *      TP through its ESD diodes (parasitic 1.5-2V), preventing a
 *      real power-off. So before cutting VCC we switch the UART
 *      pinctrl to a low-power state and drive SCL as a low output.
 */

/*
 * Re-applies every TrackPoint tunable that lives in volatile RAM on
 * the TP side.
 *
 * When from_wake=false (init path): uses DTS config values. The settings
 * subsystem will later override via zmk_mouse_ps2_settings_restore().
 *
 * When from_wake=true (wake path): uses data->tp_* live RAM values which
 * already reflect any runtime &mms adjustments the user made.
 */
static void zmk_mouse_ps2_apply_tp_settings_impl(bool from_wake) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;

    if (!data->is_trackpoint) {
        return;
    }

    if (config->tp_press_to_select) {
        LOG_INF("Enabling TP press to select...");
        zmk_mouse_ps2_tp_press_to_select_set(true);
    }

    if (from_wake) {
        /* Wake path: push the live RAM-cached values back into the TP. */
        LOG_INF("Wake: restoring TP sensitivity=%u inertia=%u value6=%u pts=%u",
                data->tp_sensitivity, data->tp_neg_inertia, data->tp_value6,
                data->tp_pts_threshold);
        zmk_mouse_ps2_tp_sensitivity_set(data->tp_sensitivity);
        zmk_mouse_ps2_tp_neg_inertia_set(data->tp_neg_inertia);
        zmk_mouse_ps2_tp_value6_upper_plateau_speed_set(data->tp_value6);
        zmk_mouse_ps2_tp_pts_threshold_set(data->tp_pts_threshold);
    } else {
        /* Init path: use DTS values (settings_restore will override later). */
        if (config->tp_press_to_select_threshold != -1) {
            zmk_mouse_ps2_tp_pts_threshold_set(config->tp_press_to_select_threshold);
        }
        if (config->tp_sensitivity != -1) {
            LOG_INF("Setting TP sensitivity to %d...", config->tp_sensitivity);
            zmk_mouse_ps2_tp_sensitivity_set(config->tp_sensitivity);
        }
        if (config->tp_neg_inertia != -1) {
            LOG_INF("Setting TP inertia to %d...", config->tp_neg_inertia);
            zmk_mouse_ps2_tp_neg_inertia_set(config->tp_neg_inertia);
        }
        if (config->tp_val6_upper_speed != -1) {
            LOG_INF("Setting TP value 6 to %d...", config->tp_val6_upper_speed);
            zmk_mouse_ps2_tp_value6_upper_plateau_speed_set(config->tp_val6_upper_speed);
        }
    }

    if (config->tp_x_invert) {
        zmk_mouse_ps2_tp_invert_x_set(true);
    }
    if (config->tp_y_invert) {
        zmk_mouse_ps2_tp_invert_y_set(true);
    }
    if (config->tp_xy_swap) {
        zmk_mouse_ps2_tp_swap_xy_set(true);
    }
}

/* Init path wrapper */
void zmk_mouse_ps2_apply_tp_settings(void) {
    zmk_mouse_ps2_apply_tp_settings_impl(false);
}

/*
 * Drives a GPIO-connected power switch to the "off" state. Uses the
 * devicetree flags (e.g. GPIO_ACTIVE_HIGH on nice!nano v2's P0.13)
 * so a raw value of 0 here translates to "inactive" on the pin.
 *
 * Deliberately outside CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING: whether a
 * vcc-gpios switch exists is a board property, independent of whether we idle
 * the device. The init path needs to assert VCC regardless, so keeping this
 * behind the idle-power-saving guard would leave startup unable to power the
 * TrackPoint on boards that have the switch.
 */
static void zmk_mouse_ps2_vcc_set(bool powered) {
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;

    if (config->vcc_gpio.port == NULL) {
        return;
    }

    /* Re-configure as output every time: on cold boot the pin is hi-Z.
     * gpio_pin_set_dt respects the ACTIVE_HIGH/ACTIVE_LOW flag.
     */
    int err = gpio_pin_configure_dt(&config->vcc_gpio,
                                    powered ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure VCC GPIO (%d)", err);
        return;
    }
    LOG_INF("TrackPoint VCC -> %s (P%d.%02d)", powered ? "ON" : "OFF",
            config->vcc_gpio_port_num, config->vcc_gpio.pin);
}

/*
 * Parks the SCL / SDA pins in a configuration that does not back-power
 * the (now-unpowered) TrackPoint via its ESD clamp diodes.
 *
 * Deliberately compiled unconditionally, like zmk_mouse_ps2_vcc_set(). Parking
 * the pins is not a power-saving feature: it is a *precondition for cutting VCC
 * at all*. Cutting VCC while SCL/SDA are still driven leaves the TrackPoint
 * sitting at a parasitic 1.5-2V through its clamp diodes — below its operating
 * voltage but above zero, i.e. a brownout state in which it neither runs nor
 * resets. The user-triggered full reset (MS_TP_RESET) needs a true 0V power
 * cycle to recover a wedged device, so it must be able to call this regardless
 * of whether idle power saving is enabled.
 *
 * - SDA belongs to the UART peripheral, so we switch the UART pinctrl
 *   to its "sleep" state. The user's DTS must define uart0_ps2_sleep
 *   with low-power-enable (the example config already does).
 * - SCL is an ordinary GPIO owned by the PS/2 driver. Driving it as
 *   an output-low is the safest: the line is clamped to GND, which
 *   is below TP's VCC-when-off (0 V) and no current flows through
 *   the diode either direction.
 * - We also suspend the UART device so the peripheral HFCLK request
 *   is released.
 */
static void zmk_mouse_ps2_transport_suspend(void) {
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;
    const struct device *ps2_dev = config->ps2_device;
    int err;

    /* 1. Stop the TP from reporting (no-op if already disabled, but also
     *    calls ps2_disable_callback which clears the queue). */
    zmk_mouse_ps2_activity_reporting_disable();

    /* 2. Reach into the PS/2 UART driver's config to get the underlying
     *    UART device + SCL GPIO. We resolve these via the phandle chain
     *    from our own DT node:
     *        mouse_ps2 --ps2-device--> uart_ps2 --parent(bus)--> uart0
     *
     *    NOTE: we do NOT call pinctrl_apply_state() here. That would
     *    require PINCTRL_DT_DEV_CONFIG_GET on uart0, but Zephyr's
     *    PINCTRL_DT_DEFINE() makes that symbol file-static inside
     *    ps2_uart.c, so we can't reference it from a different TU.
     *    Instead we rely on pm_device_action_run(SUSPEND), which the
     *    nRF UARTE driver implements to switch pinctrl to SLEEP state
     *    (and release the HFCLK request) on its own.
     */
#define MOUSE_PS2_NODE       DT_DRV_INST(0)
#define MOUSE_PS2_PS2_NODE   DT_PHANDLE(MOUSE_PS2_NODE, ps2_device)
#define MOUSE_PS2_UART_NODE  DT_PARENT(MOUSE_PS2_PS2_NODE)

#if DT_NODE_EXISTS(MOUSE_PS2_UART_NODE) && DT_NODE_HAS_STATUS(MOUSE_PS2_UART_NODE, okay)
    const struct device *uart_dev = DEVICE_DT_GET(MOUSE_PS2_UART_NODE);
    const struct gpio_dt_spec scl_gpio =
        GPIO_DT_SPEC_GET(MOUSE_PS2_PS2_NODE, scl_gpios);
#else
    const struct device *uart_dev = NULL;
    const struct gpio_dt_spec scl_gpio = { .port = NULL, .pin = 0, .dt_flags = 0 };
#endif

    (void)ps2_dev;

    /* 3. Disable UART RX IRQ and suspend the UART peripheral.
     *    pm_device SUSPEND on nRF UARTE also applies PINCTRL_STATE_SLEEP
     *    to the SDA pin (low-power-enable), so we don't need to do it
     *    manually. */
    if (uart_dev != NULL) {
        uart_irq_rx_disable(uart_dev);
#if IS_ENABLED(CONFIG_PM_DEVICE)
        err = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
        if (err && err != -EALREADY) {
            LOG_WRN("UART suspend returned %d", err);
        }
#endif
    }

    /* 4. Drive SCL low so it cannot back-feed the TP via ESD diodes.
     *    SCL is an ordinary GPIO owned by the ps2_uart driver, not the
     *    UART peripheral, so pm_device doesn't touch it.
     *
     *    NOTE: Use GPIO_OUTPUT_INACTIVE (push-pull drive to GND), NOT
     *    GPIO_INPUT | GPIO_PULL_DOWN. On nRF52 the internal pull-down
     *    is a ~13kΩ resistor; any residual voltage from TP ESD clamps
     *    or PCB parasitics will leak current through it (~30µA measured).
     *    Push-pull output low is a hard connection to GND with no
     *    leakage path. */
    if (scl_gpio.port != NULL) {
        err = gpio_pin_configure_dt(&scl_gpio, GPIO_OUTPUT_INACTIVE);
        if (err) {
            LOG_WRN("SCL low returned %d", err);
        }
    }
}

/*
 * Reverses transport_suspend(): re-applies default pinctrl, resumes the
 * UART peripheral, re-enables RX IRQ. After this the PS/2 driver is
 * ready to talk, but we still need to re-run POR + re-init TP.
 */
static void zmk_mouse_ps2_transport_resume(void) {
    int err;

#if DT_NODE_EXISTS(MOUSE_PS2_UART_NODE) && DT_NODE_HAS_STATUS(MOUSE_PS2_UART_NODE, okay)
    const struct device *uart_dev = DEVICE_DT_GET(MOUSE_PS2_UART_NODE);
    const struct gpio_dt_spec scl_gpio =
        GPIO_DT_SPEC_GET(MOUSE_PS2_PS2_NODE, scl_gpios);
#else
    const struct device *uart_dev = NULL;
    const struct gpio_dt_spec scl_gpio = { .port = NULL, .pin = 0, .dt_flags = 0 };
#endif

    /* SCL as input so the ps2_uart driver can take it over again. */
    if (scl_gpio.port != NULL) {
        err = gpio_pin_configure_dt(&scl_gpio, GPIO_INPUT);
        if (err) {
            LOG_WRN("SCL input returned %d", err);
        }
    }

    /* pm_device RESUME on nRF UARTE also re-applies PINCTRL_STATE_DEFAULT
     * (SDA becomes UART_RX again), so again we don't need a manual
     * pinctrl_apply_state() call here. */
#if IS_ENABLED(CONFIG_PM_DEVICE)
    if (uart_dev != NULL) {
        err = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
        if (err && err != -EALREADY) {
            LOG_WRN("UART resume returned %d", err);
        }
    }
#endif

    if (uart_dev != NULL) {
        uart_irq_rx_enable(uart_dev);
    }
}

#if IS_ENABLED(CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING)

int zmk_mouse_ps2_power_down(void) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    LOG_INF("TrackPoint power-down: entering idle");

    zmk_mouse_ps2_transport_suspend();

    /* Reset parser state so any leftover bytes from before suspend
     * (or the self-test bytes we'll get on resume) do not poison
     * the next packet stream. */
    zmk_mouse_ps2_activity_reset_packet_buffer();
    data->activity_reporting_on = false;

    /* Clear the discard window here, at the one point in the cycle where no
     * packets can arrive. zmk_mouse_ps2_request_wake_up_discard() only raises
     * the counter, so without this a leftover count from the previous cycle
     * would be the floor for the next one. */
    data->wake_up_packets_to_discard = 0;

    /* Finally cut VCC. Do this AFTER the pins are parked low, to
     * avoid the back-powering scenario described above. */
    zmk_mouse_ps2_vcc_set(false);

    return 0;
}

int zmk_mouse_ps2_power_up(void) {
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;

    LOG_INF("TrackPoint power-up: leaving idle");

    /* 1. Restore power. */
    zmk_mouse_ps2_vcc_set(true);
    k_sleep(K_MSEC(CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING_WAKE_DELAY_MS));

    /* 2. Bring the PS/2 transport back online. */
    zmk_mouse_ps2_transport_resume();

    /* 3. Run the 600ms POR sequence (no-op if rst-gpios unset). */
    zmk_mouse_ps2_init_power_on_reset();

    /* 4. Re-detect the device (consumes 0xAA / 0x00 self-test bytes). */
    int err = zmk_mouse_ps2_init_wait_for_mouse(data->dev);
    if (err) {
        LOG_ERR("TrackPoint did not respond after wake; giving up this cycle");
        return -EIO;
    }

    /* 5. Let the TP settle after POR. The Z-force sensor needs a brief
     *    period with no mechanical disturbance to establish a stable
     *    baseline. We do NOT send 0xFF here — a second reset would
     *    re-trigger calibration and can latch a biased baseline if
     *    there's any residual vibration or finger contact.
     *
     *    Reporting is still off at this point, so the TrackPoint's internal
     *    drift correction converges without anything reaching the host. That
     *    makes this the one place where drift can be removed rather than
     *    filtered — but every millisecond here is added wake latency, so the
     *    default stays short and the deadzone does the bulk of the work. See
     *    ZMK_INPUT_MOUSE_PS2_POST_POR_SETTLE_MS. */
    k_sleep(K_MSEC(CONFIG_ZMK_INPUT_MOUSE_PS2_POST_POR_SETTLE_MS));

    /* 6. Hard-reset the parser state and open the drift-suppression window.
     *    ⚠️ CRITICAL: This MUST be done BEFORE releasing any mouse buttons
     *    (step 7), because input_report_key() may trigger internal processing
     *    that reads wake_up_packets_to_discard or packet_buffer.
     *
     *    The discard count is deliberately small now. It exists to swallow
     *    genuine garbage in the first few packets after POR, which is a
     *    short-lived, byte-level problem. Recalibration drift is a much longer
     *    one (~1-2s), and the wide discard window that used to try to cover it
     *    was both too short to actually do so and long enough to eat the first
     *    movement the user intended. The magnitude deadzone opened below is
     *    what handles drift; it lets a deliberate push through immediately
     *    while still holding back the small residual deltas. */
    zmk_mouse_ps2_activity_reset_packet_buffer();
    zmk_mouse_ps2_activity_reset_prev_packet();
    zmk_mouse_ps2_request_wake_up_discard(10);
    zmk_mouse_ps2_start_wake_deadzone();

    /* 7. Release any mouse buttons that may be stuck in the pressed
     *    state. This prevents drift caused by the parser thinking a
     *    button is still held (e.g., from garbage bytes before sleep
     *    or interrupted packet sequences).
     *    ⚠️ This is done AFTER parser reset (step 6) to ensure
     *    wake_up_packets_to_discard is already set. */
    zmk_mouse_ps2_release_all_buttons();

    /* 8. Reapply user configuration that lives in TP RAM. */
    zmk_mouse_ps2_apply_tp_settings_impl(true);

    /* 9. Turn reporting back on. */
    err = zmk_mouse_ps2_activity_reporting_enable();
    if (err) {
        LOG_ERR("Could not re-enable reporting after wake: %d", err);
        return err;
    }

    return 0;
}

#else  /* !CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING */

int zmk_mouse_ps2_power_down(void) { return 0; }
int zmk_mouse_ps2_power_up(void) { return 0; }

#endif /* CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING */

/*
 * Full device reset — callable from any context (key behavior, work queue).
 * Performs a complete VCC power cycle + POR + re-init, equivalent to
 * physically unplugging the keyboard and plugging it back in.
 *
 * Works regardless of whether IDLE_POWER_SAVING is enabled:
 * - If enabled: uses the existing power_down/up infrastructure.
 * - If disabled: does a minimal reset (disable reporting → POR → re-init).
 */
static void zmk_mouse_ps2_reset_device_work_cb(struct k_work *work);
static K_WORK_DEFINE(zmk_mouse_ps2_reset_device_work, zmk_mouse_ps2_reset_device_work_cb);

static void zmk_mouse_ps2_reset_device_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct zmk_mouse_ps2_data *data = &zmk_mouse_ps2_data;
    const struct zmk_mouse_ps2_config *config = &zmk_mouse_ps2_config;

    LOG_INF("TrackPoint full reset triggered by user");

    /* CRITICAL: Release any mouse buttons that may be "stuck" in the
     * pressed state due to garbage bytes being interpreted as button
     * presses during a desync event. If we don't do this, the host
     * computer will remain in a drag/select state even after the TP
     * is reset, because it never received the button release event. */
    zmk_mouse_ps2_release_all_buttons();

    /* Stop reporting via the forcing variant, never the cached one. The device
     * is by definition in an unknown state when the user reaches for this key,
     * which is precisely the situation where the cached-flag path can leave the
     * driver and the device disagreeing about whether reporting is on. */
    zmk_mouse_ps2_reporting_force_resync(false);

    /* Park SCL/SDA before touching VCC. Without this the pins keep the TP alive
     * at a parasitic 1.5-2V through its ESD clamp diodes, so it neither runs nor
     * resets and comes back wedged rather than freshly booted. */
    zmk_mouse_ps2_transport_suspend();

    /* Cut VCC. No-op if the board has no vcc-gpios switch, in which case the
     * 0xFF command below is the only reset mechanism available. */
    zmk_mouse_ps2_vcc_set(false);
    k_sleep(K_MSEC(200)); /* Let TP capacitors discharge */

    /* Restore VCC and bring the transport back up. */
    zmk_mouse_ps2_vcc_set(true);
    k_sleep(K_MSEC(CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING_WAKE_DELAY_MS));
    zmk_mouse_ps2_transport_resume();

    /* Software reset as well: redundant when VCC was actually cycled, but it is
     * the entire reset on boards without a vcc-gpios switch. */
    zmk_mouse_ps2_reset(config->ps2_device);
    k_sleep(K_MSEC(500));

    /* Re-run POR sequence if RST pin is available (rst-gpios in DTS) */
    zmk_mouse_ps2_init_power_on_reset();

    /* Re-detect with a short budget: this is an interactive key press, so it
     * must not hold the PS/2 queue for the ~25s the boot budget can take. */
    int err = zmk_mouse_ps2_init_wait_for_mouse_attempts(data->dev,
                                                         MOUSE_PS2_RESET_INIT_ATTEMPTS);
    if (err) {
        LOG_ERR("TP reset: device did not respond after reset sequence");
        /* Fall through and finish the sequence anyway. Reporting is re-armed
         * unconditionally at the end, so a device that comes back a moment late
         * still works, and the user can simply press the key again. */
    }

    /* Clear parser state. This path ran a POR, so the TrackPoint has
     * recalibrated and needs the same drift handling as the wake path. */
    zmk_mouse_ps2_activity_reset_packet_buffer();
    zmk_mouse_ps2_activity_reset_prev_packet();
    zmk_mouse_ps2_request_wake_up_discard(30);  // Use same wider window as POR
    zmk_mouse_ps2_start_wake_deadzone();

    /* Re-apply user settings (sensitivity, inertia, etc.) */
    zmk_mouse_ps2_apply_tp_settings_impl(true);

    /* Re-arm reporting with the forcing variant. Using the cached-flag version
     * here is what previously made this key a one-way trip into a dead pointer:
     * if the flag was still true from a failed disable, 0xF4 was never sent. */
    zmk_mouse_ps2_reporting_force_resync(true);

    /* The forcing helper does not clear the parser state, so do it here — the
     * 0xF4 ACK and any self-test bytes must not be parsed as movement. */
    zmk_mouse_ps2_activity_reset_packet_buffer();

    LOG_INF("TrackPoint full reset complete");
}

int zmk_mouse_ps2_reset_device(void) {
    /* Runs the full VCC power-cycle + POR (~1s of blocking work). Route it to
     * the dedicated queue so it never stalls keyboard input. */
    zmk_mouse_ps2_submit_work(&zmk_mouse_ps2_reset_device_work);
    return 0;
}

DEVICE_DT_INST_DEFINE(0, &zmk_mouse_ps2_init, NULL, &zmk_mouse_ps2_data, &zmk_mouse_ps2_config,
                      POST_KERNEL, ZMK_MOUSE_PS2_INIT_PRIORITY, NULL);

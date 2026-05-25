// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#include "raw_hid_mouse.h"

#include <stdint.h>
#include <string.h>

#include "action.h"
#include "compiler_support.h"
#include "host.h"
#include "raw_hid.h"
#include "report.h"

// FRAME carries button presses and releases as uint8 bitmasks
// (one bit per button). Lifting MOUSE_BUTTON_COUNT above 8 silently
// overflows those masks on the wire, halves the addressable button
// range on the daemon side, and breaks every shipped mouse_buttonmap
// declaration that assumed 8 columns. If MOUSE_BUTTON_COUNT ever
// needs to grow, the FRAME body layout must widen in lockstep.
STATIC_ASSERT(MOUSE_BUTTON_COUNT == 8, "RAW_HID_MOUSE FRAME wire format pins MOUSE_BUTTON_COUNT at 8.");

#if defined(POINTING_DEVICE_ENABLE) && defined(POINTING_DEVICE_AUTO_MOUSE_ENABLE)
#    include "pointing_device_auto_mouse.h"
#endif

#ifndef RAW_EPSIZE
#    define RAW_EPSIZE 32
#endif

// Upper bound on wheel ticks dispatched through action_exec per axis
// per FRAME. The wire pins wheel to int8, so the natural bound is 127;
// the cap defends against a daemon bug or future protocol widening
// turning a single FRAME into enough tap-hold work to starve matrix
// scanning. Eight notches is well past any realistic free-spin per
// 8 ms frame and still feels like scrolling to the user.
#ifndef RAW_HID_MOUSE_MAX_WHEEL_TICKS_PER_FRAME
#    define RAW_HID_MOUSE_MAX_WHEEL_TICKS_PER_FRAME 8
#endif

// Feed the auto-mouse-layer activation check with a synthetic report
// derived from the raw HID event. Motion and button-down both count
// as activation signals (matching how an internal pointing sensor's
// report would be interpreted). Button-up is NOT fed: the existing
// auto-mouse timeout handles deactivation, and feeding a buttons=0
// report from a release would just race that mechanism. We deliberately
// derive the signal from the raw event, not the keymap-resolved keycode
// -- a click is mouse activity to the auto-mouse layer regardless of
// what mouse_buttonmap dispatches it to.
static void feed_auto_mouse(report_mouse_t report) {
#if defined(POINTING_DEVICE_ENABLE) && defined(POINTING_DEVICE_AUTO_MOUSE_ENABLE)
    pointing_device_task_auto_mouse(report);
#else
    (void)report;
#endif
}

static void handle_button_down(uint8_t btn) {
    if (btn >= MOUSE_BUTTON_COUNT) {
        return;
    }
    action_exec(MAKE_MOUSE_BUTTON_EVENT(btn, true));
    // host_get_mouse_buttons() reflects the last sent report, which
    // does not yet include our new bit (mousekey will set it on its
    // next flush). OR in the bit so the activation check sees the
    // imminent state.
    report_mouse_t r = {0};
    r.buttons        = (uint8_t)(host_get_mouse_buttons() | (1u << btn));
    feed_auto_mouse(r);
}

static void handle_button_up(uint8_t btn) {
    if (btn >= MOUSE_BUTTON_COUNT) {
        return;
    }
    action_exec(MAKE_MOUSE_BUTTON_EVENT(btn, false));
}

static void emit_wheel_ticks(int8_t delta, uint8_t pos_dir, uint8_t neg_dir) {
    if (delta == 0) {
        return;
    }
    uint8_t dir = (delta > 0) ? pos_dir : neg_dir;
    // Promote through int before negating to handle delta == INT8_MIN
    // (whose negation overflows int8_t).
    uint8_t count = (delta < 0) ? (uint8_t)(-(int)delta) : (uint8_t)delta;
    if (count > RAW_HID_MOUSE_MAX_WHEEL_TICKS_PER_FRAME) {
        count = RAW_HID_MOUSE_MAX_WHEEL_TICKS_PER_FRAME;
    }
    for (uint8_t i = 0; i < count; i++) {
        action_exec(MAKE_MOUSE_WHEEL_EVENT(dir, true));
        action_exec(MAKE_MOUSE_WHEEL_EVENT(dir, false));
    }
}

static void handle_motion_rel(int16_t dx, int16_t dy) {
    // Preserve held buttons. A motion report with buttons=0 would
    // silently break click-and-drag -- the host reads full state from
    // every transfer. v/h stay zero; wheel deltas are non-persistent.
    report_mouse_t r = {0};
    r.x              = dx;
    r.y              = dy;
    r.buttons        = host_get_mouse_buttons();
    host_mouse_send(&r);
    feed_auto_mouse(r);
}

static void send_version_reply(void) {
    uint8_t buf[RAW_EPSIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = RAW_HID_MOUSE_PREFIX;
    buf[1] = RAW_HID_MOUSE_SUB_VERSION_REPLY;
    buf[2] = RAW_HID_MOUSE_PROTOCOL_MAJOR;
    buf[3] = RAW_HID_MOUSE_PROTOCOL_MINOR;
    // buf[4..31] is the capabilities bitmap. Bit (subkind / 8) at
    // position (subkind % 8). Mark v1-implemented subkinds.
    static const uint8_t implemented[] = {
        RAW_HID_MOUSE_SUB_GET_VERSION,
        RAW_HID_MOUSE_SUB_PING,
        RAW_HID_MOUSE_SUB_FRAME,
    };
    for (size_t i = 0; i < sizeof(implemented); i++) {
        uint8_t sk = implemented[i];
        buf[4 + (sk >> 3)] |= (uint8_t)(1u << (sk & 0x07));
    }
    raw_hid_send(buf, sizeof(buf));
}

static void send_pong(const uint8_t *ping_payload, uint8_t payload_len) {
    uint8_t buf[RAW_EPSIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = RAW_HID_MOUSE_PREFIX;
    buf[1] = RAW_HID_MOUSE_SUB_PONG;
    if (payload_len > sizeof(buf) - 2) {
        payload_len = sizeof(buf) - 2;
    }
    if (ping_payload && payload_len > 0) {
        memcpy(&buf[2], ping_payload, payload_len);
    }
    raw_hid_send(buf, sizeof(buf));
}

// Handle a FRAME subkind packet. Body layout (8 bytes; the rest of
// the 30-byte payload is reserved-zero):
//
//   [0]    int16 LE rel_x  (low byte)
//   [1]    int16 LE rel_x  (high byte)
//   [2]    int16 LE rel_y  (low byte)
//   [3]    int16 LE rel_y  (high byte)
//   [4]    int8  wheel_vert
//   [5]    int8  wheel_horiz
//   [6]    uint8 buttons_pressed_mask    (bit N = button N pressed in this frame)
//   [7]    uint8 buttons_released_mask
//
// Dispatch order matters: presses first so HOLD_ON_OTHER_KEY_PRESS /
// CHORDAL_HOLD settle pending HRMs before any motion or wheel in the
// same frame; releases last so a press-then-release within one frame
// dispatches in the correct order.
static void handle_frame(const uint8_t *body, uint8_t body_len) {
    if (body_len < 8) {
        return;
    }
    int16_t dx           = (int16_t)((uint16_t)body[0] | ((uint16_t)body[1] << 8));
    int16_t dy           = (int16_t)((uint16_t)body[2] | ((uint16_t)body[3] << 8));
    int8_t  wv           = (int8_t)body[4];
    int8_t  wh           = (int8_t)body[5];
    uint8_t press_mask   = body[6];
    uint8_t release_mask = body[7];

    for (uint8_t btn = 0; btn < MOUSE_BUTTON_COUNT; btn++) {
        if (press_mask & (uint8_t)(1u << btn)) {
            handle_button_down(btn);
        }
    }
    if (dx != 0 || dy != 0) {
        handle_motion_rel(dx, dy);
    }
    emit_wheel_ticks(wv, MOUSE_WHEEL_UP, MOUSE_WHEEL_DOWN);
    emit_wheel_ticks(wh, MOUSE_WHEEL_RIGHT, MOUSE_WHEEL_LEFT);
    for (uint8_t btn = 0; btn < MOUSE_BUTTON_COUNT; btn++) {
        if (release_mask & (uint8_t)(1u << btn)) {
            handle_button_up(btn);
        }
    }
}

bool raw_hid_mouse_handle(uint8_t *data, uint8_t length) {
    if (data == NULL || length < 2 || data[0] != RAW_HID_MOUSE_PREFIX) {
        return false;
    }

    uint8_t        subkind  = data[1];
    const uint8_t *body     = &data[2];
    uint8_t        body_len = (uint8_t)(length - 2);

    switch (subkind) {
        case RAW_HID_MOUSE_SUB_GET_VERSION:
            send_version_reply();
            return true;

        case RAW_HID_MOUSE_SUB_PING:
            send_pong(body, body_len);
            return true;

        case RAW_HID_MOUSE_SUB_FRAME:
            handle_frame(body, body_len);
            return true;

        default:
            // Unknown subkind in our namespace: silently consume so
            // the chain doesn't try further handlers with a packet
            // they don't own either.
            return true;
    }
}

void raw_hid_mouse_init(void) {
    raw_hid_register_handler(raw_hid_mouse_handle);
}

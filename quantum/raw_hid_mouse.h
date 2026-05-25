// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * \file
 *
 * \defgroup raw_hid_mouse RAW_HID_MOUSE feature
 * \{
 *
 * Routes host-originated mouse events that arrive over the Raw HID
 * endpoint into the QMK action pipeline, so they compose with the
 * keymap, tap-hold state, layers, and modifiers exactly like a
 * physical key.
 *
 * Build flag: `RAW_HID_MOUSE_ENABLE`. Off by default. Requires
 * `MOUSE_MAP_ENABLE` (button and wheel events dispatch through
 * `mouse_buttonmap` / `mouse_wheelmap`). Auto-pulls `RAW_ENABLE`.
 *
 * Coexists with VIA on the same Raw HID endpoint by registering
 * through the handler chain in `quantum/raw_hid.c`. Only consumes
 * packets whose first byte equals `RAW_HID_MOUSE_PREFIX` (default
 * `0xFD`); other packets fall through to the next chain handler
 * (VIA, etc.) or to `raw_hid_receive_user`.
 *
 * Composes with `POINTING_DEVICE_AUTO_MOUSE_ENABLE` when both are on:
 * motion and button-down events feed the auto-mouse-layer activation
 * check (`pointing_device_task_auto_mouse`), so an external mouse
 * routed through this feature behaves like an internal pointing
 * sensor for layer-activation purposes. Button-up does not feed
 * (the existing timeout handles deactivation).
 *
 *
 * # Wire protocol
 *
 * All packets are exactly `RAW_EPSIZE` (32 bytes); unused bytes are
 * zero. Multi-byte integers are little-endian.
 *
 *     [0xFD] [subkind] [payload (30 bytes)]
 *
 * Single-prefix-byte design lets one consumer claim the dispatch on
 * one top-level byte (analogous to Vial's `0xFE` pattern) instead of
 * carving a range out of the global namespace.
 *
 *
 * # Subkinds
 *
 *     0x00  GET_VERSION    -- no body. Reply: VERSION_REPLY.
 *     0x01  PING           -- u8[30] echo payload. Reply: PONG.
 *     0x10  FRAME          -- composite event for one evdev frame:
 *                              int16 LE rel_x
 *                              int16 LE rel_y
 *                              int8  wheel_vert
 *                              int8  wheel_horiz
 *                              uint8 buttons_pressed_mask
 *                              uint8 buttons_released_mask
 *                              (8 bytes; remainder reserved-zero)
 *     0x80  VERSION_REPLY  -- u8 major, u8 minor, u8[28] capabilities bitmap.
 *     0x81  PONG           -- u8[30] echoed from PING payload.
 *
 * Subkind ranges are loosely organised: 0x00..0x0F meta (host->kbd),
 * 0x10..0x7F events (host->kbd), 0x80..0xFF replies (kbd->host, by
 * convention `request | 0x80`).
 *
 * FRAME is the only event subkind; one packet per evdev frame keeps
 * USB transactions low and gives the firmware atomic visibility into
 * the user's intent for that frame. Handler dispatch order within a
 * FRAME packet: presses first (so HRM settling via HOLD_ON_OTHER_KEY_PRESS
 * / CHORDAL_HOLD happens before motion or wheel), then motion, then
 * wheel, then releases.
 *
 *
 * # Forward-compat rules
 *
 * - Unknown prefix: silent no-op (chain falls through to other handlers).
 * - Unknown subkind: silent no-op (we own the prefix but skip what we
 *   don't know).
 * - Short message (length below the subkind's documented minimum):
 *   silent no-op.
 * - Subkinds are append-only; meaning of a shipped subkind never changes.
 * - `major` is pinned at 1 forever; only-additive changes.
 * - The `VERSION_REPLY` capabilities bitmap lets a newer daemon detect
 *   which subkinds an older firmware decodes, so it can degrade
 *   gracefully without forcing a re-flash.
 *
 *
 * # Endpoint ordering
 *
 * The "modifier-before-click" ordering that motivates this feature
 * relies on keyboard and mouse reports landing in a single FIFO into
 * one USB IN endpoint. Both `KEYBOARD_SHARED_EP` and `MOUSE_SHARED_EP`
 * must be defined; the build hard-errors below if either is missing.
 * With distinct endpoints, xHCI URB completion order is
 * implementation-defined and the cross-device race reappears one layer
 * lower than libinput.
 *
 * The shared endpoint queue holds `SHARED_IN_CAPACITY` reports
 * (default 4). A burst of more than that many reports queued faster
 * than the host polls (`bInterval` ms) can block `obqWriteTimeout`
 * for up to 100 ms.
 *
 *
 * # Drop-message recovery
 *
 * USB Raw HID is best-effort. Lost packets are accepted as a failure
 * mode: the protocol carries stateless events only (no per-event ack,
 * no firmware-side retransmit). In practice USB drops are vanishingly
 * rare. The host-side daemon is the right place for any retry or
 * watchdog policy if a use case ever needs one.
 *
 *
 * # Transport scope
 *
 * Firmware side is transport-agnostic: uses `raw_hid_send` /
 * `raw_hid_receive`, which dispatch through `host_driver_t.send_raw_hid`
 * and work over USB or Bluetooth. The reference host daemon ships in
 * a companion repo and currently targets Linux hidraw only; daemons
 * for other transports / OSes are out of scope here.
 */

#if !defined(KEYBOARD_SHARED_EP) || !defined(MOUSE_SHARED_EP)
#    error "RAW_HID_MOUSE_ENABLE requires KEYBOARD_SHARED_EP and MOUSE_SHARED_EP so keyboard and mouse reports share a single USB IN endpoint FIFO. See the 'Endpoint ordering' section in raw_hid_mouse.h."
#endif

#ifndef RAW_HID_MOUSE_PREFIX
#    define RAW_HID_MOUSE_PREFIX 0xFD
#endif

#define RAW_HID_MOUSE_PROTOCOL_MAJOR 1
#define RAW_HID_MOUSE_PROTOCOL_MINOR 0

/* Subkind constants. See protocol spec above. */
enum raw_hid_mouse_subkind {
    RAW_HID_MOUSE_SUB_GET_VERSION   = 0x00,
    RAW_HID_MOUSE_SUB_PING          = 0x01,
    RAW_HID_MOUSE_SUB_FRAME         = 0x10,
    RAW_HID_MOUSE_SUB_VERSION_REPLY = 0x80,
    RAW_HID_MOUSE_SUB_PONG          = 0x81,
};

/**
 * \brief Register the RAW_HID_MOUSE handler on the raw_hid chain.
 *
 * Called from `keyboard_init`. The handler only ever consumes packets
 * with the `RAW_HID_MOUSE_PREFIX` byte, so chain registration order
 * relative to other handlers (e.g. VIA) does not affect behaviour.
 */
void raw_hid_mouse_init(void);

/**
 * \brief The registered handler. Exposed so a keyboard providing its
 *        own strong `raw_hid_receive` can call it from there to
 *        integrate the feature without using the chain registry.
 */
bool raw_hid_mouse_handle(uint8_t *data, uint8_t length);

/** \} */

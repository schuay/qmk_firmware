// Copyright 2023 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * \file
 *
 * \defgroup raw_hid Raw HID API
 * \{
 */

/**
 * \brief Type of a chained raw HID receive handler.
 *
 * Handlers return true when they have consumed the packet; the chain
 * stops at the first true return. Returning false lets the next
 * handler in the chain (or `raw_hid_receive_user`) see the packet.
 */
typedef bool (*raw_hid_handler_t)(uint8_t *data, uint8_t length);

#ifndef RAW_HID_MAX_HANDLERS
#    define RAW_HID_MAX_HANDLERS 4
#endif

/**
 * \brief Register a handler in the raw HID receive chain.
 *
 * Handlers are invoked in registration order from the default
 * `raw_hid_receive` implementation. Use this to add a feature-level
 * dispatcher (e.g. RAW_HID_MOUSE) without claiming the
 * `raw_hid_receive` symbol outright.
 *
 * Returns true if the handler was added, false when the registry is
 * full (raise `RAW_HID_MAX_HANDLERS` if you hit this in practice).
 * The registry is not deduplicated: registering the same function
 * twice consumes two slots. Overflow is logged via `dprintf` so the
 * silent failure mode is visible in debug builds.
 *
 * Has no effect when a keyboard provides its own strong
 * `raw_hid_receive`: that override wins linker resolution and the
 * chain dispatcher is never linked in.
 */
bool raw_hid_register_handler(raw_hid_handler_t handler);

/**
 * \brief Drop every registered handler from the chain.
 *
 * Intended for test fixtures that re-run `keyboard_init` per case
 * (where each init re-registers the same handlers and would otherwise
 * accumulate them until the registry overflows). Not for production
 * use; on real firmware, handlers are registered once at init.
 */
void raw_hid_clear_handlers(void);

/**
 * \brief Callback, invoked when a raw HID report has been received from the host.
 *
 * The default implementation walks the chain registered via
 * `raw_hid_register_handler`, then falls through to
 * `raw_hid_receive_user` when no handler claims the packet.
 * Keyboards can still override this strong if they need full control.
 *
 * \param data A pointer to the received data. Always 32 bytes in length.
 * \param length The length of the buffer. Always 32.
 */
void raw_hid_receive(uint8_t *data, uint8_t length);

/**
 * \brief User-overridable fallback for raw HID packets not consumed by
 * the handler chain.
 *
 * Default no-op. Override this in user code instead of overriding
 * `raw_hid_receive` directly when you want to coexist with features
 * that register through the chain (RAW_HID_MOUSE, VIA, ...).
 */
void raw_hid_receive_user(uint8_t *data, uint8_t length);

/**
 * \brief Send an HID report.
 *
 * \param data A pointer to the data to send. Must always be 32 bytes in length.
 * \param length The length of the buffer. Must always be 32.
 */
void raw_hid_send(uint8_t *data, uint8_t length);

/** \} */

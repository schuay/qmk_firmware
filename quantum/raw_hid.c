// Copyright 2025 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stddef.h>

#include "raw_hid.h"
#include "debug.h"
#include "host.h"

static raw_hid_handler_t raw_hid_handlers[RAW_HID_MAX_HANDLERS];
static uint8_t           raw_hid_handler_count = 0;

void raw_hid_send(uint8_t *data, uint8_t length) {
    host_raw_hid_send(data, length);
}

bool raw_hid_register_handler(raw_hid_handler_t handler) {
    if (handler == NULL) {
        return false;
    }
    if (raw_hid_handler_count >= RAW_HID_MAX_HANDLERS) {
        dprintf("raw_hid: handler registry full (RAW_HID_MAX_HANDLERS=%u); raise it if you legitimately need more handlers, otherwise check for accidental double-init\n", (unsigned)RAW_HID_MAX_HANDLERS);
        return false;
    }
    raw_hid_handlers[raw_hid_handler_count++] = handler;
    return true;
}

void raw_hid_clear_handlers(void) {
    raw_hid_handler_count = 0;
}

__attribute__((weak)) void raw_hid_receive_user(uint8_t *data, uint8_t length) {}

__attribute__((weak)) void raw_hid_receive(uint8_t *data, uint8_t length) {
    for (uint8_t i = 0; i < raw_hid_handler_count; i++) {
        if (raw_hid_handlers[i](data, length)) {
            return;
        }
    }
    raw_hid_receive_user(data, length);
}

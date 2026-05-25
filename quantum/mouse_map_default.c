// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

// Identity-passthrough fallback for MOUSE_MAP_ENABLE keyboards that do not
// supply their own mouse_buttonmap / mouse_wheelmap. Dispatch from
// keymap_introspection.c lands in the lookup helpers below; they consult the
// weak `mouse_buttonmap` / `mouse_wheelmap` extern (NULL when the keymap
// supplies no strong definition) and fall back to the default tables
// otherwise.
//
// Living in a translation unit that does NOT include KEYMAP_C is load-
// bearing: it keeps the user's strong definition out of scope here, so the
// compiler cannot statically resolve `&mouse_buttonmap != NULL` (which would
// trip -Werror=address and constant-fold the fallback branch away).

#include <stdint.h>

#include "action_layer.h"
#include "keyboard.h"
#include "keycodes.h"
#include "progmem.h"
#include "report.h"

extern const uint16_t PROGMEM mouse_buttonmap[][MOUSE_BUTTON_COUNT] __attribute__((weak));
extern const uint16_t PROGMEM mouse_wheelmap[][NUM_MOUSE_WHEEL_DIRECTIONS] __attribute__((weak));

static const uint16_t PROGMEM mouse_buttonmap_default_passthrough[MAX_LAYER][MOUSE_BUTTON_COUNT] = {
    [0 ... MAX_LAYER - 1] = DEFAULT_MOUSE_BUTTONMAP,
};

static const uint16_t PROGMEM mouse_wheelmap_default_passthrough[MAX_LAYER][NUM_MOUSE_WHEEL_DIRECTIONS] = {
    [0 ... MAX_LAYER - 1] = DEFAULT_MOUSE_WHEELMAP,
};

uint16_t mouse_buttonmap_lookup_or_default(uint8_t layer_num, uint8_t button_idx) {
    if (&mouse_buttonmap != NULL) {
        return pgm_read_word(&mouse_buttonmap[layer_num][button_idx]);
    }
    return pgm_read_word(&mouse_buttonmap_default_passthrough[layer_num][button_idx]);
}

uint16_t mouse_wheelmap_lookup_or_default(uint8_t layer_num, uint8_t direction) {
    if (&mouse_wheelmap != NULL) {
        return pgm_read_word(&mouse_wheelmap[layer_num][direction]);
    }
    return pgm_read_word(&mouse_wheelmap_default_passthrough[layer_num][direction]);
}

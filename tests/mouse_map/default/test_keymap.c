/* Copyright 2026 QMK
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "quantum.h"

/* Mouse maps for the default test variant. Single layer, matching the
 * single-layer keymaps[] in tests/test_common/keymap.c (the STATIC_ASSERT in
 * keymap_introspection.c enforces parity). Slot assignments exercise the
 * dispatch in several modes:
 *   button 0: passthrough as a standard mouse button
 *   button 1: regular keyboard keycode (verifies cross-domain dispatch)
 *   button 2: modifier-wrapped keycode
 *   button 3: layer-tap (drives tap-hold composition)
 *   buttons 4..7: KC_NO (verifies that empty slots produce no report)
 *
 *   wheel up:    standard wheel-up
 *   wheel down:  regular keycode
 *   wheel left:  modifier-wrapped keycode
 *   wheel right: KC_NO
 */
const uint16_t PROGMEM mouse_buttonmap[][MOUSE_BUTTON_COUNT] = {
    [0] = {MS_BTN1, KC_A, C(KC_X), LT(0, KC_NO), KC_NO, KC_NO, KC_NO, KC_NO},
};

const uint16_t PROGMEM mouse_wheelmap[][NUM_MOUSE_WHEEL_DIRECTIONS] = {
    [0] = {MS_WHLU, KC_PGDN, C(KC_TAB), KC_NO},
};

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

/* Slot assignments mirror tests/mouse_map/default to keep the two
 * suites readable together. Button 0 passes straight through as a
 * mouse button; button 1 cross-dispatches to a keyboard keycode.
 */
const uint16_t PROGMEM mouse_buttonmap[][MOUSE_BUTTON_COUNT] = {
    [0] = {MS_BTN1, KC_A, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO},
};

const uint16_t PROGMEM mouse_wheelmap[][NUM_MOUSE_WHEEL_DIRECTIONS] = {
    [0] = {MS_WHLU, KC_PGDN, KC_NO, KC_NO},
};

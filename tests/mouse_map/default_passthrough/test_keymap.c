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

/* Identity-passthrough variant: every map slot uses the DEFAULT_MOUSE_*MAP
 * helper macros from quantum/keyboard.h. Verifies a user can opt into
 * mouse-button / wheel dispatch with no per-keycode boilerplate.
 *
 * The test_common keymap[] is fixed at 1 layer; the STATIC_ASSERT enforces
 * parity, so both maps are also 1 layer.
 */
const uint16_t PROGMEM mouse_buttonmap[][MOUSE_BUTTON_COUNT] = {
    [0] = DEFAULT_MOUSE_BUTTONMAP,
};

const uint16_t PROGMEM mouse_wheelmap[][NUM_MOUSE_WHEEL_DIRECTIONS] = {
    [0] = DEFAULT_MOUSE_WHEELMAP,
};

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

#include "gtest/gtest.h"
#include "keyboard_report_util.hpp"
#include "mouse_report_util.hpp"
#include "test_common.hpp"

using testing::_;
using testing::InSequence;

class MouseMap : public TestFixture {};

/* Synthesize a press+release through the mouse-button map. Mirrors the
 * shape of TestFixture::tap_key for matrix-position keys. */
static void tap_mouse_button(uint8_t btn_id) {
    action_exec(MAKE_MOUSE_BUTTON_EVENT(btn_id, true));
    action_exec(MAKE_MOUSE_BUTTON_EVENT(btn_id, false));
}

static void tap_mouse_wheel(uint8_t direction) {
    action_exec(MAKE_MOUSE_WHEEL_EVENT(direction, true));
    action_exec(MAKE_MOUSE_WHEEL_EVENT(direction, false));
}

/* Button 0 in the test map -> MS_BTN1. The mouse-keys infrastructure
 * should emit a standard mouse report with bit 0 of the buttons mask
 * set on press, and clear it on release. */
TEST_F(MouseMap, button_passthrough_to_mouse_report) {
    TestDriver driver;
    InSequence s;

    EXPECT_MOUSE_REPORT(driver, (0, 0, 0, 0, 1));
    action_exec(MAKE_MOUSE_BUTTON_EVENT(0, true));
    run_one_scan_loop();

    EXPECT_EMPTY_MOUSE_REPORT(driver);
    action_exec(MAKE_MOUSE_BUTTON_EVENT(0, false));
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Button 1 in the test map -> KC_A. The mouse event must be dispatched
 * through the keymap, producing a keyboard report rather than a mouse
 * report. Demonstrates cross-domain dispatch. */
TEST_F(MouseMap, button_mapped_to_keyboard_key) {
    TestDriver driver;
    InSequence s;

    EXPECT_REPORT(driver, (KC_A));
    EXPECT_EMPTY_REPORT(driver);
    tap_mouse_button(1);
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Button 2 in the test map -> C(KC_X). Verifies that modifier-wrapped
 * keycodes compose with mouse-event dispatch the way they do for normal
 * matrix keys. */
TEST_F(MouseMap, button_mapped_to_modifier_wrapped_key) {
    TestDriver driver;
    InSequence s;

    EXPECT_REPORT(driver, (KC_LCTL));
    EXPECT_REPORT(driver, (KC_LCTL, KC_X));
    EXPECT_REPORT(driver, (KC_LCTL));
    EXPECT_EMPTY_REPORT(driver);
    tap_mouse_button(2);
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Buttons 4..7 in the test map are KC_NO. Pressing any of them must not
 * produce a report on either the keyboard or the mouse interface. */
TEST_F(MouseMap, button_mapped_to_kc_no_emits_nothing) {
    TestDriver driver;
    InSequence s;

    EXPECT_NO_REPORT(driver);
    EXPECT_NO_MOUSE_REPORT(driver);
    tap_mouse_button(4);
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Index beyond MOUSE_BUTTON_COUNT must resolve to KC_NO (an unmapped
 * matrix-style position) and not crash, not emit a report, not corrupt
 * any state. */
TEST_F(MouseMap, button_index_out_of_range_is_safe_no_op) {
    TestDriver driver;
    InSequence s;

    EXPECT_NO_REPORT(driver);
    EXPECT_NO_MOUSE_REPORT(driver);
    tap_mouse_button(MOUSE_BUTTON_COUNT); /* one past the end */
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Wheel up in the test map -> MS_WHLU. Verifies the wheelmap dispatch
 * path is wired through the same keymap_key_to_keycode branch as the
 * button map. */
TEST_F(MouseMap, wheel_up_passthrough) {
    TestDriver driver;
    InSequence s;

    EXPECT_MOUSE_REPORT(driver, (0, 0, 0, 1, 0));
    action_exec(MAKE_MOUSE_WHEEL_EVENT(MOUSE_WHEEL_UP, true));
    run_one_scan_loop();

    EXPECT_EMPTY_MOUSE_REPORT(driver);
    action_exec(MAKE_MOUSE_WHEEL_EVENT(MOUSE_WHEEL_UP, false));
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Wheel down in the test map -> KC_PGDN. Cross-domain dispatch for the
 * wheelmap. */
TEST_F(MouseMap, wheel_down_mapped_to_keyboard_key) {
    TestDriver driver;
    InSequence s;

    EXPECT_REPORT(driver, (KC_PGDN));
    EXPECT_EMPTY_REPORT(driver);
    tap_mouse_wheel(MOUSE_WHEEL_DOWN);
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Wheel right in the test map -> KC_NO. No report expected. */
TEST_F(MouseMap, wheel_mapped_to_kc_no_emits_nothing) {
    TestDriver driver;
    InSequence s;

    EXPECT_NO_REPORT(driver);
    EXPECT_NO_MOUSE_REPORT(driver);
    tap_mouse_wheel(MOUSE_WHEEL_RIGHT);
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Direction beyond NUM_MOUSE_WHEEL_DIRECTIONS must safely resolve to
 * KC_NO. */
TEST_F(MouseMap, wheel_direction_out_of_range_is_safe_no_op) {
    TestDriver driver;
    InSequence s;

    EXPECT_NO_REPORT(driver);
    EXPECT_NO_MOUSE_REPORT(driver);
    tap_mouse_wheel(NUM_MOUSE_WHEEL_DIRECTIONS); /* one past the end */
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

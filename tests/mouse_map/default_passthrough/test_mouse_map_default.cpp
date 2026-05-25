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

class MouseMapDefault : public TestFixture {};

/* Identity passthrough via DEFAULT_MOUSE_BUTTONMAP: button N -> MS_BTN<N+1>,
 * which produces a mouse report with bit N set on press and clear on release.
 * MouseReport args: (x, y, h, v, buttons). */
TEST_F(MouseMapDefault, default_buttonmap_dispatches_each_button_as_its_msbtn) {
    TestDriver driver;
    for (uint8_t btn = 0; btn < MOUSE_BUTTON_COUNT; btn++) {
        InSequence s;
        const uint8_t bit = (uint8_t)(1u << btn);
        EXPECT_MOUSE_REPORT(driver, (0, 0, 0, 0, bit));
        action_exec(MAKE_MOUSE_BUTTON_EVENT(btn, true));
        run_one_scan_loop();

        EXPECT_EMPTY_MOUSE_REPORT(driver);
        action_exec(MAKE_MOUSE_BUTTON_EVENT(btn, false));
        run_one_scan_loop();
        VERIFY_AND_CLEAR(driver);
    }
}

/* Identity passthrough via DEFAULT_MOUSE_WHEELMAP: each direction maps to
 * the matching MS_WHL*, producing a wheel report on the right axis with
 * the right sign. */
TEST_F(MouseMapDefault, default_wheelmap_dispatches_each_direction_as_its_mswhl) {
    static const struct {
        uint8_t direction;
        int8_t  expected_v;
        int8_t  expected_h;
    } cases[] = {
        {MOUSE_WHEEL_UP, 1, 0},
        {MOUSE_WHEEL_DOWN, -1, 0},
        {MOUSE_WHEEL_LEFT, 0, -1},
        {MOUSE_WHEEL_RIGHT, 0, 1},
    };

    for (const auto &c : cases) {
        TestDriver driver;
        InSequence s;
        EXPECT_MOUSE_REPORT(driver, (0, 0, c.expected_h, c.expected_v, 0));
        EXPECT_EMPTY_MOUSE_REPORT(driver);
        action_exec(MAKE_MOUSE_WHEEL_EVENT(c.direction, true));
        action_exec(MAKE_MOUSE_WHEEL_EVENT(c.direction, false));
        run_one_scan_loop();
        VERIFY_AND_CLEAR(driver);
    }
}

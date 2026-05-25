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

#include <cstring>

#include "gtest/gtest.h"
#include "keyboard_report_util.hpp"
#include "mouse_report_util.hpp"
#include "test_common.hpp"

extern "C" {
#include "host.h"
#include "raw_hid.h"
#include "raw_hid_mouse.h"
}

using testing::_;
using testing::InSequence;

/* TestFixture::SetUpTestCase runs keyboard_init() once per fixture,
 * which is when raw_hid_mouse registers its handler. Tests that add
 * their own sentinel handlers (chain_* cases below) would otherwise
 * accumulate slots across cases and eventually fill the 4-slot
 * RAW_HID_MAX_HANDLERS registry. Reset before each case so every test
 * starts with raw_hid_mouse and nothing else. */
class RawHidMouse : public TestFixture {
   protected:
    void SetUp() override {
        raw_hid_clear_handlers();
        raw_hid_mouse_init();
    }
};

namespace {

constexpr uint8_t kFrameSize = 32;

/* Build a frame with our prefix and subkind, payload after byte 2. */
struct Frame {
    uint8_t buf[kFrameSize] = {};

    Frame(uint8_t subkind) {
        buf[0] = RAW_HID_MOUSE_PREFIX;
        buf[1] = subkind;
    }

    Frame &u8(size_t off, uint8_t v) {
        buf[2 + off] = v;
        return *this;
    }

    Frame &le16(size_t off, int16_t v) {
        buf[2 + off]     = static_cast<uint8_t>(v & 0xFF);
        buf[2 + off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        return *this;
    }

    bool handle() {
        return raw_hid_mouse_handle(buf, kFrameSize);
    }
};

} // namespace

/* The handler must not consume frames whose prefix byte isn't ours.
 * Returning false is the contract that lets the registry chain
 * forward the packet to VIA / raw_hid_receive_user. */
TEST_F(RawHidMouse, foreign_prefix_returns_false) {
    uint8_t buf[kFrameSize] = {};
    buf[0]                  = 0x01; /* VIA-ish */
    buf[1]                  = 0x00;
    EXPECT_FALSE(raw_hid_mouse_handle(buf, kFrameSize));
}

/* A length below 2 has no room for a subkind. Falls through; never
 * crashes accessing buf[1]. */
TEST_F(RawHidMouse, short_frame_returns_false) {
    uint8_t buf[1] = {RAW_HID_MOUSE_PREFIX};
    EXPECT_FALSE(raw_hid_mouse_handle(buf, sizeof(buf)));
}

/* Unknown subkind under our prefix: we still own the packet so the
 * chain stops here, but nothing is dispatched. */
TEST_F(RawHidMouse, unknown_subkind_silently_consumed) {
    TestDriver driver;
    InSequence s;
    EXPECT_NO_REPORT(driver);
    EXPECT_NO_MOUSE_REPORT(driver);

    Frame f(0x7F); /* in our prefix's range but undefined */
    EXPECT_TRUE(f.handle());
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* FRAME with a body shorter than the spec'd 8 bytes is a silent no-op
 * (forward-compat rule), but still consumed -- our prefix owns it. */
TEST_F(RawHidMouse, frame_short_body_silently_consumed) {
    TestDriver driver;
    InSequence s;
    EXPECT_NO_MOUSE_REPORT(driver);

    uint8_t buf[7] = {RAW_HID_MOUSE_PREFIX, RAW_HID_MOUSE_SUB_FRAME, 0, 0, 0, 0, 0};
    EXPECT_TRUE(raw_hid_mouse_handle(buf, sizeof(buf)));
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* FRAME with motion only (no buttons, no wheel): emit a mouse report
 * with the decoded delta and buttons=0. Verifies little-endian decode.
 * MouseReport args: (x, y, h, v, buttons). */
TEST_F(RawHidMouse, frame_motion_only_emits_report) {
    TestDriver driver;
    InSequence s;

    EXPECT_MOUSE_REPORT(driver, (5, -3, 0, 0, 0));

    Frame f(RAW_HID_MOUSE_SUB_FRAME);
    f.le16(0, 5).le16(2, -3); /* dx=5, dy=-3; wheel/buttons remain 0 */
    EXPECT_TRUE(f.handle());
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* FRAME with press_mask bit 0: dispatches via mouse_buttonmap[0][0] = MS_BTN1.
 * Produces a standard mouse-button report. */
TEST_F(RawHidMouse, frame_button_press_dispatches_through_mouse_map) {
    TestDriver driver;
    InSequence s;

    EXPECT_MOUSE_REPORT(driver, (0, 0, 0, 0, 1));
    Frame down(RAW_HID_MOUSE_SUB_FRAME);
    down.u8(6, 0x01); /* press bit 0 */
    EXPECT_TRUE(down.handle());
    run_one_scan_loop();

    EXPECT_EMPTY_MOUSE_REPORT(driver);
    Frame up(RAW_HID_MOUSE_SUB_FRAME);
    up.u8(7, 0x01); /* release bit 0 */
    EXPECT_TRUE(up.handle());
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* FRAME with press_mask bit 1: dispatches via mouse_buttonmap[0][1] = KC_A.
 * Must cross domains: keyboard report, not mouse report. */
TEST_F(RawHidMouse, frame_button_press_dispatches_through_keyboard_map) {
    TestDriver driver;
    InSequence s;

    EXPECT_REPORT(driver, (KC_A));
    EXPECT_EMPTY_REPORT(driver);

    Frame down(RAW_HID_MOUSE_SUB_FRAME);
    down.u8(6, 0x02); /* press bit 1 */
    EXPECT_TRUE(down.handle());
    run_one_scan_loop();

    Frame up(RAW_HID_MOUSE_SUB_FRAME);
    up.u8(7, 0x02); /* release bit 1 */
    EXPECT_TRUE(up.handle());
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* FRAME with wheel_vert = +1: dispatches via mouse_wheelmap[0][UP] = MS_WHLU,
 * producing a mouse report with v=1. */
TEST_F(RawHidMouse, frame_wheel_vertical_dispatches_through_wheelmap) {
    TestDriver driver;
    InSequence s;

    EXPECT_MOUSE_REPORT(driver, (0, 0, 0, 1, 0));
    EXPECT_EMPTY_MOUSE_REPORT(driver);

    Frame f(RAW_HID_MOUSE_SUB_FRAME);
    f.u8(4, 0x01); /* +1 vertical wheel */
    EXPECT_TRUE(f.handle());
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* Drag preservation: after pressing a button mapped to MS_BTN1, the
 * next FRAME's motion must stamp the held mask onto the outgoing
 * motion report. Exercises host_get_mouse_buttons() composition. */
TEST_F(RawHidMouse, frame_motion_preserves_held_button_for_drag) {
    TestDriver driver;
    InSequence s;

    /* Press button 0 (MS_BTN1 in the map). One scan to let mousekey
     * flush its report onto the wire, which is what
     * host_get_mouse_buttons() reads back. */
    EXPECT_MOUSE_REPORT(driver, (0, 0, 0, 0, 1));
    Frame down(RAW_HID_MOUSE_SUB_FRAME);
    down.u8(6, 0x01); /* press bit 0 */
    EXPECT_TRUE(down.handle());
    run_one_scan_loop();

    /* Now motion: button bit must come back in the report. */
    EXPECT_MOUSE_REPORT(driver, (7, 0, 0, 0, 1));
    Frame motion(RAW_HID_MOUSE_SUB_FRAME);
    motion.le16(0, 7).le16(2, 0);
    EXPECT_TRUE(motion.handle());
    run_one_scan_loop();

    /* Cleanup: release. */
    EXPECT_EMPTY_MOUSE_REPORT(driver);
    Frame up(RAW_HID_MOUSE_SUB_FRAME);
    up.u8(7, 0x01); /* release bit 0 */
    EXPECT_TRUE(up.handle());
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* FRAME with press_mask + motion in the same packet: presses must
 * dispatch before motion (so MS_BTN1's mousekey state is set), then
 * motion must carry the bit in its report. Single FRAME → single
 * atomic dispatch in the firmware. */
TEST_F(RawHidMouse, frame_press_and_motion_dispatch_in_order) {
    TestDriver driver;
    InSequence s;

    /* Press happens first inside the handler -> mouse report with
     * the button bit. */
    EXPECT_MOUSE_REPORT(driver, (0, 0, 0, 0, 1));
    /* Motion runs next; host_get_mouse_buttons() has been updated by
     * the press path, so the motion report carries the bit. */
    EXPECT_MOUSE_REPORT(driver, (3, 4, 0, 0, 1));

    Frame f(RAW_HID_MOUSE_SUB_FRAME);
    f.le16(0, 3).le16(2, 4).u8(6, 0x01);
    EXPECT_TRUE(f.handle());
    run_one_scan_loop();

    EXPECT_EMPTY_MOUSE_REPORT(driver);
    Frame release(RAW_HID_MOUSE_SUB_FRAME);
    release.u8(7, 0x01);
    EXPECT_TRUE(release.handle());
    run_one_scan_loop();
    VERIFY_AND_CLEAR(driver);
}

/* ---------- raw_hid handler-chain mechanism tests ---------- */

namespace {

/* Sentinel handler used to observe chain dispatch. Module-level state
 * because raw_hid_handler_t is a C function pointer; gmock can't wrap
 * it directly. Reset per-test. */
int                 sentinel_call_count = 0;
uint8_t             sentinel_last_byte0 = 0;
bool                sentinel_return     = false;

extern "C" bool sentinel_handler(uint8_t *data, uint8_t length) {
    sentinel_call_count++;
    if (length > 0) {
        sentinel_last_byte0 = data[0];
    }
    return sentinel_return;
}

void reset_sentinel(bool consume) {
    sentinel_call_count = 0;
    sentinel_last_byte0 = 0;
    sentinel_return     = consume;
}

} // namespace

/* A foreign-prefix packet (not 0xFD) must reach a handler registered
 * after raw_hid_mouse, proving raw_hid_mouse correctly returns false
 * for non-owned prefixes and the chain dispatcher walks to the next
 * handler. */
TEST_F(RawHidMouse, chain_falls_through_to_later_handler_on_unrecognized_prefix) {
    reset_sentinel(true);
    ASSERT_TRUE(raw_hid_register_handler(sentinel_handler));

    uint8_t pkt[32] = {0};
    pkt[0]          = 0x42;
    raw_hid_receive(pkt, sizeof(pkt));

    EXPECT_EQ(sentinel_call_count, 1);
    EXPECT_EQ(sentinel_last_byte0, 0x42);
}

/* Conversely, a 0xFD packet is claimed by raw_hid_mouse before a later
 * sentinel can see it -- first-true-wins. */
TEST_F(RawHidMouse, chain_stops_at_first_handler_that_returns_true) {
    reset_sentinel(true);
    ASSERT_TRUE(raw_hid_register_handler(sentinel_handler));

    /* A FRAME packet with empty body bytes (no buttons / motion / wheel)
     * still belongs to raw_hid_mouse -- handle() returns true, sentinel
     * never sees it. */
    Frame f(RAW_HID_MOUSE_SUB_FRAME);
    EXPECT_TRUE(f.handle());

    /* And via the chain dispatcher directly, with an unknown subkind
     * under our prefix: still consumed (silent no-op). */
    uint8_t pkt[32] = {0};
    pkt[0]          = RAW_HID_MOUSE_PREFIX;
    pkt[1]          = 0x7F; /* unknown subkind in our namespace */
    raw_hid_receive(pkt, sizeof(pkt));

    EXPECT_EQ(sentinel_call_count, 0);
}

/* ---------- reply-path tests (GET_VERSION, PING) ---------- */

/* GET_VERSION triggers a 32-byte VERSION_REPLY via raw_hid_send.
 * Verify the prefix, subkind, major/minor, and at least one expected
 * capability bit. */
TEST_F(RawHidMouse, get_version_replies_with_version_and_capabilities) {
    TestDriver driver;

    EXPECT_CALL(driver, send_raw_hid_mock(testing::_, 32)).WillOnce(testing::Invoke([](uint8_t *data, uint8_t length) {
        ASSERT_EQ(length, 32);
        EXPECT_EQ(data[0], RAW_HID_MOUSE_PREFIX);
        EXPECT_EQ(data[1], RAW_HID_MOUSE_SUB_VERSION_REPLY);
        EXPECT_EQ(data[2], RAW_HID_MOUSE_PROTOCOL_MAJOR);
        EXPECT_EQ(data[3], RAW_HID_MOUSE_PROTOCOL_MINOR);
        /* FRAME (0x10): bit 0 of capabilities byte at index 0x10/8 = 2,
         * within the bitmap which starts at buf[4]. */
        constexpr uint8_t sk      = RAW_HID_MOUSE_SUB_FRAME;
        const uint8_t     cap_off = 4 + (sk >> 3);
        EXPECT_NE(data[cap_off] & (uint8_t)(1u << (sk & 0x07)), 0);
    }));

    Frame f(RAW_HID_MOUSE_SUB_GET_VERSION);
    EXPECT_TRUE(f.handle());
}

/* PING echoes its payload back as PONG. Verify both prefix/subkind
 * and that the payload bytes are reflected. */
TEST_F(RawHidMouse, ping_echoes_payload_as_pong) {
    TestDriver driver;

    EXPECT_CALL(driver, send_raw_hid_mock(testing::_, 32)).WillOnce(testing::Invoke([](uint8_t *data, uint8_t length) {
        ASSERT_EQ(length, 32);
        EXPECT_EQ(data[0], RAW_HID_MOUSE_PREFIX);
        EXPECT_EQ(data[1], RAW_HID_MOUSE_SUB_PONG);
        /* The first three payload bytes we sent below should round-trip. */
        EXPECT_EQ(data[2], 0xDE);
        EXPECT_EQ(data[3], 0xAD);
        EXPECT_EQ(data[4], 0xBE);
    }));

    Frame f(RAW_HID_MOUSE_SUB_PING);
    f.u8(0, 0xDE).u8(1, 0xAD).u8(2, 0xBE);
    EXPECT_TRUE(f.handle());
}

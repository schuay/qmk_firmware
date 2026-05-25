#!/usr/bin/env python3
"""Host-side bridge: forward physical mouse events to a QMK keyboard over Raw HID.

The firmware-side RAW_HID_MOUSE_ENABLE feature decodes a single-prefix-byte
protocol -- every frame starts with 0xFD followed by a 1-byte subkind --
and dispatches buttons and wheel through the keymap-level `mouse_buttonmap`
/ `mouse_wheelmap` (MOUSE_MAP_ENABLE). Motion bypasses the map and goes
through the keyboard's HID mouse interface directly.

Event subkind used by this daemon:
  0x10 FRAME    8-byte composite: int16 LE rel_x, int16 LE rel_y,
                int8 wheel_vert, int8 wheel_horiz, u8 press_mask,
                u8 release_mask. One packet per evdev frame.

Meta subkinds: 0x00 GET_VERSION (-> 0x80 VERSION_REPLY), 0x01 PING
(-> 0x81 PONG). Full spec lives in quantum/raw_hid_mouse.h.

This daemon takes exclusive control (EVIOCGRAB) of every physical mouse on
the system and decodes evdev events one by one. Routing depends on --mode:

  no-motion (default): motion (REL_X/REL_Y) is re-injected on a per-source
    uinput virtual device so the user's compositor pointer-acceleration
    profile keeps applying and motion does not cross the keyboard MCU.
    Wheel and buttons still flow through the firmware FRAME, so
    `mouse_buttonmap` and `mouse_wheelmap` continue to dispatch through
    the keymap. The virtual device's name is "qmk-mouse-bridge: <orig>"
    so the daemon self-excludes it from discovery.

  full: motion, wheel, and buttons all go through the firmware. The
    keyboard's HID mouse interface fully replaces the original device.
    No uinput device is created.

Device discovery is by QMK's universal Raw HID usage tag (page 0xFF60,
usage 0x61) -- works against any QMK keyboard whose firmware implements
the protocol above, no per-board configuration. If multiple QMK keyboards
are present, the daemon refuses to engage until --vid/--pid/--device-path
narrows the choice. Keyboards without a Generic-Desktop / Mouse HID
interface are skipped at connect time (they would silently drop our
messages and brick the user's mouse).

The previous architecture used a uinput mirror plus a fixed daemon-side
wait to keep the modifier ahead of the click in the compositor. That wait
is now firmware-side; see RAW_HID_MOUSE_CLICK_DEFER_MS in raw_hid_mouse.h.

Linux only. Requires `python-evdev`, `hid`, and `pyudev`.
"""
from __future__ import annotations

import argparse
import asyncio
import fnmatch
import logging
import os
import signal
import struct
import sys
from dataclasses import dataclass
from typing import Optional

import evdev
import hid
import pyudev

# QMK universal Raw HID descriptor tag. Defined in
# tmk_core/protocol/usb_descriptor.h; effectively constant across QMK
# builds. Overridable via CLI for the rare custom build that changed
# RAW_USAGE_PAGE / RAW_USAGE in its config.h.
DEFAULT_QMK_RAW_USAGE_PAGE = 0xFF60
DEFAULT_QMK_RAW_USAGE = 0x61

# QMK's RAW_EPSIZE default. Overridable but almost never overridden.
REPORT_SIZE = 32

# Standard HID Usage Tables identifiers for the mouse interface.
# Used to verify a discovered QMK keyboard actually exposes a mouse HID
# endpoint that can dispatch our forwarded events. Without this check, a
# keyboard built with MOUSE_ENABLE=no would silently drop everything we
# send while the daemon held our physical mice grabbed: dead pointer, no
# error signal.
HID_USAGE_PAGE_GENERIC_DESKTOP = 0x01
HID_USAGE_MOUSE = 0x02

# RAW_HID_MOUSE single-prefix-byte protocol. Matches RAW_HID_MOUSE_PREFIX
# and the subkind enum in quantum/raw_hid_mouse.h.
PROTO_PREFIX = 0xFD
SUB_GET_VERSION = 0x00
SUB_FRAME = 0x10
SUB_VERSION_REPLY = 0x80

# How long to wait for a VERSION_REPLY before giving up. Firmware that
# supports the handshake answers in well under a USB polling interval;
# 500ms is generous and bounds shutdown latency if the thread is wedged.
VERSION_HANDSHAKE_TIMEOUT_MS = 500

# Map evdev button codes to the firmware's button-id space. The eight slots
# correspond directly to bits 0..7 of report_mouse_t.buttons -- the eight
# buttons QMK's mouse HID descriptor already exposes. Anything outside this
# set is silently dropped (touch-bar gestures, esoteric extras, etc.).
BUTTON_ID = {
    evdev.ecodes.BTN_LEFT: 0,
    evdev.ecodes.BTN_RIGHT: 1,
    evdev.ecodes.BTN_MIDDLE: 2,
    evdev.ecodes.BTN_SIDE: 3,
    evdev.ecodes.BTN_EXTRA: 4,
    evdev.ecodes.BTN_FORWARD: 5,
    evdev.ecodes.BTN_BACK: 6,
    evdev.ecodes.BTN_TASK: 7,
}

# Virtual-device name prefix used by MotionRepeater. Doubles as the
# self-exclusion key in is_mouselike(): if udev ever surfaces one of
# our own uinput devices, the daemon must not try to grab it.
UINPUT_NAME_PREFIX = "qmk-mouse-bridge: "

MODE_NO_MOTION = "no-motion"
MODE_FULL = "full"
MODES = (MODE_NO_MOTION, MODE_FULL)

# Codes routed to the uinput repeater in no-motion mode. Wheel
# (incl. hi-res) stays on the firmware path so mouse_wheelmap entries
# keep dispatching through the keymap.
_REPEATED_REL_CODES = frozenset({evdev.ecodes.REL_X, evdev.ecodes.REL_Y})

HID_RECONNECT_MIN_S = 0.5
HID_RECONNECT_MAX_S = 8.0

# How often to poll for keyboard presence when no mouse activity is driving
# send() calls. Keeps the daemon responsive when the keyboard appears or
# disappears (USB unplug, bootloader-mode flash, MCU reset) while the user
# isn't touching the mouse.
KEYBOARD_PRESENCE_POLL_S = 1.5

logger = logging.getLogger("qmk-mouse-bridge")


def is_mouselike(dev: evdev.InputDevice) -> bool:
    """Structural check: relative pointer with at least one watched button.

    Excludes touchpads (ABS axes / multitouch; not a HRM+click workflow).
    Excludes the daemon's own uinput repeater devices by name prefix,
    so no-motion mode does not grab the motion-passthrough device it
    just created. Active-keyboard self-exclusion is *not* done here;
    see `Bridge._is_active_keyboard_mouse`. Pure structure so the same
    function works against any QMK board, not just the one we happen
    to be bridged to right now.
    """
    if dev.name.startswith(UINPUT_NAME_PREFIX):
        return False
    caps = dev.capabilities()
    keys = caps.get(evdev.ecodes.EV_KEY, [])
    rels = caps.get(evdev.ecodes.EV_REL, [])
    has_button = any(b in keys for b in BUTTON_ID)
    has_rel_x = evdev.ecodes.REL_X in rels
    return has_button and has_rel_x


_BY_ID_DIR = "/dev/input/by-id"


def device_by_id_names(event_path: str) -> list[str]:
    """Return the `/dev/input/by-id/*` basenames pointing at this event node.

    A single device can have multiple by-id symlinks (e.g. an
    "if02-event-mouse" alias alongside an "event-kbd" alias for the
    same composite). The include/exclude glob compares against each.
    Returns an empty list for devices with no by-id link, in which case
    the include/exclude rules cannot match -- which is the right
    behaviour: stable filtering needs stable names.
    """
    names: list[str] = []
    try:
        entries = os.listdir(_BY_ID_DIR)
    except OSError:
        return names
    target = os.path.realpath(event_path)
    for entry in entries:
        link = os.path.join(_BY_ID_DIR, entry)
        try:
            if os.path.realpath(link) == target:
                names.append(entry)
        except OSError:
            continue
    return names


def device_admitted(by_id_names: list[str],
                    includes: list[str],
                    excludes: list[str]) -> tuple[bool, str]:
    """Apply --include / --exclude glob filters. Returns (admit?, reason).

    Semantics:
      * Excludes always win over includes.
      * If `includes` is non-empty, the device must match at least one
        include to be admitted; if empty, any not-excluded device is
        admitted.
      * Matching is fnmatch.fnmatchcase against each by-id basename.
        A device with no by-id symlink fails any non-empty include
        filter but passes when `includes` is empty.
    """
    for pat in excludes:
        for name in by_id_names:
            if fnmatch.fnmatchcase(name, pat):
                return False, f"excluded by --exclude {pat!r}"
    if not includes:
        return True, ""
    for pat in includes:
        for name in by_id_names:
            if fnmatch.fnmatchcase(name, pat):
                return True, ""
    return False, "no --include pattern matched"


@dataclass(frozen=True)
class DeviceFilter:
    """User-supplied constraints on which QMK keyboard to bridge to.

    All fields optional. Default-constructed filter (everything None) means
    "auto-discover any QMK keyboard." Multiple fields AND together. Mostly
    used to disambiguate when several QMK keyboards are plugged in.
    """

    vid: Optional[int] = None
    pid: Optional[int] = None
    device_path: Optional[bytes] = None
    usage_page: int = DEFAULT_QMK_RAW_USAGE_PAGE
    usage: int = DEFAULT_QMK_RAW_USAGE

    def matches(self, info: dict) -> bool:
        if info.get("usage_page") != self.usage_page or info.get("usage") != self.usage:
            return False
        if self.vid is not None and info.get("vendor_id") != self.vid:
            return False
        if self.pid is not None and info.get("product_id") != self.pid:
            return False
        if self.device_path is not None and info.get("path") != self.device_path:
            return False
        return True


def _describe(info: dict) -> str:
    """Compact human label for a hidapi info dict, for log messages."""
    vid = info.get("vendor_id", 0)
    pid = info.get("product_id", 0)
    product = info.get("product_string") or "?"
    path_b = info.get("path") or b"?"
    path = path_b.decode(errors="replace") if isinstance(path_b, bytes) else str(path_b)
    return f"{vid:04x}:{pid:04x} {product!r} @ {path}"


def _device_has_mouse_interface(vid: int, pid: int) -> bool:
    """True iff (vid, pid) advertises a Generic Desktop / Mouse top-level usage.

    hidapi enumerates one entry per top-level usage, so a composite device
    that exposes both Raw HID and Mouse appears at least twice in the
    enumerate() list. Mouse interfaces use the standard HID Usage Tables
    identifiers regardless of whether the keyboard runs on a shared endpoint.
    """
    return any(
        info.get("usage_page") == HID_USAGE_PAGE_GENERIC_DESKTOP
        and info.get("usage") == HID_USAGE_MOUSE
        for info in hid.enumerate(vid, pid)
    )


class _SelectionWarnings:
    """Per-Bridge dedup state for keyboard selection warnings.

    The presence loop calls select_keyboard() every poll cycle while
    disconnected; without dedup the same ambiguity / no-mouse-interface
    message floods the journal. Kept per-instance so multiple Bridges in
    one process (typically only happens in tests) don't cross-contaminate.
    """

    def __init__(self) -> None:
        self.skipped_idents: set[tuple[int, int]] = set()
        self.last_ambiguity: Optional[frozenset] = None

    def reset_ambiguity(self) -> None:
        self.last_ambiguity = None


def select_keyboard(filter: DeviceFilter, warnings: _SelectionWarnings) -> Optional[dict]:
    """Find the single QMK keyboard we should bridge to, or None.

    Steps:
      1. Enumerate every HID device on the host and keep those whose
         top-level usage matches the QMK Raw HID tag (and the optional
         vid/pid/path filter, if the user supplied one).
      2. Drop candidates that don't advertise a Mouse HID interface --
         they would silently drop our messages and brick the user's mouse.
      3. If exactly one survives, return it. If multiple, log all and
         return None (the user must disambiguate with --vid/--pid/--device-path).
         If zero, return None silently (presence loop will retry).

    Warnings about ambiguity or skipped mouseless-firmware boards are
    de-duplicated via the supplied `_SelectionWarnings` so the journal
    sees one line per change, not one per poll.
    """
    candidates = [info for info in hid.enumerate(0, 0) if filter.matches(info)]
    if not candidates:
        warnings.reset_ambiguity()
        return None

    capable: list[dict] = []
    for c in candidates:
        ident = (c["vendor_id"], c["product_id"])
        if _device_has_mouse_interface(*ident):
            capable.append(c)
            warnings.skipped_idents.discard(ident)
        elif ident not in warnings.skipped_idents:
            warnings.skipped_idents.add(ident)
            logger.warning(
                "skipping %s: QMK Raw HID present but no Mouse HID interface "
                "(rebuild firmware with MOUSE_ENABLE)",
                _describe(c),
            )

    if not capable:
        warnings.reset_ambiguity()
        return None
    if len(capable) > 1:
        signature = frozenset((c["vendor_id"], c["product_id"]) for c in capable)
        if signature != warnings.last_ambiguity:
            warnings.last_ambiguity = signature
            logger.warning(
                "%d QMK keyboards match; refusing to engage. Disambiguate with "
                "--vid/--pid/--device-path. Candidates:",
                len(capable),
            )
            for c in capable:
                logger.warning("  %s", _describe(c))
        return None
    warnings.reset_ambiguity()
    return capable[0]


class HidSender:
    """Writer for the keyboard's Raw HID endpoint, with explicit liveness state.

    Open / close transitions are reported via the `on_state_change` callback
    (`True` -> connected, `False` -> disconnected). The Bridge uses these to
    grab / ungrab physical mice, so mouse passthrough resumes the moment we
    notice the keyboard is gone (cable yank, bootloader-mode flash, MCU reset).

    The active keyboard's (vid, pid) is exposed via `active_vid_pid` while
    connected. The Bridge consults this to avoid grabbing the keyboard's own
    mouse HID interface, which would loop firmware -> host -> daemon -> firmware
    and produce a visible feedback storm.
    """

    def __init__(self, filter: DeviceFilter, on_state_change=None) -> None:
        self._device: Optional[hid.Device] = None
        self._filter = filter
        self._on_state_change = on_state_change or (lambda _connected: None)
        self._selection_warnings = _SelectionWarnings()
        self.active_vid_pid: Optional[tuple[int, int]] = None

    def is_connected(self) -> bool:
        return self._device is not None

    def try_open(self) -> bool:
        """Best-effort open. Returns True on success, False if absent or ambiguous."""
        if self._device is not None:
            return True
        info = select_keyboard(self._filter, self._selection_warnings)
        if info is None:
            return False
        try:
            self._device = hid.Device(path=info["path"])
        except (hid.HIDException, OSError) as exc:
            logger.warning("hid open failed for %s: %s", _describe(info), exc)
            self._device = None
            return False
        self.active_vid_pid = (info["vendor_id"], info["product_id"])
        logger.info("connected to %s", _describe(info))
        self._on_state_change(True)
        return True

    def close(self) -> None:
        """Drop the connection and notify (if we were connected)."""
        if self._device is None:
            return
        try:
            self._device.close()
        except Exception:
            pass
        self._device = None
        self.active_vid_pid = None
        logger.info("keyboard connection closed")
        self._on_state_change(False)

    async def log_version_handshake(self) -> None:
        """Send GET_VERSION, log the firmware's reply at DEBUG.

        Observation-only for now: the daemon doesn't gate features on
        the reported capabilities. Silent (debug-log only) when the
        firmware doesn't answer -- a pre-handshake firmware build is
        a perfectly valid configuration.

        The hidapi read is blocking and is wrapped in asyncio.to_thread
        so it doesn't stall the event loop. Worst case: the thread
        blocks for VERSION_HANDSHAKE_TIMEOUT_MS on shutdown.
        """
        if self._device is None:
            return
        request = bytearray(REPORT_SIZE)
        request[0] = PROTO_PREFIX
        request[1] = SUB_GET_VERSION
        try:
            self._device.write(b"\x00" + bytes(request))
        except (hid.HIDException, OSError) as exc:
            logger.debug("version handshake write failed: %s", exc)
            return
        try:
            reply = await asyncio.to_thread(self._device.read, REPORT_SIZE, VERSION_HANDSHAKE_TIMEOUT_MS)
        except (hid.HIDException, OSError) as exc:
            logger.debug("version handshake read failed: %s", exc)
            return
        if not reply or len(reply) < 4:
            logger.debug("no version reply within %dms (firmware may predate the handshake)", VERSION_HANDSHAKE_TIMEOUT_MS)
            return
        if reply[0] != PROTO_PREFIX or reply[1] != SUB_VERSION_REPLY:
            logger.debug("unexpected reply: byte0=0x%02x byte1=0x%02x", reply[0], reply[1])
            return
        major = reply[2]
        minor = reply[3]
        caps_hex = bytes(reply[4:REPORT_SIZE]).hex()
        logger.debug("firmware protocol v%d.%d capabilities=%s", major, minor, caps_hex)

    def send(self, payload: bytes) -> bool:
        """Write one Raw HID report. Returns True on success, False on failure.

        On failure the connection is closed (firing the state-change callback)
        so the Bridge can ungrab mice immediately rather than waiting for the
        next presence poll. The caller is responsible for not calling send()
        when not connected -- the Bridge enforces this by tearing down proxies
        on disconnect. The disconnected path here is a transient: it covers
        the window between hidsender.close() and the proxy teardown that
        the state callback triggers.
        """
        if self._device is None:
            logger.debug("send() on disconnected HidSender; dropping %d bytes", len(payload))
            return False
        # hidapi expects the leading byte to be the HID report ID.
        # QMK's Raw HID is report ID 0.
        assert len(payload) == REPORT_SIZE
        try:
            self._device.write(b"\x00" + payload)
            return True
        except (hid.HIDException, OSError) as exc:
            logger.warning("hid write failed (%s)", exc)
            self.close()
            return False


def _pad(payload: bytes) -> bytes:
    """Right-pad a Raw HID message to REPORT_SIZE bytes (zeros)."""
    if len(payload) > REPORT_SIZE:
        raise ValueError(f"payload too large: {len(payload)} > {REPORT_SIZE}")
    return payload + b"\x00" * (REPORT_SIZE - len(payload))


def encode_frame(dx: int, dy: int, wheel_vert: int, wheel_horiz: int,
                 press_mask: int, release_mask: int) -> bytes:
    """One FRAME packet for an entire evdev frame.

    Motion is int16 LE per axis; wheel is int8 per axis; press/release
    are uint8 bitmasks (bit N = button N). Empty masks / zero deltas
    are legal -- a partial frame just leaves the other fields zero.
    """
    return _pad(struct.pack(
        "<BBhhbbBB",
        PROTO_PREFIX, SUB_FRAME,
        dx, dy,
        wheel_vert, wheel_horiz,
        press_mask & 0xFF, release_mask & 0xFF,
    ))


def _clip_int16(v: int) -> int:
    if v > 32767:
        return 32767
    if v < -32768:
        return -32768
    return v


def _clip_int8(v: int) -> int:
    if v > 127:
        return 127
    if v < -128:
        return -128
    return v


class MotionRepeater:
    """uinput passthrough for motion events in no-motion mode.

    EVIOCGRAB on a physical mouse hides it from libinput, which would
    freeze the cursor. We re-inject REL_X / REL_Y / SYN_REPORT on a
    per-source uinput virtual device so the compositor still routes
    motion through its normal pointer-device path. Wheel and buttons
    do NOT come through here -- they continue to flow through the
    firmware FRAME so mouse_wheelmap and mouse_buttonmap entries
    keep dispatching through the keymap.

    The virtual device inherits the source's vendor/product so
    libinput's device-quirk database can match it. Caveat: the
    compositor's per-device pointer-speed setting is keyed by device
    identity, so the user re-applies their slider once when the
    virtual device first appears; after that it sticks.
    """

    # BTN_LEFT is declared but never emitted: systemd's udev-builtin-input_id
    # only sets ID_INPUT_MOUSE=1 (and therefore lets libinput treat the
    # device as a pointer) when both REL_X/REL_Y and BTN_LEFT are present.
    # Buttons still go through the firmware FRAME, not through here.
    _CAPS = {
        evdev.ecodes.EV_REL: [
            evdev.ecodes.REL_X,
            evdev.ecodes.REL_Y,
        ],
        evdev.ecodes.EV_KEY: [
            evdev.ecodes.BTN_LEFT,
        ],
    }

    def __init__(self, source: evdev.InputDevice) -> None:
        self._ui = evdev.UInput(
            self._CAPS,
            name=f"{UINPUT_NAME_PREFIX}{source.name}",
            vendor=source.info.vendor,
            product=source.info.product,
            version=source.info.version,
            phys=f"qmk-bridge/{source.path}",
        )

    def write(self, event: evdev.InputEvent) -> None:
        self._ui.write(event.type, event.code, event.value)

    def syn(self) -> None:
        self._ui.syn()

    def close(self) -> None:
        try:
            self._ui.close()
        except Exception:
            pass


class MouseProxy:
    """Grabs one physical mouse and forwards its events as Raw HID messages.

    Buffers a single evdev frame (delimited by SYN_REPORT) so a frame's
    motion + wheel + button events can be coalesced into one Raw HID write
    per axis, rather than one write per evdev event.

    In no-motion mode an attached MotionRepeater takes REL_X/REL_Y
    out of the FRAME; wheel and buttons still ride the firmware path.
    """

    def __init__(self, path: str, hid_sender: HidSender, mode: str) -> None:
        self.path = path
        self.hid = hid_sender
        self.mode = mode
        self._src: Optional[evdev.InputDevice] = None
        self._repeater: Optional[MotionRepeater] = None
        self._task: Optional[asyncio.Task] = None
        # Cumulative button mask the firmware currently believes is held,
        # updated by every successful FRAME flush. Used by SYN_DROPPED
        # recovery to compute the press/release transitions needed to
        # realign firmware state with the kernel's view of physical
        # button state.
        self._last_button_mask: int = 0
        self._reset_frame()

    def _reset_frame(self) -> None:
        self._frame_dx: int = 0
        self._frame_dy: int = 0
        self._frame_vert: int = 0
        self._frame_horiz: int = 0
        # List of (btn_id, pressed) pairs in event order, so we can emit
        # button events with the correct relative ordering.
        self._frame_buttons: list[tuple[int, bool]] = []

    def start(self) -> None:
        self._task = asyncio.create_task(self._run(), name=f"proxy:{self.path}")
        # Surface unhandled exceptions immediately. Default asyncio behavior
        # only logs them when the task is GC'd, but proxies live in
        # Bridge.proxies for the whole session -- so a startup crash here
        # would otherwise stay invisible until shutdown.
        self._task.add_done_callback(self._log_task_exception)

    @staticmethod
    def _log_task_exception(task: asyncio.Task) -> None:
        if task.cancelled():
            return
        exc = task.exception()
        if exc is not None:
            logger.error("proxy task %s crashed: %r", task.get_name(), exc, exc_info=exc)

    async def stop(self) -> None:
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
            self._task = None

    async def _run(self) -> None:
        try:
            self._src = evdev.InputDevice(self.path)
        except (FileNotFoundError, PermissionError, OSError) as exc:
            logger.warning("cannot open %s: %s", self.path, exc)
            return

        try:
            self._src.grab()
        except OSError as exc:
            logger.warning("cannot grab %s (%s): leaving it untouched", self.path, exc)
            self._src.close()
            self._src = None
            return

        if self.mode == MODE_NO_MOTION:
            try:
                self._repeater = MotionRepeater(self._src)
            except (OSError, evdev.UInputError) as exc:
                # /dev/uinput not writable, kernel uinput module missing,
                # or any other UInput init failure. Without the repeater
                # the grabbed device would freeze the cursor -- safer to
                # back off and leave the mouse alone. Catching the evdev
                # exception explicitly is required because UInputError
                # does not derive from OSError.
                logger.warning(
                    "cannot create uinput repeater for %s (%s); ungrabbing. "
                    "Check /dev/uinput permissions or pass --mode full to bypass.",
                    self.path, exc,
                )
                self._teardown()
                return

        logger.info("forwarding %s (%s) [mode=%s]", self.path, self._src.name, self.mode)
        try:
            async for event in self._src.async_read_loop():
                await self._on_event(event)
        except OSError as exc:
            logger.info("input device %s closed: %s", self.path, exc)
        finally:
            self._teardown()

    def _teardown(self) -> None:
        if self._repeater is not None:
            self._repeater.close()
            self._repeater = None
        if self._src is not None:
            try:
                self._src.ungrab()
            except Exception:
                pass
            try:
                self._src.close()
            except Exception:
                pass
            self._src = None

    async def _on_event(self, event: evdev.InputEvent) -> None:
        if event.type == evdev.ecodes.EV_SYN:
            if event.code == evdev.ecodes.SYN_REPORT:
                if self._repeater is not None:
                    self._repeater.syn()
                await self._flush_frame()
            elif event.code == evdev.ecodes.SYN_DROPPED:
                logger.warning("SYN_DROPPED on %s; resyncing", self.path)
                self._reset_frame()
                self._recover_button_state()
            return

        if event.type == evdev.ecodes.EV_REL:
            if self._repeater is not None and event.code in _REPEATED_REL_CODES:
                # no-motion mode: REL_X/REL_Y go to libinput via uinput.
                # Wheel falls through to the firmware FRAME so mouse_wheelmap
                # keeps dispatching through the keymap.
                self._repeater.write(event)
                return
            if event.code == evdev.ecodes.REL_X:
                self._frame_dx += event.value
            elif event.code == evdev.ecodes.REL_Y:
                self._frame_dy += event.value
            elif event.code == evdev.ecodes.REL_WHEEL:
                self._frame_vert += event.value
            elif event.code == evdev.ecodes.REL_HWHEEL:
                self._frame_horiz += event.value
            # REL_WHEEL_HI_RES / REL_HWHEEL_HI_RES: ignored; the discrete
            # REL_WHEEL the kernel emits alongside is what we forward.
            return

        if event.type == evdev.ecodes.EV_KEY:
            btn = BUTTON_ID.get(event.code)
            if btn is None:
                return
            if event.value == 1:
                self._frame_buttons.append((btn, True))
            elif event.value == 0:
                self._frame_buttons.append((btn, False))
            # value == 2 is autorepeat; mouse buttons don't typically emit
            # it, but ignore defensively.
            return

    async def _flush_frame(self) -> None:
        # One FRAME packet per evdev frame. Combines motion + wheel +
        # button transitions into a single 32-byte HID write. A failed
        # send closes the HidSender, which fires the state-change
        # callback and triggers Bridge teardown of all proxies on the
        # next loop iteration -- the user is back on their original
        # mouse before the next evdev frame arrives.
        press_mask = 0
        release_mask = 0
        for btn, pressed in self._frame_buttons:
            bit = 1 << btn
            if pressed:
                press_mask |= bit
            else:
                release_mask |= bit
        if (self._frame_dx | self._frame_dy | self._frame_vert
                | self._frame_horiz | press_mask | release_mask) != 0:
            self.hid.send(encode_frame(
                _clip_int16(self._frame_dx), _clip_int16(self._frame_dy),
                _clip_int8(self._frame_vert), _clip_int8(self._frame_horiz),
                press_mask, release_mask,
            ))
            self._last_button_mask = (self._last_button_mask | press_mask) & ~release_mask
        self._reset_frame()

    def _recover_button_state(self) -> None:
        """Realign the firmware's idea of held buttons with the kernel after SYN_DROPPED.

        SYN_DROPPED means the evdev queue overflowed and some events
        were never delivered. Most are recoverable from the next frame
        (motion is relative; the next SYN_REPORT gives us a fresh
        delta). Button transitions are not: if a BTN_LEFT release was
        in the dropped set, the firmware stays stuck button-down until
        the user clicks again. Query the kernel directly and emit a
        FRAME with the press/release transitions needed to match.

        Reliably reproducible after daemon stalls -- suspend/resume,
        GC pauses, slow journald write -- so the recovery path is on
        the hot path for real users, not a theoretical edge.
        """
        if self._src is None:
            return
        try:
            active = set(self._src.active_keys())
        except OSError as exc:
            logger.warning("active_keys() failed on %s (%s); cannot resync buttons", self.path, exc)
            return
        actual_mask = 0
        for code, idx in BUTTON_ID.items():
            if code in active:
                actual_mask |= 1 << idx
        diff = actual_mask ^ self._last_button_mask
        if diff == 0:
            return
        press_mask   = actual_mask & diff
        release_mask = self._last_button_mask & diff
        self.hid.send(encode_frame(0, 0, 0, 0, press_mask, release_mask))
        self._last_button_mask = actual_mask


class Bridge:
    """Coordinator. Tracks discovered mice and the keyboard's presence state.

    Mice are *known* (in `_mouse_paths`) the moment udev tells us about them,
    but they are only *proxied* (grab + forward) while the keyboard is
    connected. On disconnect -- cable yank, bootloader-mode flash, MCU reset,
    or a write failure -- every proxy is torn down, the kernel releases the
    grabs, and the original mouse devices become visible to the compositor
    again. When the keyboard re-enumerates, proxies are restarted for every
    known mouse path.
    """

    def __init__(self, filter: DeviceFilter, mode: str,
                 includes: list[str], excludes: list[str]) -> None:
        self.mode = mode
        self.includes = includes
        self.excludes = excludes
        self.hid = HidSender(filter, on_state_change=self._on_kb_state_change)
        self.proxies: dict[str, MouseProxy] = {}
        # path -> (vid, pid). Lets us self-exclude the active keyboard's own
        # mouse HID interface at proxy-start time without re-opening the evdev
        # node. (vid, pid) is captured once at discovery; udev events update.
        self._mouse_devices: dict[str, tuple[int, int]] = {}
        self._kb_present = False
        self._kb_state_changed = asyncio.Event()
        self._stopping = asyncio.Event()
        self._udev_monitor: Optional[pyudev.Monitor] = None
        # udev events spawn one-shot async handlers; tracked so shutdown
        # can cancel any in-flight ones rather than leak them past loop close.
        self._udev_tasks: set[asyncio.Task] = set()

    async def run(self) -> None:
        await self._scan_existing_devices()
        self._start_udev_monitor()
        presence_task = asyncio.create_task(self._presence_loop(), name="presence")
        state_task = asyncio.create_task(self._state_loop(), name="state")
        try:
            await self._stopping.wait()
        finally:
            self._stop_udev_monitor()
            presence_task.cancel()
            state_task.cancel()
            for t in self._udev_tasks:
                t.cancel()
            for t in (presence_task, state_task, *self._udev_tasks):
                try:
                    await t
                except (asyncio.CancelledError, Exception):
                    pass
            self._udev_tasks.clear()
            await self._stop_all_proxies()
            self.hid.close()

    def shutdown(self) -> None:
        self._stopping.set()

    def _on_kb_state_change(self, connected: bool) -> None:
        """Called from HidSender on open() / close(). Runs on the event loop
        thread because HidSender is only ever invoked from coroutines or the
        presence loop. Drives the state loop to actually grab/ungrab."""
        self._kb_state_changed.set()

    async def _presence_loop(self) -> None:
        """Detect appearance / disappearance even when no mouse activity drives HidSender.

        Disappearance is normally caught by send() failures (immediate), but a
        keyboard that vanishes while the user isn't moving the mouse needs an
        active poll. Same loop covers reappearance after bootloader-mode flash:
        the user plugs back in / firmware finishes flashing, and we re-grab on
        the next poll without waiting for any other trigger.

        While connected, we re-enumerate just the active (vid, pid) -- much
        cheaper than the full discovery scan, and also gives the right answer
        when one QMK keyboard is replaced by another (the active enumerate
        returns empty, we close, the next iteration calls select_keyboard()
        which finds the new device).
        """
        while not self._stopping.is_set():
            if not self.hid.is_connected():
                if self.hid.try_open():
                    await self.hid.log_version_handshake()
            else:
                vid, pid = self.hid.active_vid_pid
                if not hid.enumerate(vid, pid):
                    self.hid.close()
            try:
                await asyncio.wait_for(self._stopping.wait(), timeout=KEYBOARD_PRESENCE_POLL_S)
            except asyncio.TimeoutError:
                pass

    def _is_active_keyboard_mouse(self, path: str) -> bool:
        """True if `path` is the mouse interface of the currently bridged keyboard.

        Re-evaluated lazily on every proxy-start, so a keyboard swap during a
        session (disconnect A, connect B) ends up grabbing the right set
        without restarting the daemon.
        """
        active = self.hid.active_vid_pid
        if active is None:
            return False
        return self._mouse_devices.get(path) == active

    async def _state_loop(self) -> None:
        """React to HidSender open/close transitions: grab or ungrab mice.

        Decoupled from the callback itself so all grab/ungrab work happens in
        a single async context, not interleaved with whatever was driving the
        transition (presence poll or a failed send from a MouseProxy).
        """
        while not self._stopping.is_set():
            await self._kb_state_changed.wait()
            self._kb_state_changed.clear()
            target_present = self.hid.is_connected()
            if target_present == self._kb_present:
                continue
            self._kb_present = target_present
            if target_present:
                grab = [p for p in sorted(self._mouse_devices) if not self._is_active_keyboard_mouse(p)]
                excluded = len(self._mouse_devices) - len(grab)
                logger.info(
                    "keyboard up; grabbing %d mouse device(s)%s",
                    len(grab),
                    f" (skipped {excluded} self-mouse)" if excluded else "",
                )
                for path in grab:
                    self._add_proxy(path)
            else:
                logger.info("keyboard down; releasing %d mouse device(s) (passthrough)", len(self.proxies))
                await self._stop_all_proxies()

    def _admit(self, path: str) -> bool:
        """Apply --include / --exclude. Logs the reason when a device is
        rejected so users running with --log-level=DEBUG can see why a
        physical mouse wasn't grabbed."""
        admit, reason = device_admitted(device_by_id_names(path), self.includes, self.excludes)
        if not admit:
            logger.debug("skipping %s: %s", path, reason)
        return admit

    async def _scan_existing_devices(self) -> None:
        for path in sorted(evdev.list_devices()):
            try:
                dev = evdev.InputDevice(path)
            except OSError:
                continue
            if is_mouselike(dev) and self._admit(path):
                self._mouse_devices[path] = (dev.info.vendor, dev.info.product)
            dev.close()

    def _add_proxy(self, path: str) -> None:
        if path in self.proxies:
            return
        proxy = MouseProxy(path, self.hid, self.mode)
        self.proxies[path] = proxy
        proxy.start()

    async def _stop_proxy(self, path: str) -> None:
        proxy = self.proxies.pop(path, None)
        if proxy is not None:
            await proxy.stop()

    async def _stop_all_proxies(self) -> None:
        for path in list(self.proxies):
            await self._stop_proxy(path)

    def _start_udev_monitor(self) -> None:
        """Track input device hot-plug via pyudev.

        Register the monitor's netlink fd with the asyncio loop directly,
        rather than running the blocking poll on a worker thread, so that
        SIGTERM-driven shutdown is immediate and doesn't leave a non-daemon
        thread holding the process alive past loop.close().
        """
        context = pyudev.Context()
        monitor = pyudev.Monitor.from_netlink(context)
        monitor.filter_by(subsystem="input")
        monitor.start()
        self._udev_monitor = monitor
        asyncio.get_running_loop().add_reader(monitor.fileno(), self._drain_udev)

    def _stop_udev_monitor(self) -> None:
        if self._udev_monitor is None:
            return
        try:
            asyncio.get_running_loop().remove_reader(self._udev_monitor.fileno())
        except (RuntimeError, ValueError, OSError):
            pass
        self._udev_monitor = None

    def _drain_udev(self) -> None:
        if self._udev_monitor is None:
            return
        while True:
            device = self._udev_monitor.poll(timeout=0)
            if device is None:
                return
            action = device.action
            node = device.device_node
            if node is None or not node.startswith("/dev/input/event"):
                continue
            if action == "add":
                self._spawn_udev_task(self._on_added(node))
            elif action == "remove":
                self._spawn_udev_task(self._on_removed(node))

    def _spawn_udev_task(self, coro) -> None:
        t = asyncio.create_task(coro)
        self._udev_tasks.add(t)
        t.add_done_callback(self._udev_tasks.discard)

    async def _on_added(self, node: str) -> None:
        # Small settle; udev "add" fires before the device is fully readable.
        await asyncio.sleep(0.1)
        try:
            dev = evdev.InputDevice(node)
        except (FileNotFoundError, PermissionError, OSError):
            return
        mouselike = is_mouselike(dev)
        vid_pid = (dev.info.vendor, dev.info.product) if mouselike else None
        dev.close()
        if not mouselike:
            return
        if not self._admit(node):
            return
        self._mouse_devices[node] = vid_pid
        if self._kb_present and not self._is_active_keyboard_mouse(node):
            self._add_proxy(node)

    async def _on_removed(self, node: str) -> None:
        self._mouse_devices.pop(node, None)
        await self._stop_proxy(node)


def _parse_hex_int(value: str) -> int:
    """Accept '0xFAAD', 'FAAD', '64173' -- whatever the user types."""
    s = value.strip()
    try:
        if s.lower().startswith("0x"):
            return int(s, 16)
        # Bare hex (no 0x) is ambiguous; require base-0 to keep decimal too.
        # int("FAAD", 0) raises; fall back to base-16.
        try:
            return int(s, 0)
        except ValueError:
            return int(s, 16)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"not a valid hex/int: {value!r}") from exc


def list_devices(includes: list[str], excludes: list[str]) -> int:
    """Print every evdev device alongside the daemon's admit/reject verdict.

    Stand-in for `--dry-run`: shows is_mouselike + include/exclude as
    seen at startup. Active-keyboard self-exclusion is dynamic and runs
    later (when the keyboard is up), so this view annotates rather than
    pretends to enumerate the final grab set. Returns a shell exit
    code suitable for `sys.exit`.
    """
    rows: list[tuple[str, str, str, str]] = []
    for path in sorted(evdev.list_devices()):
        try:
            dev = evdev.InputDevice(path)
        except OSError as exc:
            rows.append((path, "<unreadable>", "", f"open failed: {exc}"))
            continue
        name = dev.name
        by_ids = device_by_id_names(path)
        dev.close()
        if not is_mouselike(evdev.InputDevice(path)):
            verdict = "skip (not mouselike)"
        else:
            admit, reason = device_admitted(by_ids, includes, excludes)
            verdict = "admit" if admit else f"skip ({reason})"
        by_id_str = ",".join(by_ids) if by_ids else "-"
        rows.append((path, name, by_id_str, verdict))
    width_path = max(len(r[0]) for r in rows) if rows else 4
    width_name = max(len(r[1]) for r in rows) if rows else 4
    width_byid = max(len(r[2]) for r in rows) if rows else 4
    fmt = f"{{:<{width_path}}}  {{:<{width_name}}}  {{:<{width_byid}}}  {{}}"
    print(fmt.format("PATH", "NAME", "BY-ID", "VERDICT"))
    for r in rows:
        print(fmt.format(*r))
    return 0


async def _amain(bridge: Bridge) -> None:
    """Async entrypoint. Installs signal handlers on the running loop and
    awaits Bridge.run(). Kept separate from main() so the asyncio.run()
    boundary is clean -- run() owns loop creation, set, and close."""
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, bridge.shutdown)
    await bridge.run()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="QMK Raw HID mouse bridge. Auto-discovers any QMK keyboard "
                    "by its Raw HID usage tag; use --vid/--pid/--device-path to "
                    "disambiguate when multiple QMK keyboards are present.",
    )
    parser.add_argument("--log-level", default="INFO",
                        choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    parser.add_argument("--vid", type=_parse_hex_int, default=None,
                        help="restrict discovery to this USB vendor ID (e.g. 0xFAAD)")
    parser.add_argument("--pid", type=_parse_hex_int, default=None,
                        help="restrict discovery to this USB product ID (e.g. 0x23B7)")
    parser.add_argument("--device-path", default=None,
                        help="exact hidraw path to bind to (e.g. /dev/hidraw6); "
                             "skips discovery entirely")
    parser.add_argument("--usage-page", type=_parse_hex_int,
                        default=DEFAULT_QMK_RAW_USAGE_PAGE,
                        help="Raw HID usage page (default: QMK's 0xFF60)")
    parser.add_argument("--usage", type=_parse_hex_int,
                        default=DEFAULT_QMK_RAW_USAGE,
                        help="Raw HID usage (default: QMK's 0x61)")
    parser.add_argument(
        "--mode", choices=MODES, default=MODE_NO_MOTION,
        help=(
            f"event routing (default: {MODE_NO_MOTION!s}). "
            f"{MODE_NO_MOTION!s}: motion is re-injected via a uinput virtual "
            f"device so libinput keeps applying its pointer-accel profile; "
            f"wheel and buttons still cross the firmware (mouse_wheelmap and "
            f"mouse_buttonmap dispatch through the keymap). "
            f"{MODE_FULL!s}: motion, wheel, and buttons all cross the firmware."
        ),
    )
    parser.add_argument(
        "--include", action="append", default=[], metavar="GLOB",
        help="fnmatch pattern matched against /dev/input/by-id/* basenames. "
             "If given, only devices whose by-id name matches at least one "
             "pattern are grabbed. May be repeated.",
    )
    parser.add_argument(
        "--exclude", action="append", default=[], metavar="GLOB",
        help="fnmatch pattern matched against /dev/input/by-id/* basenames. "
             "Devices whose by-id name matches any pattern are left alone. "
             "Wins over --include if both match. May be repeated.",
    )
    parser.add_argument(
        "--list-devices", action="store_true",
        help="print every evdev device with the daemon's admit/skip verdict "
             "and exit. Useful for tuning --include / --exclude before running.",
    )
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    if args.list_devices:
        return list_devices(args.include, args.exclude)

    filter = DeviceFilter(
        vid=args.vid,
        pid=args.pid,
        device_path=args.device_path.encode() if args.device_path else None,
        usage_page=args.usage_page,
        usage=args.usage,
    )
    bridge = Bridge(filter, mode=args.mode,
                    includes=args.include, excludes=args.exclude)
    asyncio.run(_amain(bridge))
    return 0


if __name__ == "__main__":
    sys.exit(main())

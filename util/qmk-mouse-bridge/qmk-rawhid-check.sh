#!/bin/sh
# Detect a QMK Raw HID interface by inspecting the kernel-exposed HID
# report descriptor at $1 for QMK's universal usage tag:
#
#   Usage Page (0xFF60)  -> bytes 06 60 ff
#   Usage      (0x61)    -> bytes 09 61
#
# Both items appear at the top of QMK's Raw HID collection in
# tmk_core/protocol/usb_descriptor.c (HID_RI_USAGE_PAGE(16, 0xFF60)
# followed by HID_RI_USAGE(8, 0x61)).
#
# Invoked from the udev IMPORT{program} rule on hidraw devices in
# 99-qmk-mouse-bridge.rules. Prints `QMK_RAW_HID=1` on match so a
# subsequent rule can attach TAG+="uaccess". Prints nothing otherwise
# (the device is left unaffected).
#
# POSIX sh only. Read-only on /sys; no side effects.

set -eu

desc="${1:-}"
[ -n "$desc" ] && [ -r "$desc" ] || exit 0

bytes=$(od -An -v -tx1 "$desc" 2>/dev/null | tr -s ' \n' ' ')
case "$bytes" in
    *"06 60 ff "*"09 61"*) echo "QMK_RAW_HID=1" ;;
esac

#!/usr/bin/env bash
# Install/uninstall the QMK Raw HID mouse bridge.
#
# Per-user parts (no sudo):
#   - Python deps via pip --user
#   - daemon script  -> $HOME/.local/bin/qmk-mouse-bridge
#   - systemd unit   -> $HOME/.config/systemd/user/qmk-mouse-bridge.service
#
# System parts (sudo):
#   - udev rule      -> /etc/udev/rules.d/99-qmk-mouse-bridge.rules
#   - udev helper    -> /usr/local/lib/qmk-mouse-bridge/qmk-rawhid-check.sh
#
# Usage:
#   ./install.sh                       install (default)
#   ./install.sh uninstall             remove everything this script installs
#   ./install.sh --skip-dependencies   install but don't touch Python deps
#                                      (use when deps come from your distro
#                                      packages or a venv you manage yourself)
#
# Aimed at reviewers and first adopters: one-shot, fails loudly, leaves
# a usable system. Production deployments should package this through
# their distro instead.

set -euo pipefail

SKIP_DEPS=0
ARGS=()
for arg in "$@"; do
    case "$arg" in
        --skip-dependencies) SKIP_DEPS=1 ;;
        *) ARGS+=("$arg") ;;
    esac
done
set -- "${ARGS[@]+"${ARGS[@]}"}"

HERE="$(cd "$(dirname "$0")" && pwd)"

# Honor XDG_CONFIG_HOME / XDG_DATA_HOME if set; otherwise fall back to
# the standard defaults. (XDG_DATA_HOME isn't used today but kept for
# the eventual venv-based install layout.)
XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"

DAEMON_DST="$HOME/.local/bin/qmk-mouse-bridge"
SERVICE_DST="$XDG_CONFIG_HOME/systemd/user/qmk-mouse-bridge.service"

# System paths are FHS-standard on Linux distros that this daemon targets
# (udev + systemd + hidraw). Hardcoded rather than configurable -- the
# user installing this should be on a system where these paths apply.
UDEV_RULE_DST="/etc/udev/rules.d/99-qmk-mouse-bridge.rules"
UDEV_HELPER_DIR="/usr/local/lib/qmk-mouse-bridge"
UDEV_HELPER_DST="$UDEV_HELPER_DIR/qmk-rawhid-check.sh"

say() { printf '==> %s\n' "$*"; }

install_deps() {
    if [ "$SKIP_DEPS" = "1" ]; then
        say "Skipping Python dependency install (--skip-dependencies)."
        return
    fi

    # If the modules already import, the user has them satisfied another
    # way (distro packages, venv already on sys.path, etc.). Don't touch.
    if python3 -c "import evdev, hid, pyudev" 2>/dev/null; then
        say "Python dependencies already importable; skipping pip."
        return
    fi

    say "Installing Python dependencies (evdev, hid, pyudev)..."
    # Try pip --user first. On PEP 668-protected distros (Arch, Debian
    # bookworm+, Ubuntu 23.04+, Fedora 39+), this fails with an
    # "externally-managed-environment" error. Those distros want you to
    # use a venv or pipx first; --break-system-packages is the escape
    # hatch, not the recommendation. Don't silently bypass -- print a
    # clear hint and stop, unless the user explicitly opts in.
    if pip install --user -r "$HERE/requirements.txt" 2>/tmp/qmkmb-pip.err; then
        return
    fi
    if grep -q "externally-managed-environment" /tmp/qmkmb-pip.err; then
        if [ "${QMK_BREAK_SYSTEM:-}" = "1" ]; then
            say "Bypassing PEP 668 protection (QMK_BREAK_SYSTEM=1)..."
            pip install --user --break-system-packages -r "$HERE/requirements.txt"
            return
        fi
        cat >&2 <<EOF

Python deps install was blocked by PEP 668 (your distro marks the
system Python environment as externally managed). Pick one:

  * Install via distro packages, then re-run this script (the importable-
    modules probe will detect them and skip pip):
      Arch:   sudo pacman -S python-evdev python-hid python-pyudev
      Debian: sudo apt install python3-evdev python3-hid python3-pyudev

  * Or: install in a venv and run the daemon from there:
      python3 -m venv ~/.venv-qmk-mouse-bridge
      ~/.venv-qmk-mouse-bridge/bin/pip install -r $HERE/requirements.txt
      # then point the systemd unit's ExecStart at the venv's python,
      # and re-run this script with --skip-dependencies.

  * Or: re-run with --skip-dependencies if your deps are already
    available some other way the probe above didn't find.

  * Or: re-run with QMK_BREAK_SYSTEM=1 to install --user with
    --break-system-packages. Same result as pip --user used to give
    on older distros; the protection marker is bypassed.
EOF
        return 1
    fi
    cat /tmp/qmkmb-pip.err >&2
    return 1
}

install_per_user() {
    say "Installing daemon -> $DAEMON_DST"
    install -Dm755 "$HERE/qmk_mouse_bridge.py" "$DAEMON_DST"

    say "Installing systemd user unit -> $SERVICE_DST"
    install -Dm644 "$HERE/qmk-mouse-bridge.service" "$SERVICE_DST"
    systemctl --user daemon-reload
}

install_system() {
    say "Installing udev helper + rule (sudo)..."
    sudo install -Dm755 "$HERE/qmk-rawhid-check.sh" "$UDEV_HELPER_DST"
    sudo install -Dm644 "$HERE/99-qmk-mouse-bridge.rules" "$UDEV_RULE_DST"
    sudo udevadm control --reload
    sudo udevadm trigger --subsystem-match=input --subsystem-match=hidraw
}

post_install_hints() {
    cat <<EOF

Installed. To enable and start the bridge:

    systemctl --user enable --now qmk-mouse-bridge.service
    journalctl --user -u qmk-mouse-bridge.service -f

The udev rule grants per-session access via the 'uaccess' tag, which
works on most desktop sessions (anything managed by logind). If the
daemon logs "Permission denied" on /dev/input/event* or /dev/hidraw*
-- typically on headless, multi-seat, or non-seat0 setups -- give
your user a group with read access to those nodes. The group name
varies by distro; common candidates are 'input' and 'plugdev'. Pick
whichever the device nodes already use:

    ls -l /dev/input/event* /dev/hidraw*
    # then, e.g.:
    sudo usermod -aG <group> \$USER

To run the bridge from this source tree without installing:

    python3 $HERE/qmk_mouse_bridge.py --log-level=DEBUG

EOF
}

do_install() {
    install_deps
    install_per_user
    install_system
    post_install_hints
}

do_uninstall() {
    say "Stopping and disabling user service..."
    systemctl --user disable --now qmk-mouse-bridge.service 2>/dev/null || true

    say "Removing per-user files..."
    rm -f "$DAEMON_DST" "$SERVICE_DST"
    systemctl --user daemon-reload || true

    say "Removing system files (sudo)..."
    sudo rm -f "$UDEV_RULE_DST" "$UDEV_HELPER_DST"
    sudo rmdir "$UDEV_HELPER_DIR" 2>/dev/null || true
    sudo udevadm control --reload 2>/dev/null || true

    cat <<EOF

Uninstalled. Python dependencies (evdev, hid, pyudev) installed via pip
were left in place; remove with:

    pip uninstall -y evdev hid pyudev

EOF
}

case "${1:-install}" in
    install)
        do_install ;;
    uninstall|--uninstall)
        do_uninstall ;;
    -h|--help)
        sed -n '2,17p' "$0" | sed 's/^# \?//' ;;
    *)
        echo "Usage: $0 [install | uninstall]" >&2
        exit 1 ;;
esac

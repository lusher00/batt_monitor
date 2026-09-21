#!/usr/bin/env bash
set -euo pipefail

COMMAND="${1:-install}"

if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG=/etc/default/batt_monitor

install_system() {
    cd "$PROJECT_DIR"

    make -B batt_monitor
    make

    if make -n test >/dev/null 2>&1; then
        make test
    fi

    install -m 0755 batt_monitor /usr/local/bin/batt_monitor

    # Preserve the previous configuration for recovery.
    if [[ -f "$CONFIG" ]]; then
        cp -a "$CONFIG" "${CONFIG}.previous"
    fi

    cat >"$CONFIG" <<'EOF'
# BeagleBone Blue battery monitor configuration
#
# Hardware: DC-jack voltage sense on AIN5.
# Calibrated divider: 11.2702.
#
# The boot check only publishes a reading. It cannot request shutdown.
BATT_CHECK_ARGS="--channel 5 --divider 11.2702 --cells 3"

# Continuous coordinated shutdown policy:
# - sample every 10 seconds
# - normally require 3 consecutive critical readings
# - permit one-reading confirmation only after a sustained two-hour decline
# - give Robot Link/Pi 15 seconds before shutting down the Bone
BATT_MONITOR_ARGS="--watch --shutdown --channel 5 --divider 11.2702 --cells 3 --interval 10 --confirm-samples 3 --trend-hours 2 --trend-drop 0.30 --shutdown-grace 15"
EOF

    cat >/etc/systemd/system/batt_check.service <<'EOF'
[Unit]
Description=Battery voltage boot reading
DefaultDependencies=no
After=local-fs.target
Before=network.target sysinit.target

[Service]
Type=oneshot
EnvironmentFile=/etc/default/batt_monitor
ExecStart=/usr/local/bin/batt_monitor --check $BATT_CHECK_ARGS
RemainAfterExit=no

[Install]
WantedBy=sysinit.target
EOF

    cat >/etc/systemd/system/batt_monitor.service <<'EOF'
[Unit]
Description=Battery voltage monitor and coordinated shutdown policy
After=local-fs.target robot-link-boned.service
Wants=local-fs.target

[Service]
Type=simple
EnvironmentFile=/etc/default/batt_monitor
ExecStart=/usr/local/bin/batt_monitor $BATT_MONITOR_ARGS
Restart=on-failure
RestartSec=10
StandardError=journal

# The timer starts this service after the recovery window.
# Do not enable this service directly.
EOF

    cat >/etc/systemd/system/batt_monitor.timer <<'EOF'
[Unit]
Description=Start battery shutdown monitoring after recovery window

[Timer]
OnBootSec=3min
Unit=batt_monitor.service
AccuracySec=1s

[Install]
WantedBy=timers.target
EOF

    systemctl daemon-reload

    # Avoid bypassing the recovery timer.
    systemctl disable batt_monitor.service 2>/dev/null || true

    systemctl enable batt_check.service
    systemctl enable --now batt_monitor.timer

    # Apply the new binary/config now if monitoring is already active.
    if systemctl is-active --quiet batt_monitor.service; then
        systemctl restart batt_monitor.service
    fi

    echo
    echo "Battery monitoring installed."
    echo "Boot recovery window: 3 minutes"
    echo "Hardware: AIN5, divider 11.2702, 3S"
    echo
    status_system
}

disable_system() {
    systemctl disable --now batt_monitor.timer 2>/dev/null || true
    systemctl stop batt_monitor.service 2>/dev/null || true

    echo "Continuous battery shutdown monitoring disabled."
    echo "The harmless boot voltage check remains enabled."
}

enable_system() {
    systemctl enable --now batt_monitor.timer

    echo "Battery timer enabled."
    echo "It will start the monitor three minutes after the next boot."
    echo "Starting the monitor immediately for this boot."

    systemctl restart batt_monitor.service
}

status_system() {
    echo "--- enablement ---"
    printf "batt_check:         %s\n" \
        "$(systemctl is-enabled batt_check.service 2>/dev/null || true)"
    printf "batt_monitor:       %s\n" \
        "$(systemctl is-enabled batt_monitor.service 2>/dev/null || true)"
    printf "batt_monitor.timer: %s\n" \
        "$(systemctl is-enabled batt_monitor.timer 2>/dev/null || true)"

    echo
    echo "--- activity ---"
    printf "monitor: %s\n" \
        "$(systemctl is-active batt_monitor.service 2>/dev/null || true)"
    printf "timer:   %s\n" \
        "$(systemctl is-active batt_monitor.timer 2>/dev/null || true)"

    echo
    echo "--- configuration ---"
    sed -n '1,120p' "$CONFIG" 2>/dev/null || true

    echo
    echo "--- battery status ---"
    cat /run/batt_status.json 2>/dev/null || echo "No status published yet."
    echo
}

case "$COMMAND" in
    install) install_system ;;
    disable) disable_system ;;
    enable)  enable_system ;;
    status)  status_system ;;
    *)
        echo "Usage: $0 {install|disable|enable|status}" >&2
        exit 2
        ;;
esac

#!/bin/sh
# Deploy the gateway on an Orange Pi Zero 3 running a Debian/Ubuntu/Armbian
# based distribution. Run as root on the target:
#
#   sudo sh deploy/install.sh
#
# The script will build natively on the target when build/opiz3-gateway is
# missing or is not an aarch64 ELF. Alternatively, cross-compile on a host and
# copy the binary before running this script.

set -eu

BIN="opiz3-gateway"
BUILD_BIN="build/$BIN"
APP_PATH="/usr/bin/$BIN"
CONF_PATH="/etc/orangepi/opiz3-gateway.conf"
SERVICE_PATH="/etc/systemd/system/opiz3-gateway.service"

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run as root (sudo sh deploy/install.sh)" >&2
    exit 1
fi

if [ ! -x "$BUILD_BIN" ] || ! file "$BUILD_BIN" | grep -q 'ARM aarch64'; then
    echo "==> Building natively for this board"
    make clean
    make
fi

echo "==> Installing binary"
install -m 0755 "$BUILD_BIN" "$APP_PATH"

echo "==> Installing config (preserving existing)"
if [ -f "$CONF_PATH" ]; then
    echo "    existing config kept at $CONF_PATH"
    install -m 0644 config/opiz3-gateway.conf "$CONF_PATH.new"
else
    install -m 0644 config/opiz3-gateway.conf "$CONF_PATH"
fi

echo "==> Installing systemd service"
install -m 0644 deploy/opiz3-gateway.service "$SERVICE_PATH"

echo "==> Reloading systemd and enabling service"
systemctl daemon-reload
systemctl enable opiz3-gateway
systemctl restart opiz3-gateway

echo "==> Done"
systemctl status --no-pager -l opiz3-gateway || true

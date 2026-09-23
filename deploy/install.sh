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
install -d -m 0755 /etc/orangepi
if [ -f "$CONF_PATH" ]; then
    echo "    existing settings kept at $CONF_PATH"
    chmod 0600 "$CONF_PATH"
    install -m 0600 config/opiz3-gateway.conf "$CONF_PATH.new"

    # Match the gateway's key=value parser: sections are ignored and the last
    # value for a key wins. Only emit a migration decision, never the token.
    migration_state=$(awk '
        BEGIN { bind_ip = "127.0.0.1"; has_token = 0 }
        {
            line = $0
            sub(/^[[:space:]]+/, "", line)
            if (line == "" || substr(line, 1, 1) == "#" ||
                substr(line, 1, 1) == ";" || substr(line, 1, 1) == "[") next
            pos = index(line, "=")
            if (!pos) next
            key = substr(line, 1, pos - 1)
            value = substr(line, pos + 1)
            sub(/[[:space:]]+$/, "", key)
            sub(/[;#].*$/, "", value)
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            if (key == "http_bind_ip") bind_ip = value
            if (key == "http_access_token") has_token = (value != "")
        }
        END {
            if (bind_ip != "127.0.0.1" && !has_token) print "migrate"
            else print "ok"
        }
    ' "$CONF_PATH")

    if [ "$migration_state" = migrate ]; then
        echo "    external bind has no access token; migrating $CONF_PATH"
        # A token must never appear in shell xtrace or installer output.
        set +x
        token=$(od -An -N 24 -tx1 /dev/urandom | tr -d '[:space:]')
        if [ "$(printf %s "$token" | wc -c)" -ne 48 ]; then
            echo "cannot generate a 24-byte access token" >&2
            exit 1
        fi
        case "$token" in
            *[!0-9a-f]*)
                echo "cannot generate a hexadecimal access token" >&2
                exit 1
                ;;
        esac
        printf '\nhttp_access_token = %s\n' "$token" >> "$CONF_PATH"
        unset token
        echo "    access token written to $CONF_PATH (mode 0600); inspect the file to retrieve it"
    fi
else
    install -m 0600 config/opiz3-gateway.conf "$CONF_PATH"
fi

echo "==> Installing systemd service"
install -m 0644 deploy/opiz3-gateway.service "$SERVICE_PATH"

echo "==> Reloading systemd and enabling service"
systemctl daemon-reload
systemctl enable opiz3-gateway
systemctl restart opiz3-gateway

echo "==> Done"
systemctl status --no-pager -l opiz3-gateway || true

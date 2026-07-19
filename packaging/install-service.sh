#!/bin/sh
#
# Installs pixelgo as a user service (systemd on Linux, launchd on macOS), so
# it starts at login and restarts if it crashes.
#
#   ./packaging/install-service.sh            # install and start
#   ./packaging/install-service.sh --uninstall
#
# Deliberately a USER service, not system-wide: pixelgo serves a web UI with no
# authentication on 127.0.0.1, and the agents run with your permissions. There
# is nothing to gain from running it as root, and plenty to lose.

set -e

PORT="${PIXELGO_PORT:-8080}"
BIN_DIR="$HOME/.local/bin"
DATA_DIR="$HOME/.local/share/pixelgo"
CONF_DIR="$HOME/.config/pixelgo"

cd "$(dirname "$0")/.."   # project root

OS="$(uname -s)"

# ---------------------------------------------------------------- uninstall
if [ "$1" = "--uninstall" ]; then
    if [ "$OS" = "Darwin" ]; then
        PLIST="$HOME/Library/LaunchAgents/io.pixelgo.serve.plist"
        launchctl bootout "gui/$(id -u)/io.pixelgo.serve" 2>/dev/null || true
        rm -f "$PLIST"
        echo "Uninstalled. Your data in $DATA_DIR was left untouched."
    else
        systemctl --user disable --now pixelgo.service 2>/dev/null || true
        rm -f "$HOME/.config/systemd/user/pixelgo.service"
        systemctl --user daemon-reload 2>/dev/null || true
        echo "Uninstalled. Your data in $DATA_DIR was left untouched."
    fi
    exit 0
fi

# ---------------------------------------------------------------- checks
if [ ! -x ./pixelgo ]; then
    echo "Cannot find ./pixelgo. Run 'make' first."
    exit 1
fi

# ---------------------------------------------------------------- install
echo "Installing pixelgo as a user service (port $PORT)..."

mkdir -p "$BIN_DIR" "$DATA_DIR" "$CONF_DIR"
install -m 755 ./pixelgo "$BIN_DIR/pixelgo"
echo "  binary -> $BIN_DIR/pixelgo"
echo "  data   -> $DATA_DIR"

# The API keys. pixelgo looks for ~/.config/pixelgo/env on its own; we seed it
# from an existing .env so you do not have to retype the keys.
if [ ! -f "$CONF_DIR/env" ]; then
    if [ -f .env ]; then
        cp .env "$CONF_DIR/env"
        echo "  keys   -> copied .env to $CONF_DIR/env"
    else
        cp .env.example "$CONF_DIR/env"
        echo "  keys   -> created $CONF_DIR/env (EDIT IT: add your API key)"
    fi
fi
chmod 600 "$CONF_DIR/env"

if [ "$OS" = "Darwin" ]; then
    # ------------------------------------------------------------ macOS
    PLIST_DIR="$HOME/Library/LaunchAgents"
    PLIST="$PLIST_DIR/io.pixelgo.serve.plist"
    mkdir -p "$PLIST_DIR"

    sed -e "s|__HOME__|$HOME|g" \
        -e "s|<string>8080</string>|<string>$PORT</string>|" \
        packaging/launchd/io.pixelgo.serve.plist > "$PLIST"

    # bootout first, so re-running this script reloads cleanly
    launchctl bootout "gui/$(id -u)/io.pixelgo.serve" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$PLIST"

    echo ""
    echo "Installed and started."
    echo "  status:  launchctl print gui/$(id -u)/io.pixelgo.serve | head"
    echo "  logs:    tail -f $DATA_DIR/pixelgo.log"
    echo "  stop:    launchctl bootout gui/$(id -u)/io.pixelgo.serve"
else
    # ------------------------------------------------------------ Linux
    if ! command -v systemctl >/dev/null 2>&1; then
        echo "systemctl not found - this script supports systemd and launchd."
        echo "Run it manually instead:  pixelgo serve $PORT --daemon"
        exit 1
    fi

    UNIT_DIR="$HOME/.config/systemd/user"
    mkdir -p "$UNIT_DIR"

    sed -e "s|pixelgo serve 8080|pixelgo serve $PORT|" \
        packaging/systemd/pixelgo.service > "$UNIT_DIR/pixelgo.service"

    systemctl --user daemon-reload
    systemctl --user enable --now pixelgo.service

    # Without lingering, a user service stops when you log out. Most people
    # setting up a service want it running after a reboot too.
    if command -v loginctl >/dev/null 2>&1; then
        if [ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" != "yes" ]; then
            echo ""
            echo "Note: to keep it running after you log out, enable lingering:"
            echo "  sudo loginctl enable-linger $USER"
        fi
    fi

    echo ""
    echo "Installed and started."
    echo "  status:  systemctl --user status pixelgo"
    echo "  logs:    journalctl --user -u pixelgo -f"
    echo "  stop:    systemctl --user stop pixelgo"
fi

echo "  web UI:  http://127.0.0.1:$PORT"

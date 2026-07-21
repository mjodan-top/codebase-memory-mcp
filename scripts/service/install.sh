#!/bin/sh
# Issue #29: user-level service registration for the codebase-memory-mcp
# daemon (socket activation).
#
# Platforms:
#   Darwin           -> launchd LaunchAgent (plist + launchctl bootstrap)
#   Linux + systemd  -> user .socket/.service pair (systemctl --user)
#   anything else    -> ENOTSUP: explicit error, exit 1 (never fake success)
#
# The service manager binds the UDS and starts the daemon on the FIRST
# client connection; the daemon adopts the listener fd (launchd:
# launch_activate_socket, systemd: sd_listen_fds contract) — same seams the
# live E2Es exercise (tests/e2e/test_launchd_activation.sh,
# tests/e2e/test_systemd_activation.sh).
#
# User-level only: never sudo, never touches system domains.
set -eu

PROG="[cbm-install]"

usage() {
    cat >&2 <<'EOF'
Usage: install.sh --bin <path> --socket <path> [options]

Required:
  --bin <path>      daemon binary (must exist and be executable)
  --socket <path>   UDS pathname the service manager will bind

Options:
  --label <name>    service label / unit name
                    (default: dev.codebase-memory.daemon)
  --prefix <dir>    directory for the plist / unit files instead of
                    ~/Library/LaunchAgents or ~/.config/systemd/user
                    (test rigging; keeps the real dirs untouched)
  --log-dir <dir>   daemon stdout/stderr log directory (launchd only;
                    default: ~/Library/Logs/codebase-memory-mcp)
  --no-load         write the service files only; skip
                    launchctl bootstrap / systemctl enable --now
EOF
    exit 2
}

die() {
    printf '%s ERROR: %s\n' "$PROG" "$1" >&2
    exit 1
}
note() { printf '%s %s\n' "$PROG" "$1"; }

BIN=""
SOCK=""
LABEL="dev.codebase-memory.daemon"
PREFIX=""
LOG_DIR=""
NO_LOAD=0

while [ $# -gt 0 ]; do
    case "$1" in
    --bin)
        [ $# -ge 2 ] || usage
        BIN=$2
        shift 2
        ;;
    --socket)
        [ $# -ge 2 ] || usage
        SOCK=$2
        shift 2
        ;;
    --label)
        [ $# -ge 2 ] || usage
        LABEL=$2
        shift 2
        ;;
    --prefix)
        [ $# -ge 2 ] || usage
        PREFIX=$2
        shift 2
        ;;
    --log-dir)
        [ $# -ge 2 ] || usage
        LOG_DIR=$2
        shift 2
        ;;
    --no-load)
        NO_LOAD=1
        shift
        ;;
    -h | --help)
        usage
        ;;
    *)
        printf '%s ERROR: unknown argument: %s\n' "$PROG" "$1" >&2
        usage
        ;;
    esac
done

[ -n "$BIN" ] || die "--bin is required"
[ -n "$SOCK" ] || die "--socket is required"
[ -e "$BIN" ] || die "--bin '$BIN' does not exist"
[ -x "$BIN" ] || die "--bin '$BIN' is not executable"
# Absolute paths: the service manager runs without our cwd.
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
case "$SOCK" in
/*) ;;
*) die "--socket must be an absolute path (got '$SOCK')" ;;
esac

OS=$(uname -s)

install_launchd() {
    AGENT_DIR=${PREFIX:-$HOME/Library/LaunchAgents}
    LOG_DIR=${LOG_DIR:-$HOME/Library/Logs/codebase-memory-mcp}
    PLIST="$AGENT_DIR/$LABEL.plist"
    UID_NUM=$(id -u)

    # Refuse half-overwrites: same label must be fully uninstalled first.
    [ ! -e "$PLIST" ] ||
        die "'$PLIST' already exists — run uninstall.sh first (no half-overwrite)"
    if launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; then
        die "launchd already knows label '$LABEL' — run uninstall.sh first"
    fi

    mkdir -p "$AGENT_DIR" "$LOG_DIR"

    cat >"$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>daemon</string>
        <string>--launchd</string>
        <string>--socket</string>
        <string>$SOCK</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>HOME</key><string>$HOME</string>
    </dict>
    <key>Sockets</key>
    <dict>
        <key>Listeners</key>
        <dict>
            <key>SockPathName</key><string>$SOCK</string>
            <key>SockPathMode</key><integer>384</integer>
        </dict>
    </dict>
    <key>StandardOutPath</key><string>$LOG_DIR/daemon.out</string>
    <key>StandardErrorPath</key><string>$LOG_DIR/daemon.err</string>
</dict>
</plist>
EOF
    note "wrote $PLIST"

    if [ "$NO_LOAD" -eq 1 ]; then
        note "--no-load: skipping launchctl bootstrap"
    else
        launchctl bootstrap "gui/$UID_NUM" "$PLIST" ||
            die "launchctl bootstrap gui/$UID_NUM $PLIST failed"
        note "bootstrapped gui/$UID_NUM/$LABEL (socket-activated; daemon starts on first connection)"
    fi
    note "installed: label=$LABEL socket=$SOCK bin=$BIN"
}

install_systemd() {
    UNIT_DIR=${PREFIX:-${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user}
    SOCKET_UNIT="$UNIT_DIR/$LABEL.socket"
    SERVICE_UNIT="$UNIT_DIR/$LABEL.service"

    [ ! -e "$SOCKET_UNIT" ] && [ ! -e "$SERVICE_UNIT" ] ||
        die "unit files for '$LABEL' already exist in $UNIT_DIR — run uninstall.sh first (no half-overwrite)"

    mkdir -p "$UNIT_DIR"

    cat >"$SOCKET_UNIT" <<EOF
[Unit]
Description=codebase-memory-mcp daemon socket ($LABEL)

[Socket]
ListenStream=$SOCK
SocketMode=0600

[Install]
WantedBy=sockets.target
EOF

    cat >"$SERVICE_UNIT" <<EOF
[Unit]
Description=codebase-memory-mcp daemon ($LABEL)
Requires=$LABEL.socket

[Service]
Type=simple
Environment=HOME=$HOME
ExecStart=$BIN daemon --systemd --socket $SOCK
EOF
    note "wrote $SOCKET_UNIT"
    note "wrote $SERVICE_UNIT"

    if [ "$NO_LOAD" -eq 1 ]; then
        note "--no-load: skipping systemctl --user enable --now"
    else
        [ -z "$PREFIX" ] ||
            die "--prefix with load is unsupported on systemd (systemd only reads its own unit dirs); pass --no-load"
        systemctl --user daemon-reload ||
            die "systemctl --user daemon-reload failed"
        systemctl --user enable --now "$LABEL.socket" ||
            die "systemctl --user enable --now $LABEL.socket failed"
        note "enabled $LABEL.socket (socket-activated; daemon starts on first connection)"
    fi
    note "installed: label=$LABEL socket=$SOCK bin=$BIN"
}

case "$OS" in
Darwin)
    install_launchd
    ;;
Linux)
    command -v systemctl >/dev/null 2>&1 ||
        die "ENOTSUP: Linux without systemctl — user service install not supported here"
    systemctl --user show-environment >/dev/null 2>&1 || [ "$NO_LOAD" -eq 1 ] ||
        die "ENOTSUP: user systemd instance unavailable (systemctl --user unreachable)"
    install_systemd
    ;;
*)
    die "ENOTSUP: no supported user service manager on '$OS' (launchd/systemd only)"
    ;;
esac

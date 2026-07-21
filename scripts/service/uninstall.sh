#!/bin/sh
# Issue #29: user-level service DE-registration for the codebase-memory-mcp
# daemon. Counterpart of install.sh.
#
# Behaviour:
#   - Darwin: launchctl bootout gui/UID/<label> + remove plist.
#   - Linux:  systemctl --user disable --now <label>.socket (+ stop service)
#             + remove unit files + daemon-reload.
#   - If the socket file still exists AND belongs to this service (it is a
#     socket at the path recorded in the service definition), remove it.
#   - Not installed at all: idempotent success (exit 0) but prints
#     "not-installed" so callers can tell.
#   - Anything else: ENOTSUP, exit 1.
#
# User-level only: never sudo, never touches system domains.
set -eu

PROG="[cbm-uninstall]"

usage() {
    cat >&2 <<'EOF'
Usage: uninstall.sh [options]

Options:
  --label <name>    service label / unit name
                    (default: dev.codebase-memory.daemon)
  --prefix <dir>    directory holding the plist / unit files instead of
                    ~/Library/LaunchAgents or ~/.config/systemd/user
                    (must match the --prefix given to install.sh)
  --no-load         only remove files; skip launchctl bootout /
                    systemctl --user disable
EOF
    exit 2
}

die() {
    printf '%s ERROR: %s\n' "$PROG" "$1" >&2
    exit 1
}
note() { printf '%s %s\n' "$PROG" "$1"; }

LABEL="dev.codebase-memory.daemon"
PREFIX=""
NO_LOAD=0

while [ $# -gt 0 ]; do
    case "$1" in
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

# Extract the --socket path recorded in a service definition file so we can
# clean up a leftover socket file that belongs to THIS service only.
socket_path_from_definition() {
    # Works for both the plist (<string>/path</string> right after the
    # --socket argument string) and the unit file (ListenStream=/path).
    deffile=$1
    if grep -q 'ListenStream=' "$deffile" 2>/dev/null; then
        sed -n 's/^ListenStream=//p' "$deffile" | head -1
    else
        # plist: the <string> element following the --socket argument.
        awk '/<string>--socket<\/string>/ { getline; gsub(/.*<string>|<\/string>.*/, ""); print; exit }' \
            "$deffile"
    fi
}

remove_socket_file() {
    sock=$1
    [ -n "$sock" ] || return 0
    if [ -S "$sock" ]; then
        rm -f "$sock"
        note "removed leftover socket $sock"
    elif [ -e "$sock" ]; then
        # Path exists but is not a socket — do not touch foreign files.
        note "leaving $sock alone (exists but is not a socket; not ours to delete)"
    fi
}

uninstall_launchd() {
    AGENT_DIR=${PREFIX:-$HOME/Library/LaunchAgents}
    PLIST="$AGENT_DIR/$LABEL.plist"
    UID_NUM=$(id -u)

    found=0
    if launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; then
        found=1
        if [ "$NO_LOAD" -eq 1 ]; then
            note "--no-load: skipping launchctl bootout for loaded label $LABEL"
        else
            launchctl bootout "gui/$UID_NUM/$LABEL" ||
                die "launchctl bootout gui/$UID_NUM/$LABEL failed"
            # bootout is asynchronous while the job is running: wait
            # (bounded) until launchd actually forgot the label so callers
            # can rely on "uninstalled" meaning gone.
            i=0
            while launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; do
                i=$((i + 1))
                [ "$i" -lt 100 ] ||
                    die "launchd still knows '$LABEL' 5s after bootout"
                sleep 0.05
            done
            note "booted out gui/$UID_NUM/$LABEL"
        fi
    fi
    if [ -e "$PLIST" ]; then
        found=1
        SOCK=$(socket_path_from_definition "$PLIST")
        rm -f "$PLIST"
        note "removed $PLIST"
        remove_socket_file "$SOCK"
    fi
    if [ "$found" -eq 0 ]; then
        note "not-installed: label '$LABEL' unknown to launchd and no plist at $PLIST"
    else
        note "uninstalled: label=$LABEL"
    fi
}

uninstall_systemd() {
    UNIT_DIR=${PREFIX:-${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user}
    SOCKET_UNIT="$UNIT_DIR/$LABEL.socket"
    SERVICE_UNIT="$UNIT_DIR/$LABEL.service"

    found=0
    if [ "$NO_LOAD" -eq 0 ] && command -v systemctl >/dev/null 2>&1 &&
        systemctl --user show-environment >/dev/null 2>&1; then
        if systemctl --user list-unit-files "$LABEL.socket" 2>/dev/null |
            grep -q "$LABEL.socket"; then
            found=1
            systemctl --user disable --now "$LABEL.socket" 2>/dev/null ||
                note "disable --now $LABEL.socket reported an error (continuing cleanup)"
            systemctl --user stop "$LABEL.service" 2>/dev/null || true
            systemctl --user reset-failed "$LABEL.service" 2>/dev/null || true
            note "disabled $LABEL.socket"
        fi
    fi
    if [ -e "$SOCKET_UNIT" ] || [ -e "$SERVICE_UNIT" ]; then
        found=1
        SOCK=$(socket_path_from_definition "$SOCKET_UNIT" 2>/dev/null || true)
        rm -f "$SOCKET_UNIT" "$SERVICE_UNIT"
        note "removed $SOCKET_UNIT $SERVICE_UNIT"
        if [ "$NO_LOAD" -eq 0 ] && command -v systemctl >/dev/null 2>&1; then
            systemctl --user daemon-reload 2>/dev/null || true
        fi
        remove_socket_file "$SOCK"
    fi
    if [ "$found" -eq 0 ]; then
        note "not-installed: no units for '$LABEL' in $UNIT_DIR"
    else
        note "uninstalled: label=$LABEL"
    fi
}

case "$(uname -s)" in
Darwin)
    uninstall_launchd
    ;;
Linux)
    uninstall_systemd
    ;;
*)
    die "ENOTSUP: no supported user service manager on '$(uname -s)' (launchd/systemd only)"
    ;;
esac

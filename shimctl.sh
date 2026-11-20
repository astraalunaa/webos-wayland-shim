#!/bin/sh
# Start/stop/status the webOS Wayland shim.
#
# Environment:
#   XDG_RUNTIME_DIR        runtime dir used by the TV compositor (default /tmp/xdg)
#   HOST_WAYLAND_DISPLAY   TV compositor socket name (default wayland-0)
#   SHIM_WAYLAND_DISPLAY   socket exposed to apps (default wayland-shim)
#   SHIM_LOG               shim log file (default /tmp/shim.log)

set -u

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${XDG_RUNTIME_DIR:=/tmp/xdg}"
: "${HOST_WAYLAND_DISPLAY:=wayland-0}"
: "${SHIM_WAYLAND_DISPLAY:=wayland-shim}"
: "${SHIM_LOG:=/tmp/shim.log}"

SOCK="$XDG_RUNTIME_DIR/$SHIM_WAYLAND_DISPLAY"
LOCK="$SOCK.lock"
PIDFILE="$XDG_RUNTIME_DIR/webos-wayland-shim.pid"

pids() {
    ps -eo pid,comm 2>/dev/null | awk '$2 == "shim" { print $1 }'
}

running() {
    [ -n "$(pids)" ]
}

usage() {
    cat <<EOF
usage: $(basename "$0") {start|stop|restart|status|log}

  start     start the shim if it is not already running
  stop      stop all shim processes and remove stale sockets
  restart   stop + start
  status    show process, socket and log information
  log       show the last 50 lines of $SHIM_LOG

Environment overrides:
  XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR
  HOST_WAYLAND_DISPLAY=$HOST_WAYLAND_DISPLAY
  SHIM_WAYLAND_DISPLAY=$SHIM_WAYLAND_DISPLAY
EOF
}

start() {
    mkdir -p "$XDG_RUNTIME_DIR"
    if running; then
        echo "shim already running: pid $(pids | tr '\n' ' ')"
        return 0
    fi

    # A killed shim leaves its socket and abstract-socket lock behind.
    rm -f "$SOCK" "$LOCK"

    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    WAYLAND_DISPLAY="$HOST_WAYLAND_DISPLAY" \
        setsid "$DIR/shim" >"$SHIM_LOG" 2>&1 </dev/null &

    i=0
    while [ "$i" -lt 10 ]; do
        [ -S "$SOCK" ] && break
        sleep 1
        i=$((i + 1))
    done

    if running; then
        pids > "$PIDFILE"
        echo "shim started: pid $(pids | tr '\n' ' ')"
        echo "  socket: $SOCK"
        echo "  log:    $SHIM_LOG"
    else
        echo "shim failed to start; last log lines:" >&2
        tail -20 "$SHIM_LOG" >&2 2>/dev/null
        return 1
    fi
}

stop() {
    if ! running; then
        rm -f "$SOCK" "$LOCK" "$PIDFILE" "$PIDFILE.tmp"
        echo "shim is not running"
        return 0
    fi

    p=$(pids | tr '\n' ' ')
    echo "stopping shim: pid $p"
    kill $(pids) 2>/dev/null

    i=0
    while [ "$i" -lt 5 ] && running; do
        sleep 1
        i=$((i + 1))
    done
    if running; then
        kill -9 $(pids) 2>/dev/null
    fi

    rm -f "$SOCK" "$LOCK" "$PIDFILE"
    echo "shim stopped"
}

status() {
    echo "XDG_RUNTIME_DIR:     $XDG_RUNTIME_DIR"
    echo "host display:        $HOST_WAYLAND_DISPLAY"
    echo "shim display:        $SHIM_WAYLAND_DISPLAY"
    if running; then
        echo "process:             running (pid $(pids | tr '\n' ' '))"
    else
        echo "process:             not running"
    fi
    if [ -S "$SOCK" ]; then
        echo "socket:              present ($SOCK)"
    else
        echo "socket:              missing ($SOCK)"
    fi
    echo "log:                 $SHIM_LOG"
}

case "${1:-}" in
    start)   start ;;
    stop)    stop ;;
    restart) stop; start ;;
    status)  status ;;
    log)     tail -50 "$SHIM_LOG" ;;
    *)       usage; exit 2 ;;
esac

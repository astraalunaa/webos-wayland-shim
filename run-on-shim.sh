#!/bin/sh
# Run any graphical program on the shim.
#
#   ./run-on-shim.sh gimp
#   ./run-on-shim.sh libreoffice --writer
#   ./run-on-shim.sh supertuxkart --screensize=1280x720
#
# The shim is started automatically if needed.  Environment defaults are
# chosen for Wayland + software GL, which is what the TV chroot can provide.
# Override any of them before invoking this script if necessary.

set -u

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "$#" -eq 0 ]; then
    echo "usage: $(basename "$0") [--] PROGRAM [ARGS...]" >&2
    exit 2
fi
[ "$1" = "--" ] && shift
if [ "$#" -eq 0 ]; then
    echo "usage: $(basename "$0") [--] PROGRAM [ARGS...]" >&2
    exit 2
fi

: "${XDG_RUNTIME_DIR:=/tmp/xdg}"
: "${SHIM_WAYLAND_DISPLAY:=wayland-shim}"
: "${SHIM_HOME:=/root}"

export XDG_RUNTIME_DIR
export WAYLAND_DISPLAY="$SHIM_WAYLAND_DISPLAY"
export HOME="$SHIM_HOME"

# These are ignored by toolkits that don't use them.
: "${GDK_BACKEND:=wayland}"
: "${QT_QPA_PLATFORM:=wayland}"
: "${SDL_VIDEODRIVER:=wayland}"
: "${LIBGL_ALWAYS_SOFTWARE:=1}"
: "${SAL_USE_VCLPLUGIN:=gtk3}"
export GDK_BACKEND QT_QPA_PLATFORM SDL_VIDEODRIVER LIBGL_ALWAYS_SOFTWARE SAL_USE_VCLPLUGIN

"$DIR/shimctl.sh" start >/dev/null || exit 1
exec "$@"

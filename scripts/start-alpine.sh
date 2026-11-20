#!/bin/sh
# Run this on the TV (outside the chroot) as root.
#
# Mounts an ext4 USB rootfs and the bind mounts a chroot needs, fixes DNS,
# then enters the Alpine chroot.
#
#   USB_DEV=/dev/sda1 ./scripts/start-alpine.sh
#
# Environment:
#   ALPINE_ROOT            default /tmp/alpine
#   USB_DEV                default /dev/sda1
#   HOST_XDG_RUNTIME_DIR   default /tmp/xdg
#   WAYLAND_DISPLAY        default wayland-0

set -eu

ROOT=${ALPINE_ROOT:-/tmp/alpine}
USB_DEV=${USB_DEV:-/dev/sda1}
HOST_XDG=${HOST_XDG_RUNTIME_DIR:-/tmp/xdg}
HOST_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}

mounted() {
    awk -v m="$1" '$2 == m { found = 1 } END { exit found ? 0 : 1 }' /proc/mounts
}

mkdir -p "$ROOT" "$HOST_XDG"

if ! mounted "$ROOT"; then
    echo "mounting $USB_DEV -> $ROOT"
    mount -t ext4 "$USB_DEV" "$ROOT"
fi

mkdir -p "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/tmp/xdg"

mounted "$ROOT/dev"      || mount --bind /dev "$ROOT/dev"
mounted "$ROOT/proc"     || mount -t proc proc "$ROOT/proc"
mounted "$ROOT/sys"      || mount -t sysfs sysfs "$ROOT/sys"
mounted "$ROOT/tmp/xdg"  || mount --bind "$HOST_XDG" "$ROOT/tmp/xdg"

# Some programs (LibreOffice, games) need shared memory.  If /dev/shm was a
# separate mount on the host, bind it separately because --bind is not rbind.
if [ -d /dev/shm ]; then
    mkdir -p "$ROOT/dev/shm"
    mounted "$ROOT/dev/shm" || mount --bind /dev/shm "$ROOT/dev/shm" || true
fi

# The host resolv.conf generally points at 127.0.0.1/::1, which does not work
# after chrooting.  Replace it only if it is empty or loopback-only.
if [ ! -s "$ROOT/etc/resolv.conf" ] || \
   grep -qE '^nameserver[[:space:]]+(127\.0\.0\.1|::1)' "$ROOT/etc/resolv.conf"; then
    [ -f "$ROOT/etc/resolv.conf" ] && [ ! -f "$ROOT/etc/resolv.conf.bak" ] && \
        cp "$ROOT/etc/resolv.conf" "$ROOT/etc/resolv.conf.bak"
    printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$ROOT/etc/resolv.conf"
fi

export XDG_RUNTIME_DIR=/tmp/xdg
export WAYLAND_DISPLAY="$HOST_DISPLAY"

echo "Alpine root: $ROOT"
echo "Host Wayland: $HOST_DISPLAY (inside chroot: XDG_RUNTIME_DIR=/tmp/xdg)"
exec chroot "$ROOT" /bin/sh

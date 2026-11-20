#!/bin/sh
# Unmount the chroot mounts and the USB rootfs. Run on the TV as root.
#
#   ./scripts/umount-alpine.sh
#
# Set ALPINE_ROOT if you did not use /tmp/alpine.

set -u

ROOT=${ALPINE_ROOT:-/tmp/alpine}

mounted() {
    awk -v m="$1" '$2 == m { found = 1 } END { exit found ? 0 : 1 }' /proc/mounts
}

for m in "$ROOT/tmp/xdg" "$ROOT/dev/shm" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT"; do
    if mounted "$m"; then
        echo "unmounting $m"
        umount "$m" || umount -l "$m" || echo "could not unmount $m" >&2
    fi
done

echo "done"

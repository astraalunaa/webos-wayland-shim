# webos-wayland-shim

A small nested Wayland compositor that lets modern Linux GUI applications run
natively on LG webOS TVs.

> **AI-assisted project:** This repository and its documentation were created
> with the help of an AI coding assistant (DeepSeek V4.1 Flash via OpenCode)
> and were tested on the author's rooted TV. Read the code and the rooting
> notes before running anything on your own hardware.

webOS's surface manager exposes `wl_shell` and `wl_webos_shell`, but **not**
`xdg_wm_base`. Current GTK, Qt, SDL and Electron applications expect
`xdg_wm_base`, so without a translation layer they cannot create a window at
all. This shim:

- connects to the TV compositor as one fullscreen `wl_shell` surface,
- identifies itself with a persistent webOS `appId`,
- exposes `xdg_wm_base`, `wl_shm`, `wl_seat`, `wl_output` and a minimal
  `wl_data_device_manager` to applications,
- composites their windows into a single fullscreen buffer, and
- forwards keyboard and pointer input from the TV's per-device seats.

It does not take over DRM/KMS and does not touch the TV's display pipeline.
The TV compositor remains in control the whole time.

```
┌──────────────────────────────────────────────┐
│ webOS surface manager (LSM)                  │
│   wl_shell + wl_webos_shell (appId)          │
└──────────────────▲───────────────────────────┘
                   │ fullscreen wl_shell surface
┌──────────────────┴───────────────────────────┐
│ webos-wayland-shim                           │
│   xdg_wm_base / wl_shm / wl_seat / wl_output │
│   pixman software compositing                │
└──────────────────▲───────────────────────────┘
                   │ xdg-shell
        ┌──────────┴──────────┐
        │ GIMP / LibreOffice  │
        │ SDL games / etc.    │
        └─────────────────────┘
```

## Tested environment

- LG webOS TV, Realtek RTD2875, webOS build `20250913080627`
- Alpine Linux 3.24.4, `armv7l`, inside an ext4 USB chroot
- GIMP 3.2.4 (with the GIMP-specific workaround in
  [`docs/APP-NOTES.md`](docs/APP-NOTES.md))
- LibreOffice 25.8.7.3
- SuperTuxKart 1.5 (software GL; memory pressure can still kill it)

The TV has about 1 GB of RAM. The shim itself is tiny, but applications and
software rendering are not. Close other TV apps when running something heavy.

## Requirements

- A rooted TV with root SSH access. See [step 0](#0-root-the-tv-and-enable-ssh).
- An ext4-formatted USB drive with enough free space.
- An Alpine `minirootfs` for the TV's architecture. Most rooted LG TVs of this
  generation are `armv7l`; newer ones may be `aarch64`. Check with `uname -m`.
- The TV compositor's Wayland socket. On the tested TV it is
  `/tmp/xdg/wayland-0`.

## 0. Root the TV and enable SSH

Rooting methods depend on the exact model and firmware. Do not update the TV
first: a firmware update can remove the exploit. The short version:

1. **Find the model number.** It is on the label on the back of the TV.
2. **Check support.** Enter the model number at
   <https://cani.rootmy.tv>. That site tells you whether the model is
   supported and which rooting method/tool applies.
3. **Check the firmware version.** On the TV, open
   **Settings → All Settings → Support → Software Update** and note the
   software version. Compare it with the versions supported by the rooting
   tool. If your version is not listed as supported, stop here; do not attempt
   the root.
4. **Disable automatic updates.** In the same update settings, turn off
   **Auto Update** (tick it off). A later firmware update can break the root
   and the `/tmp/xdg` Wayland setup this project relies on.
5. **Root the TV.** Follow the instructions provided by the rooting tool for
   your exact model and firmware. Reboot the TV when the tool tells you to.
6. **Enable SSH.** Open the **Homebrew Channel** app, find its settings, and
   enable **SSH**. Reboot the TV.
7. **Connect and check the architecture.**

   ```sh
   ssh root@<TV_IP>
   # default password on many webOS root setups:
   # alpine
   uname -m
   ```

   Typical results:

   - `armv7l` → use Alpine's `armv7` minirootfs
   - `aarch64` → use Alpine's `aarch64` minirootfs

   Change the default root password with `passwd` if you have not already.

## 1. Put Alpine on the USB drive

Use the architecture from `uname -m` in step 0. On a Linux workstation:

1. Open <https://alpinelinux.org/downloads/>, scroll down to
   **Mini Root Filesystem**, and download the build for your architecture.
   For example, `armv7l` uses the `armv7` minirootfs and `aarch64` uses the
   `aarch64` one.
2. Format the USB partition as **ext4** with your favourite partition manager
   (GParted, `fdisk` + `mkfs.ext4`, etc.).
3. Mount the partition and extract the minirootfs as root. Preserve
   permissions and ownership with `-p`.

```sh
# Example: adjust ARCH and the version to the download you picked.
ARCH=armv7              # uname -m showed armv7l -> use armv7
VER=3.24.4
wget "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/$ARCH/alpine-minirootfs-$VER-$ARCH.tar.gz"

# Destructive: this erases the partition.
sudo mkfs.ext4 -L alpine /dev/sdX1
sudo mount /dev/sdX1 /mnt

# Extract as root. Do not drop the -p flag.
sudo tar -xpf "alpine-minirootfs-$VER-$ARCH.tar.gz" -C /mnt

# Put this repository inside the chroot so it is available after boot.
sudo cp -a webos-wayland-shim /mnt/root/

# If you use a separate TV-side post-mount script (for example one that
# exports XDG_RUNTIME_DIR and WAYLAND_DISPLAY), copy it here too, or merge
# those two exports into scripts/start-alpine.sh.

sync
sudo umount /mnt
```

If the USB drive already contains an Alpine rootfs, you can extract/copy over
it instead of formatting. Device nodes, setuid bits and ownership matter, so
always extract as root with `-p`.

Unplug the USB from the workstation and plug it into the TV. **If the TV asks
to format the USB, do not format it** — that would erase Alpine.

## 2. Mount it and enter the chroot on the TV

The repository is already inside the USB rootfs at
`/root/webos-wayland-shim`. Copy only the host-side start script to the TV,
because the script has to run **outside** the chroot and before the USB is
mounted at `/tmp/alpine`.

On the workstation:

```sh
scp webos-wayland-shim/scripts/start-alpine.sh root@<TV_IP>:/home/root/
```

Then, over SSH on the TV:

```sh
chmod +x /home/root/start-alpine.sh
/home/root/start-alpine.sh
```

The helper mounts the USB partition at `/tmp/alpine`, mounts `/dev`, `/proc`,
`/sys`, the host runtime directory, repairs loopback-only DNS, and enters the
chroot. If the USB device is not `/dev/sda1`:

```sh
USB_DEV=/dev/sdb1 /home/root/start-alpine.sh
```

If your TV auto-mounted the USB somewhere else and the helper cannot mount
`/tmp/alpine`, unmount the auto-mount first or set `ALPINE_ROOT`/`USB_DEV`
explicitly:

```sh
USB_DEV=/dev/sdb1 ALPINE_ROOT=/tmp/alpine /home/root/start-alpine.sh
```

What the script does, if you prefer to do it manually:

```sh
ROOT=/tmp/alpine
mount -t ext4 /dev/sda1 "$ROOT"
mount --bind /dev "$ROOT/dev"
mount -t proc proc "$ROOT/proc"
mount -t sysfs sysfs "$ROOT/sys"

# The TV compositor socket lives on the host at /tmp/xdg.
mkdir -p /tmp/xdg "$ROOT/tmp/xdg"
mount --bind /tmp/xdg "$ROOT/tmp/xdg"

# Shared memory for LibreOffice/games, if the host has it.
mount --bind /dev/shm "$ROOT/dev/shm"

# The host resolv.conf usually points at 127.0.0.1/::1 and will not work
# inside the chroot.
printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$ROOT/etc/resolv.conf"

export XDG_RUNTIME_DIR=/tmp/xdg
export WAYLAND_DISPLAY=wayland-0
chroot "$ROOT" /bin/sh
```

Inside the chroot, `/tmp/xdg/wayland-0` must exist and be connectable. If your
TV uses a different runtime directory or socket name, export
`HOST_XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` accordingly.

To leave, exit the shell and run:

```sh
./scripts/umount-alpine.sh
```

## 3. Build the shim

Inside Alpine:

```sh
cd /root/webos-wayland-shim
./scripts/install-build-deps.sh
make
```

Optional test applications:

```sh
apk add gimp libreoffice supertuxkart
```

See [`docs/APP-NOTES.md`](docs/APP-NOTES.md) for GIMP's bubblewrap workaround
and notes on LibreOffice and SuperTuxKart.

The build needs `wayland-scanner` and `wayland-protocols` to generate the
xdg-shell server bindings and the webOS shell client bindings. Both are
installed by the script.

Before building, it is useful to confirm what the TV actually exposes:

```sh
wayland-info | grep -E "wl_shell|wl_webos_shell|xdg_wm_base"
```

Expected result: `wl_shell` and `wl_webos_shell` are present, `xdg_wm_base` is
not. That is exactly the gap this shim fills.

## 4. Run applications

Start the shim once:

```sh
./shimctl.sh start
```

Then launch **any** graphical program through the generic launcher:

```sh
./run-on-shim.sh gimp
./run-on-shim.sh libreoffice --writer
./run-on-shim.sh supertuxkart --screensize=1280x720
./run-on-shim.sh xterm
```

`run-on-shim.sh`:

- starts the shim if it is not already running,
- sets `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY=wayland-shim` and a safe `HOME`,
- selects Wayland for GTK, Qt and SDL,
- enables software GL (`LIBGL_ALWAYS_SOFTWARE=1`) because the TV GPU is not
  available inside the chroot,
- selects LibreOffice's GTK3 VCL plugin,
- then `exec`s the program with all remaining arguments.

No per-application launch scripts are needed. Override environment variables
before the command if a program needs something different:

```sh
LIBGL_ALWAYS_SOFTWARE=0 ./run-on-shim.sh some-app
```

Useful control commands:

```sh
./shimctl.sh status
./shimctl.sh log
./shimctl.sh restart
./shimctl.sh stop
```

Set `SHIM_DEBUG=1` (or pass `--debug` to the shim) for verbose input and popup
logging:

```sh
SHIM_DEBUG=1 ./shimctl.sh restart
```

## 5. How it works

### Persistent window

The shim creates a `wl_shell_surface`, sets it fullscreen, and calls
`wl_webos_shell_surface.set_property("appId", ...)` plus
`_WEBOS_WINDOW_TYPE_CARD`. Without the `appId`, the surface manager hides the
window about a second after it maps. With it, the window is treated like a
normal fullscreen app and keeps keyboard focus.

The default app id is `com.webosbrew.shim`. If you run more than one shim
instance, give each a unique id in `shim.c`; duplicate ids cause one window to
be minimized.

### Client side

The shim exposes:

| Global | Version | Notes |
| --- | --- | --- |
| `wl_compositor` | 4 | `damage_buffer` and buffer scale accepted |
| `wl_shm` | 1 | libwayland's built-in implementation |
| `wl_seat` | 4 | keyboard + pointer |
| `wl_output` | 3 | geometry, mode, scale |
| `xdg_wm_base` | 3 | toplevels, popups, positioners |
| `wl_data_device_manager` | 1 | minimal; required by GTK before it will bind a seat |

The data-device manager intentionally has no real clipboard implementation.
GTK3 postpones creating a seat until it has seen both `wl_compositor` and
`wl_data_device_manager`; this minimal global satisfies that check.

### Compositing

Client buffers are `wl_shm` buffers. The shim copies each mapped surface with
pixman into one of two fullscreen `wl_shm` buffers and commits that to the TV.
Buffers are held until the client commits a replacement, so pointer-driven
repaints never read a buffer the client is still drawing into.

A small green crosshair is drawn by the shim as a software cursor. That gives
visible feedback even when the TV compositor hides its own cursor.

### Stacking

The fullscreen main window is pinned at the bottom (`z=0`). Tool windows and
dialogs sit above it, popups above those. Stacking is assigned when a window
maps and is not changed on every repaint. Clicking a tool window raises it
only within the tool-window band.

### Input

webOS exposes **one `wl_seat` per input device**. The shim binds all of them,
not just the first one, and forwards the first keyboard/pointer that produces
events. This matters for the magic remote, which is usually not seat 0.

Vendor keycodes above Linux `KEY_MAX` (`0x2ff`) — for example `1198`/`1199`
from the magic remote — are dropped. Forwarding them makes GTK/LibreOffice
insert replacement characters.

## 6. Limitations

- **No GPU acceleration.** `/dev/mali0` and `/dev/fb0` are owned by the TV's
  `surface-manager`; `/sys/class/drm` has no usable connectors. Rendering is
  software (llvmpipe) and scales with resolution.
- **One fullscreen host surface.** Applications are nested inside it. This is
  not a multi-window desktop; you cannot switch them with a TV window manager.
- **No `wl_drm` / dmabuf.** Only `wl_shm` buffers are supported.
- **No viewporter or output scaling.** `wp_viewporter` and
  `zxdg_output_manager_v1` are absent, so clients that require them disable
  display scaling.
- **No xdg-foreign.** GIMP prints a harmless warning about this.
- **No real clipboard.** Copy/paste between applications is not implemented.
- **Fixed fullscreen output.** Toplevels are configured to the TV output size.
  A heavy 3D application may be more usable at a lower resolution, but the
  current shim does not scale windows; use the application's own resolution
  options where available.
- **Root/chroot required.** This is a rooted-TV experiment, not a supported
  webOS feature.

## 7. Troubleshooting

See [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) and
[`docs/APP-NOTES.md`](docs/APP-NOTES.md).

The most common failures are:

- `wl_display_add_socket ... Resource temporarily unavailable` — a stale
  socket/lock from a killed shim. Run `./shimctl.sh stop`, or remove
  `$XDG_RUNTIME_DIR/wayland-shim` and `.lock`.
- `cannot set up host Wayland connection` — wrong `XDG_RUNTIME_DIR` or
  `WAYLAND_DISPLAY`; check `wayland-info`.
- Application opens an X11 window / fails to start — force the backend with
  `GDK_BACKEND=wayland`, `QT_QPA_PLATFORM=wayland` or
  `SDL_VIDEODRIVER=wayland`.
- No input — confirm the program is connected to the shim, not the host
  display, and check `SHIM_DEBUG=1 ./shimctl.sh log`.

## Protocol files

`protocol/webos-shell.xml` is the webOS shell protocol used by the tested
webOS release. webOS adds requests to this interface between releases without
bumping the interface version, so a different TV may need the XML from
[`webosbrew/wayland-protocols`](https://github.com/webosbrew/wayland-protocols)
for its release.

The xdg-shell bindings are generated from the `wayland-protocols` installed by
Alpine; they are not committed.

## License

MIT. See [`LICENSE`](LICENSE).

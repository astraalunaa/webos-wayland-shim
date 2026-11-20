# Troubleshooting

## First checks

Run these **inside** the chroot:

```sh
echo "$XDG_RUNTIME_DIR"
echo "$WAYLAND_DISPLAY"
wayland-info | head -40
```

The host display should be visible at
`$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`. On the tested TV that is
`/tmp/xdg/wayland-0`.

Expected TV globals:

- `wl_compositor`
- `wl_shm`
- `wl_shell`
- `wl_webos_shell`
- several `wl_seat` globals
- **no `xdg_wm_base`**

If `wayland-info` cannot connect, fix the host display first. The shim cannot
create a window without it.

## `cannot set up host Wayland connection`

The shim prints:

```text
shim: missing host globals (...)
```

or fails to connect. Check:

```sh
ls -l "$XDG_RUNTIME_DIR"/wayland-*
```

`XDG_RUNTIME_DIR` is often `/tmp/xdg` because that is where the TV compositor
puts its socket. If you use a different runtime directory, start the shim with
that value:

```sh
XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0 ./shimctl.sh start
```

The helper scripts default to `HOST_WAYLAND_DISPLAY=wayland-0`; override it if
needed.

## `wl_display_add_socket ... Resource temporarily unavailable`

A previous shim died without removing its socket and abstract-socket lock.
`shimctl.sh start` removes them when no shim process is running. To do it by
hand:

```sh
./shimctl.sh stop
rm -f "$XDG_RUNTIME_DIR/wayland-shim" "$XDG_RUNTIME_DIR/wayland-shim.lock"
./shimctl.sh start
```

Do not remove the socket while a shim is still running; you will end up with
two servers and applications may connect to the wrong one.

## Window appears, then disappears after ~1 second

The webOS surface manager is rejecting the `appId` or the surface is being
minimized. Check the shim log for:

```text
shim: webos state 1
```

State `3` is fullscreen; state `1` is minimized. Make sure:

- `wl_webos_shell_surface.set_property("appId", ...)` is called **before** the
  first commit;
- the app id is unique among running shim instances;
- the TV has not launched another app that took focus.

## Window is visible but applications think there is no seat

GTK3 will not bind a `wl_seat` until it has also seen a
`wl_data_device_manager` global. The shim provides a minimal one. If you
modify the shim and remove it, keyboard/pointer objects will never be created
and GIMP/LibreOffice will log:

```text
gdk_seat_get_pointer: assertion 'GDK_IS_SEAT (seat)' failed
```

## No input

- Use `SHIM_DEBUG=1 ./shimctl.sh restart`, then check
  `./shimctl.sh log` for pointer/keyboard events.
- The TV exposes one seat per device. The shim binds all seats; if you modify
  it, do not stop at the first `wl_seat`.
- A Bluetooth keyboard must be paired with the TV first.
- The magic remote's `wl_pointer` events work, but its vendor keycodes
  (`1198`, `1199`, ...) are intentionally dropped. They are not real keyboard
  symbols and cause replacement characters in GTK/LibreOffice.
- If keyboard shortcuts act like the wrong modifier is held, check the
  keyboard's own programming (some keyboards can remap Ctrl/Cmd in firmware).

## Application chooses X11 or tries to open a display

Use the generic launcher, which exports the Wayland backends:

```sh
./run-on-shim.sh some-program
```

If the program ignores those hints, set the backend explicitly:

```sh
GDK_BACKEND=wayland ./run-on-shim.sh gimp
QT_QPA_PLATFORM=wayland ./run-on-shim.sh some-qt-app
SDL_VIDEODRIVER=wayland ./run-on-shim.sh some-sdl-app
```

## GIMP 3.x aborts on its first icon

See [`APP-NOTES.md`](APP-NOTES.md#gimp-3x-glycinbubblewrap-abort). This is a
GIMP/glycin packaging issue on the TV's old kernel, not a shim issue.

## LibreOffice is slow or runs out of memory

LibreOffice uses a lot of RAM. The TV has about 1 GB total. Close the browser
and other TV apps, and do not run LibreOffice and GIMP at the same time.

## SuperTuxKart crashes while loading

Likely OOM or software-rendering pressure. It runs with Mesa llvmpipe, not the
TV GPU. Close everything else; try a smaller `--screensize`; expect a low frame
rate at 1080p.

## Clipboard does not work

Expected. The shim's `wl_data_device_manager` is deliberately minimal so GTK
will bind a seat; it does not implement data offers or clipboard transfer.

## Raising the log level

```sh
SHIM_DEBUG=1 ./shimctl.sh restart
./shimctl.sh log
```

`SHIM_DEBUG=1` logs pointer/keyboard input, popup positioner values, and
per-surface window geometry.

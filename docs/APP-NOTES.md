# Application notes

These are host-application quirks, not requirements of the shim itself.

## GIMP 3.x glycin/bubblewrap abort

On the tested TV kernel (`4.4.84`), GIMP 3.2 aborts on its first SVG icon:

```text
Gtk:ERROR: ... Failed to load .../image-missing-symbolic.svg:
Loader process exited early with status '1'
Command: "bwrap" "--unshare-all" ...
gimp: fatal error: Aborted
```

GIMP uses glycin for SVG icons, and glycin runs its loader inside
`bubblewrap --unshare-all`. The sandbox fails on this vendor kernel. There is
no supported glycin switch to disable it.

Temporary workaround: replace `bwrap` inside the chroot with a shim that
strips the sandbox options and execs the loader directly. Keep the real binary
so the change is reversible.

```sh
[ -f /usr/bin/bwrap.real ] || mv /usr/bin/bwrap /usr/bin/bwrap.real

cat > /usr/bin/bwrap <<'SH'
#!/bin/sh
# glycin sandbox bypass: strip bwrap options, exec the payload directly
while [ $# -gt 0 ]; do
  case "$1" in
    --unshare-all|--die-with-parent|--clearenv) shift ;;
    --chdir|--dev|--tmpfs|--seccomp|--proc|--mqueue) shift 2 ;;
    --ro-bind|--ro-bind-try|--bind|--bind-try|--symlink|--setenv|--unsetenv) shift 3 ;;
    --) shift; break ;;
    *) break ;;
  esac
done
exec "$@"
SH

chmod +x /usr/bin/bwrap
```

Revert with:

```sh
mv /usr/bin/bwrap.real /usr/bin/bwrap
```

This weakens the glycin sandbox. Do it only inside the TV chroot.

GIMP may also log:

```text
Invalid theme: directory '/usr/share/gimp/3.0/themes/Default' contains neither ...
```

Forcing the PNG-based Legacy icon theme avoids some SVG loading:

```sh
mkdir -p /root/.config/GIMP/3.2
printf '(icon-theme "Legacy")\n' > /root/.config/GIMP/3.2/gimprc
```

## LibreOffice

The generic launcher already sets `SAL_USE_VCLPLUGIN=gtk3`. If you launch it
by hand:

```sh
SAL_USE_VCLPLUGIN=gtk3 ./run-on-shim.sh libreoffice --writer
```

LibreOffice is memory-hungry. Do not run it alongside GIMP on a 1 GB TV.

## SuperTuxKart

SuperTuxKart uses Mesa llvmpipe because the TV GPU is not exposed inside the
chroot. It works, but is slow at 1080p:

```sh
./run-on-shim.sh supertuxkart --screensize=1280x720
```

`-N` / `--no-start-screen` skips the menu and online checks:

```sh
./run-on-shim.sh supertuxkart --screensize=1280x720 -N
```

First start is extra slow: shaders are compiled and the texture cache is
built. If it dies while loading, it is probably OOM. Close other TV apps and
try a smaller resolution.

## Fallback: Xvfb + noVNC

If an application needs X11/GLX or the shim hits a protocol limitation, the
usual fallback is a virtual X server exported through the TV browser:

```sh
apk add xvfb x11vnc novnc websockify

Xvfb :1 -screen 0 1920x1080x24 -nolisten tcp &
x11vnc -display :1 -forever -shared -nopw -localhost -rfbport 5900 &
websockify --web /usr/share/novnc 0.0.0.0:6080 localhost:5900 &

DISPLAY=:1 some-program
```

Then open `http://<tv-ip>:6080/vnc.html` in the TV browser. This is much slower
and less integrated than the shim, but it works when an application insists on
X11.

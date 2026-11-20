# Protocol files

- `webos-shell.xml` is the webOS shell protocol used by the tested TV
  (webOS build `20250913080627`). It is kept here because webOS ships this
  interface only inside its compositor, not in a public development package.

The upstream protocol definitions change between webOS releases, sometimes
without bumping the interface version. If the shim builds but no window appears
on a different TV, compare this file with the dump for that release from:

- <https://github.com/webosbrew/wayland-protocols>
- <https://github.com/webosose/webos-wayland-extensions>

The xdg-shell server bindings are generated from the `wayland-protocols`
package installed by Alpine and are not committed.

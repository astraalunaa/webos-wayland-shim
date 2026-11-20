#!/bin/sh
# Install the build dependencies inside Alpine.
set -eu
apk update
apk add --no-cache \
    build-base pkgconf \
    wayland-dev wayland-protocols wayland-utils \
    pixman-dev libxkbcommon-dev

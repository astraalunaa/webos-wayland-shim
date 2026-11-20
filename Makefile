# webos-wayland-shim
#
# Requires: wayland-client, wayland-server, pixman, xkbcommon,
#           wayland-scanner and wayland-protocols.

CC       ?= cc
PKG_CONFIG ?= pkg-config
SCANNER  ?= wayland-scanner

PROTO_DIR := protocol
BUILD_DIR := build

WAYLAND_PROTOCOLS := $(patsubst //%,/%,$(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols))
XDG_XML := $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml

PKGS := wayland-client wayland-server pixman-1 xkbcommon
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
          -I$(PROTO_DIR) $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

GEN := \
	$(PROTO_DIR)/xdg-shell-server-protocol.h \
	$(PROTO_DIR)/xdg-shell-protocol.c \
	$(PROTO_DIR)/webos-shell-client-protocol.h \
	$(PROTO_DIR)/webos-shell-protocol.c

OBJS := \
	$(BUILD_DIR)/shim.o \
	$(BUILD_DIR)/xdg-shell-protocol.o \
	$(BUILD_DIR)/webos-shell-protocol.o

.PHONY: all clean check-deps

all: check-deps shim

shim: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/shim.o: shim.c $(GEN) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/xdg-shell-protocol.o: $(PROTO_DIR)/xdg-shell-protocol.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/webos-shell-protocol.o: $(PROTO_DIR)/webos-shell-protocol.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(PROTO_DIR)/xdg-shell-server-protocol.h: $(XDG_XML)
	$(SCANNER) server-header $< $@

$(PROTO_DIR)/xdg-shell-protocol.c: $(XDG_XML)
	$(SCANNER) private-code $< $@

$(PROTO_DIR)/webos-shell-client-protocol.h: $(PROTO_DIR)/webos-shell.xml
	$(SCANNER) client-header $< $@

$(PROTO_DIR)/webos-shell-protocol.c: $(PROTO_DIR)/webos-shell.xml
	$(SCANNER) private-code $< $@

check-deps:
	@$(PKG_CONFIG) --exists $(PKGS) wayland-protocols || \
		{ echo "Missing build dependencies. On Alpine:"; \
		  echo "  apk add build-base wayland-dev wayland-protocols pixman-dev libxkbcommon-dev"; \
		  exit 1; }
	@command -v $(SCANNER) >/dev/null || \
		{ echo "wayland-scanner not found (install wayland-dev)"; exit 1; }

clean:
	rm -rf $(BUILD_DIR) shim $(GEN)

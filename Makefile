VERSION  := 0.1.2
COMMIT   := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
CXX      ?= g++
# -ffp-contract=off: the crop maths must round exactly as the macOS original's does.
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -ffp-contract=off -pthread
CPPFLAGS += -DC1TUNE_VERSION='"$(VERSION)"' -DC1TUNE_COMMIT='"$(COMMIT)"'

CORE     := src/depthai.cpp src/settings.cpp src/firmware.cpp
DAEMON   := $(CORE) src/usb.cpp src/xlink.cpp src/session.cpp src/main.cpp
HEADERS  := $(wildcard src/*.h)

all: build/c1-tune build/c1-tune-panel

build/c1-tune: $(DAEMON) $(HEADERS) Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DAEMON) -o $@ $(LDFLAGS)

build/c1-tune-panel: $(wildcard panel/*) Makefile
	@mkdir -p build/panel
	cd build/panel && qmake6 ../../panel/panel.pro PANEL_VERSION=$(VERSION) && $(MAKE) -s
	cp build/panel/c1-tune-panel $@

# Protocol tests: encoders vs reference bytes. No camera needed.
test: build/tests
	build/tests tests/fixtures

build/tests: $(CORE) tests/main.cpp $(HEADERS) Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/main.cpp -o $@

# The pointer tool for clicking through the UI for real (see tests/ui/ui.sh).
build/ptr: tests/ui/ptr.c tests/ui/wlr-virtual-pointer-unstable-v1.xml
	@mkdir -p build/vp
	wayland-scanner client-header tests/ui/wlr-virtual-pointer-unstable-v1.xml build/vp/vp.h
	wayland-scanner private-code tests/ui/wlr-virtual-pointer-unstable-v1.xml build/vp/vp.c
	$(CC) -O1 -Ibuild/vp -o $@ tests/ui/ptr.c build/vp/vp.c -lwayland-client

# Everything that installs is bound to one exact commit. PIN is the 40-character commit the
# README names for the release; these targets refuse to run unless this checkout is exactly
# that commit with nothing changed or added, so what gets built (and, for the package and the
# udev rule, installed as root) is the reviewed release and not whatever a branch has become.
# Working on the code itself: PIN=worktree builds what is checked out, and says so.
PIN ?=
MAKEPKGFLAGS ?= -fsi

verify-pin:
	@if [ "$(PIN)" = worktree ]; then \
	  echo "PIN=worktree: building this working tree as it is, not a pinned release." >&2; \
	else \
	  printf '%s' "$(PIN)" | grep -Eq '^[0-9a-f]{40}$$' || { \
	    echo "This installs a pinned release. Run: make $(or $(MAKECMDGOALS),install) PIN=<the 40-character commit from the README>" >&2; exit 1; }; \
	  head=$$(git rev-parse HEAD 2>/dev/null) || { echo "Not a git checkout, so the commit can't be verified." >&2; exit 1; }; \
	  [ "$$head" = "$(PIN)" ] || { \
	    echo "This checkout is at $$head, not the pinned $(PIN)." >&2; \
	    echo "Run: git checkout --detach $(PIN)" >&2; exit 1; }; \
	  [ -z "$$(git status --porcelain)" ] || { \
	    echo "This checkout has local changes or extra files; a pinned install needs it untouched:" >&2; \
	    git status --short >&2; exit 1; }; \
	  echo "Verified: this checkout is exactly $(PIN)."; \
	fi

# Builds the Arch package and installs it (binary, panel, launcher entry, icon, udev rule).
# The PKGBUILD checks the pin again itself before it builds anything.
install: verify-pin
	cd pkgbuild && C1TUNE_PIN="$(PIN)" makepkg $(MAKEPKGFLAGS)

# Without sudo: the same files under ~/.local, plus the bar widget. The udev rule still has
# to be installed once (make udev); the package (make install) supersedes all of this.
PLUGIN := $(HOME)/.config/omarchy/plugins/com.ryankirkman.c1-tune

install-user: verify-pin all
	install -Dm755 build/c1-tune $(HOME)/.local/bin/c1-tune
	install -Dm755 build/c1-tune-panel $(HOME)/.local/bin/c1-tune-panel
	install -Dm755 scripts/fetch-opal-tuning $(HOME)/.local/share/c1-tune/fetch-opal-tuning
	install -Dm644 pkgbuild/c1-tune.svg $(HOME)/.local/share/icons/hicolor/scalable/apps/c1-tune.svg
	sed 's|^Exec=c1-tune|Exec=$(HOME)/.local/bin/c1-tune|' pkgbuild/c1-tune.desktop > build/c1-tune.desktop
	install -Dm644 build/c1-tune.desktop $(HOME)/.local/share/applications/c1-tune.desktop
	install -Dm644 manifest.json $(PLUGIN)/manifest.json
	install -Dm644 -t $(PLUGIN)/bar-widget bar-widget/BarWidget.qml bar-widget/C1Icon.qml
	install -Dm644 pkgbuild/opal-c1-no-suspend.conf $(HOME)/.config/wireplumber/wireplumber.conf.d/opal-c1-no-suspend.conf

uninstall-user:
	rm -f $(HOME)/.local/bin/c1-tune $(HOME)/.local/bin/c1-tune-panel $(HOME)/.local/share/applications/c1-tune.desktop
	rm -f $(HOME)/.local/share/icons/hicolor/scalable/apps/c1-tune.svg $(HOME)/.config/autostart/c1-tune.desktop
	rm -rf $(HOME)/.local/share/c1-tune/fetch-opal-tuning $(HOME)/.config/omarchy/plugins/com.ryankirkman.c1-tune
	rm -f $(HOME)/.config/wireplumber/wireplumber.conf.d/opal-c1-no-suspend.conf

# Just the udev rule, for running from the build directory without the package.
udev: verify-pin
	sudo install -Dm644 pkgbuild/70-opal-c1.rules /etc/udev/rules.d/70-opal-c1.rules
	sudo udevadm control --reload
	sudo udevadm trigger --attr-match=idVendor=03e7

clean:
	rm -rf build

.PHONY: all test verify-pin install install-user uninstall-user udev clean

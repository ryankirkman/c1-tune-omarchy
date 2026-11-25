VERSION  := 0.1.0
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
	cd build/panel && qmake6 ../../panel/panel.pro "VERSION_STRING=$(VERSION) ($(COMMIT))" && $(MAKE) -s
	cp build/panel/c1-tune-panel $@

# Protocol tests: encoders vs reference bytes. No camera needed.
test: build/tests
	build/tests tests/fixtures

build/tests: $(CORE) tests/main.cpp $(HEADERS) Makefile
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) tests/main.cpp -o $@

# Builds the Arch package and installs it (binary, panel, launcher entry, icon, udev rule).
install:
	cd pkgbuild && makepkg -fsi

# Just the udev rule, for running from the build directory without the package.
udev:
	sudo install -Dm644 pkgbuild/70-opal-c1.rules /etc/udev/rules.d/70-opal-c1.rules
	sudo udevadm control --reload
	sudo udevadm trigger --attr-match=idVendor=03e7

clean:
	rm -rf build

.PHONY: all test install udev clean

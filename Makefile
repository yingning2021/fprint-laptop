CC ?= cc
AR ?= ar
CPPFLAGS ?=
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic
LDLIBS := $(shell pkg-config --libs libusb-1.0)
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
PROJECT_CPPFLAGS := -Iinclude
LIBFPRINT_BUILD ?= build/libfprint-oms-repro
LIBFPRINT_PC := $(abspath $(LIBFPRINT_BUILD)/meson-uninstalled)
LIBFPRINT_CFLAGS := $(shell PKG_CONFIG_PATH=$(LIBFPRINT_PC) pkg-config --cflags libfprint-2 2>/dev/null)
LIBFPRINT_LIBS := $(shell PKG_CONFIG_PATH=$(LIBFPRINT_PC) pkg-config --libs libfprint-2 2>/dev/null)

.PHONY: all check clean libfprint-build arch-package

all: build/omsctl build/oms-probe

build/oms_protocol.o: src/oms_protocol.c include/oms_protocol.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(USB_CFLAGS) -c $< -o $@

build/libomsproto.a: build/oms_protocol.o
	$(AR) rcs $@ $<

build/omsctl: tools/omsctl.c build/libomsproto.a include/oms_protocol.h
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(USB_CFLAGS) $< build/libomsproto.a -o $@ $(LDLIBS)

build/oms-probe: build/omsctl
	cp -f $< $@

build/test_protocol: tests/test_protocol.c build/libomsproto.a include/oms_protocol.h
	$(CC) $(CPPFLAGS) $(PROJECT_CPPFLAGS) $(CFLAGS) $(USB_CFLAGS) $< build/libomsproto.a -o $@ $(LDLIBS)

build/libfprint-smoke: tools/libfprint-smoke.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIBFPRINT_CFLAGS) $< -o $@ $(LIBFPRINT_LIBS) \
		-Wl,-rpath,$(abspath $(LIBFPRINT_BUILD)/libfprint)

check: build/test_protocol
	./build/test_protocol

libfprint-build:
	./scripts/build-libfprint.sh

arch-package:
	./packaging/arch/build-package.sh

clean:
	rm -f build/oms-probe build/omsctl build/oms_protocol.o build/libomsproto.a build/test_protocol build/libfprint-smoke

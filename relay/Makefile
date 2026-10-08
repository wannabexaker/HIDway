# hidwayd - Raspberry Pi relay. Plain C, no external dependencies.
#   make            build ./hidwayd (version taken from git)
#   make check      build and run the common/ unit tests
#   make run        build and run (bind 0.0.0.0:47800, verbose, no serial)
#   make clean
#
# Deployment on the Pi (service, udev rule, auto-update) is handled by
# deploy/install.sh; see docs/RELAY.md.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -I../common

VERSION ?= $(shell git -C .. describe --always --dirty --tags 2>/dev/null || echo dev)
CPPFLAGS += -DHIDWAY_VERSION='"$(VERSION)"'

COMMON  = ../common/serial.c ../common/cobs.c ../common/crc16.c \
          ../common/hidway_crypto.c ../common/vendor/monocypher/monocypher.c
HEADERS = ../common/protocol.h ../common/hidway_kbd.h ../common/serial.h \
          ../common/cobs.h ../common/crc16.h ../common/hidway_crypto.h \
          ../common/vendor/monocypher/monocypher.h

TEST_SRC = ../tests/test_common.c ../common/hidway_kbd.c ../common/hidway_motion.c \
           ../common/keymap_sc2hid.c $(COMMON)

hidwayd: hidwayd.c $(COMMON) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ hidwayd.c $(COMMON)

hidway_tests: $(TEST_SRC) $(HEADERS) ../common/hidway_motion.h ../common/keymap_sc2hid.h
	$(CC) $(CFLAGS) -I../common -o $@ $(TEST_SRC)

.PHONY: check run clean
check: hidway_tests
	./hidway_tests

run: hidwayd
	./hidwayd

clean:
	rm -f hidwayd hidway_tests

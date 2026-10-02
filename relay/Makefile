# hidwayd - Raspberry Pi relay. Plain C, no external dependencies.
#   make            build ./hidwayd
#   make run        build and run (bind 0.0.0.0:47800, verbose, no serial)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -I../common

COMMON  = ../common/serial.c ../common/cobs.c ../common/crc16.c
HEADERS = ../common/protocol.h ../common/hidway_kbd.h ../common/serial.h \
          ../common/cobs.h ../common/crc16.h

hidwayd: hidwayd.c $(COMMON) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ hidwayd.c $(COMMON)

.PHONY: run clean
run: hidwayd
	./hidwayd

clean:
	rm -f hidwayd

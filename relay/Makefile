# hidwayd - Raspberry Pi relay. Plain C, no external dependencies.
#   make            build ./hidwayd
#   make run        build and run (bind 0.0.0.0:47800, verbose)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -I../common

hidwayd: hidwayd.c ../common/protocol.h ../common/hidway_kbd.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ hidwayd.c

.PHONY: run clean
run: hidwayd
	./hidwayd

clean:
	rm -f hidwayd

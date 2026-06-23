CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += $(shell pkg-config --cflags ncursesw)
LDLIBS  += $(shell pkg-config --libs ncursesw)

okfi: okfi.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f okfi

.PHONY: clean

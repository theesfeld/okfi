CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += $(shell pkg-config --cflags ncursesw)
LDLIBS  += $(shell pkg-config --libs ncursesw)

fokf: fokf.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f fokf

.PHONY: clean

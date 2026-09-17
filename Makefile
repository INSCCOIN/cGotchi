CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -s
PREFIX ?= /usr/local

cGotchi: cGotchi.c
	$(CC) $(CFLAGS) -o cGotchi cGotchi.c -lncurses

install: cGotchi
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 cGotchi $(DESTDIR)$(PREFIX)/bin/cGotchi

clean:
	rm -f cGotchi

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags xft freetype2)
LDFLAGS = -lX11 -lXrender -lXft -lXcursor -lm

window: window.c
	$(CC) $(CFLAGS) -o window window.c $(LDFLAGS)
	@bash tests/run_tests.sh

install: window
	cp window /usr/local/bin/window

uninstall:
	rm -f /usr/local/bin/window

clean:
	rm -f window

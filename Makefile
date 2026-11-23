CC ?= cc
CFLAGS ?= -O2
WARNINGS = -std=c99 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Isrc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
DESTDIR ?=
INSTALL ?= install

OBJECTS = build/pgn_reverse.o build/pgn_scan.o
COUNT_OBJECTS = build/pgn_count.o build/pgn_scan.o
LIST_OBJECTS = build/pgn_list.o build/pgn_scan.o

.PHONY: all clean test debug release install uninstall

all: pgn-reverse pgn-count pgn-list

pgn-reverse: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

pgn-count: $(COUNT_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(COUNT_OBJECTS)

pgn-list: $(LIST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(LIST_OBJECTS)

build:
	mkdir -p build

build/pgn_reverse.o: src/pgn_reverse.c src/pgn_scan.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c -o $@ $<

build/pgn_count.o: src/pgn_count.c src/pgn_scan.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c -o $@ $<

build/pgn_list.o: src/pgn_list.c src/pgn_scan.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c -o $@ $<

build/pgn_scan.o: src/pgn_scan.c src/pgn_scan.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c -o $@ $<

test: pgn-reverse
	sh tests/test_cli.sh

debug: clean
	$(MAKE) CFLAGS='-g -O0' all

release: clean
	$(MAKE) CFLAGS='-O2' all

install: pgn-reverse pgn-count pgn-list
	$(INSTALL) -d -m 755 '$(DESTDIR)$(BINDIR)' '$(DESTDIR)$(MANDIR)/man1'
	$(INSTALL) -m 755 pgn-reverse '$(DESTDIR)$(BINDIR)/pgn-reverse'
	$(INSTALL) -m 755 pgn-count '$(DESTDIR)$(BINDIR)/pgn-count'
	$(INSTALL) -m 755 pgn-list '$(DESTDIR)$(BINDIR)/pgn-list'
	$(INSTALL) -m 644 man/pgn-reverse.1 '$(DESTDIR)$(MANDIR)/man1/pgn-reverse.1'
	$(INSTALL) -m 644 man/pgn-count.1 '$(DESTDIR)$(MANDIR)/man1/pgn-count.1'
	$(INSTALL) -m 644 man/pgn-list.1 '$(DESTDIR)$(MANDIR)/man1/pgn-list.1'

uninstall:
	rm -f '$(DESTDIR)$(BINDIR)/pgn-reverse' \
	      '$(DESTDIR)$(BINDIR)/pgn-count' \
	      '$(DESTDIR)$(BINDIR)/pgn-list' \
	      '$(DESTDIR)$(MANDIR)/man1/pgn-reverse.1' \
	      '$(DESTDIR)$(MANDIR)/man1/pgn-count.1' \
	      '$(DESTDIR)$(MANDIR)/man1/pgn-list.1'

clean:
	rm -rf build pgn-reverse pgn-count pgn-list

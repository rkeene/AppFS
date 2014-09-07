CC = gcc
PKG_CONFIG = pkg-config
CFLAGS = $(shell $(PKG_CONFIG) --cflags fuse)
LIBS = $(shell $(PKG_CONFIG) --libs fuse)
PREFIX = /usr/local
prefix = $(PREFIX)
bindir = $(prefix)/bin

all: appfs

appfs: appfs.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o appfs appfs.o $(LIBS)

appfs.o: appfs.c

install: appfs
	cp appfs $(bindir)

clean:
	rm -f appfs appfs.o

distclean: clean

.PHONY: all clean distclean install

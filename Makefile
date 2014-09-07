CC = gcc
PKG_CONFIG = pkg-config
TCL_CFLAGS =
TCL_LDFLAGS =
TCL_LIBS = -ltcl
CFLAGS = -Wall -g3 $(shell $(PKG_CONFIG) --cflags fuse) $(TCL_CFLAGS)
LDFLAGS = $(TCL_LDFLAGS)
LIBS = $(shell $(PKG_CONFIG) --libs fuse) $(TCL_LIBS)
PREFIX = /usr/local
prefix = $(PREFIX)
bindir = $(prefix)/bin

all: appfs

appfs: appfs.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o appfs appfs.o $(LIBS)

appfs-test: appfs-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o appfs-test appfs-test.o $(LIBS)

appfs.o: appfs.c appfs.tcl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o appfs.o -c appfs.c

appfs-test.o: appfs.c appfs.tcl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DAPPFS_TEST_DRIVER=1 -o appfs-test.o -c appfs.c

appfs.tcl.h: appfs.tcl stringify.tcl
	./stringify.tcl appfs.tcl > appfs.tcl.h.new
	mv appfs.tcl.h.new appfs.tcl.h

install: appfs
	cp appfs $(bindir)

test: appfs-test
	./appfs-test

clean:
	rm -f appfs appfs.o
	rm -f appfs-test appfs-test.o
	rm -f appfs.tcl.h

distclean: clean

.PHONY: all test clean distclean install

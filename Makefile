CC = gcc
PKG_CONFIG = pkg-config
CFLAGS = -Wall -g3 $(shell $(PKG_CONFIG) --cflags fuse) $(shell $(PKG_CONFIG) --cflags sqlite3) $(TCL_CFLAGS)
LDFLAGS = $(TCL_LDFLAGS)
LIBS = $(shell $(PKG_CONFIG) --libs fuse) $(shell $(PKG_CONFIG) --libs sqlite3) $(TCL_LIBS)
PREFIX = /usr/local
prefix = $(PREFIX)
bindir = $(prefix)/bin

ifneq ($(TCLKIT_SDK_DIR),)
TCLCONFIG_SH_PATH = $(TCLKIT_SDK_DIR)/lib/tclConfig.sh
TCL_LDFLAGS = -Wl,-R,$(TCLKIT_SDK_DIR)/lib
export TCLKIT_SDK_DIR
else
TCLCONFIG_SH_PATH = $(shell echo 'puts [::tcl::pkgconfig get libdir,install]' | tclsh)/tclConfig.sh
endif
TCL_CFLAGS = $(shell . $(TCLCONFIG_SH_PATH); echo "$${TCL_INCLUDE_SPEC}")
TCL_LIBS = $(shell . $(TCLCONFIG_SH_PATH); echo "$${TCL_LIB_SPEC}")

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

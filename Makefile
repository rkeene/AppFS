CC = gcc
PKG_CONFIG = pkg-config
CFLAGS = -Wall -g3 -ggdb3 $(shell $(PKG_CONFIG) --cflags fuse) $(shell $(PKG_CONFIG) --cflags sqlite3) $(TCL_CFLAGS)
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

all: appfsd

appfsd: appfsd.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o appfsd appfsd.o $(LIBS)

appfsd.o: appfsd.c appfsd.tcl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o appfsd.o -c appfsd.c

appfsd.tcl.h: appfsd.tcl stringify.tcl
	sed 's@[\\"]@\\&@g;s@^@   "@;s@$$@\\n"@' appfsd.tcl > appfsd.tcl.h.new
	mv appfsd.tcl.h.new appfsd.tcl.h

install: appfsd
	cp appfsd $(bindir)

clean:
	rm -f appfsd appfsd.o
	rm -f appfsd.tcl.h

distclean: clean

.PHONY: all test clean distclean install

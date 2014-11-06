CC = gcc
PKG_CONFIG = pkg-config
FUSE_CFLAGS = $(shell $(PKG_CONFIG) --cflags fuse)
SQLITE3_CFLAGS = $(shell $(PKG_CONFIG) --cflags sqlite3)
CFLAGS = -Wall $(FUSE_CFLAGS) $(SQLITE3_CFLAGS) $(TCL_CFLAGS) -DDEBUG=1
LDFLAGS = $(TCL_LDFLAGS)
FUSE_LIBS = $(shell $(PKG_CONFIG) --libs fuse)
SQLITE3_LIBS = $(shell $(PKG_CONFIG) --libs sqlite3)
LIBS = $(FUSE_LIBS) $(SQLITE3_LIBS) $(TCL_LIBS)
PREFIX = /usr/local
prefix = $(PREFIX)
bindir = $(prefix)/bin
sbindir = $(prefix)/sbin

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

appfsd.tcl.h: appfsd.tcl sha1.tcl
	sed '/@@SHA1\.TCL@@/ r sha1.tcl' appfsd.tcl | sed '/@@SHA1\.TCL@@/ d' | sed 's@[\\"]@\\&@g;s@^@   "@;s@$$@\\n"@' > appfsd.tcl.h.new
	mv appfsd.tcl.h.new appfsd.tcl.h

install: appfsd
	if [ ! -d '$(DESTDIR)$(sbindir)' ]; then mkdir -p '$(DESTDIR)$(sbindir)'; chmod 755 '$(DESTDIR)$(sbindir)'; fi
	cp appfsd '$(DESTDIR)$(sbindir)/'

clean:
	rm -f appfsd appfsd.o
	rm -f appfsd.tcl.h

distclean: clean

.PHONY: all test clean distclean install

APPFS_VERSION  = 1.12
CC             = gcc
PKG_CONFIG     = pkg-config
FUSE_CFLAGS    = $(shell $(PKG_CONFIG) --cflags fuse)
CFLAGS_DEBUG   = -Wall -g3 -ggdb3 -DDEBUG=1 -UNDEBUG -O0 -DAPPFS_EXIT_PATH=1
CFLAGS_RELEASE = -Wall -UDEBUG -DNDEBUG=1 -O3
ifneq ($(APPFS_DEBUG_BUILD),1)
CFLAGS         += $(FUSE_CFLAGS) $(TCL_CFLAGS) $(CFLAGS_RELEASE)
else
CFLAGS         += $(FUSE_CFLAGS) $(TCL_CFLAGS) $(CFLAGS_DEBUG)
endif
LDFLAGS        += $(TCL_LDFLAGS)
FUSE_LIBS      = $(shell $(PKG_CONFIG) --libs fuse)
LIBS           += $(FUSE_LIBS) $(TCL_LIBS)
PREFIX         = /usr/local
prefix         = $(PREFIX)
exec_prefix    = $(prefix)
bindir         = $(exec_prefix)/bin
sbindir        = $(exec_prefix)/sbin
mandir         = $(prefix)/share/man

ifneq ($(TCLKIT_SDK_DIR),)
TCLCONFIG_SH_PATH = $(TCLKIT_SDK_DIR)/lib/tclConfig.sh
TCL_LDFLAGS = -Wl,-R,$(TCLKIT_SDK_DIR)/lib
export TCLKIT_SDK_DIR
else
TCLCONFIG_SH_PATH = $(shell echo 'puts [::tcl::pkgconfig get libdir,install]' | tclsh)/tclConfig.sh
endif
TCL_CFLAGS = $(shell . $(TCLCONFIG_SH_PATH); echo "$${TCL_INCLUDE_SPEC} $${TCL_DEFS}")
TCL_LIBS = $(shell . $(TCLCONFIG_SH_PATH); echo "$${TCL_LIB_SPEC} $${TCL_LIBS}")

all: appfsd

appfsd: appfsd.o sha1.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o appfsd appfsd.o sha1.o $(LIBS)

appfsd.o: appfsd.c appfsd.tcl.h pki.tcl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o appfsd.o -c appfsd.c

sha1.o: sha1.c sha1.tcl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o sha1.o -c sha1.c

pki.tcl:
	rm -f pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/asn/asn.tcl?name=aea6802a16e69c9f2d4f5eca20fdc23174609731 > pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/aes/aes.tcl?name=94452b42b4ca98298ab1465c40fd87d11a40cf5e >> pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/des/tcldes.tcl?name=ffea6ca6eb4468c0edef7a745b1dadc632ff5aeb >> pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/des/des.tcl?name=5d8f3a7c1a6ea88ee988652643db8f06038aff49 >> pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/math/bignum.tcl?name=3bc84d9b1f18c2e7360573381317c4dc9af731f9 >> pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/md5/md5x.tcl?name=3cddfa803d680a79ab7dfac90edfd751f3d4fadd >> pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/sha1/sha256.tcl?name=1fd001eb65e88c823b980456726079deae3512df >> pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/base64/base64.tcl?name=812f146bfc1a12bb863a7a845548b9eef9cd6573 >> pki.tcl.new
	curl -L https://core.tcl-lang.org/tcllib/raw/modules/pki/pki.tcl?name=8318fd31981dcc00bfadd6c427518f9d71a12b34 >> pki.tcl.new
	openssl sha1 pki.tcl.new | grep 'aad7cca08cca00c8f7cd6eccc46e61da235753fc' >/dev/null
	mv pki.tcl.new pki.tcl

%.tcl.h: %.tcl
	sed 's@[\\"]@\\&@g;s@^@   "@;s@$$@\\n"@' $^ > $@.new
	mv $@.new $@

install: appfsd appfs-cache appfs-mkfs appfsd.8
	if [ ! -d '$(DESTDIR)$(sbindir)' ]; then mkdir -p '$(DESTDIR)$(sbindir)' && chmod 755 '$(DESTDIR)$(sbindir)'; fi
	if [ ! -d '$(DESTDIR)$(bindir)' ]; then mkdir -p '$(DESTDIR)$(bindir)' && chmod 755 '$(DESTDIR)$(bindir)'; fi
	if [ ! -d '$(DESTDIR)$(mandir)' ]; then mkdir -p '$(DESTDIR)$(mandir)' && chmod 755 '$(DESTDIR)$(mandir)'; fi
	cp appfsd '$(DESTDIR)$(sbindir)/'
	ln -s appfsd '$(DESTDIR)$(sbindir)/mount.appfs'
	cp appfs-cache '$(DESTDIR)$(sbindir)/'
	cp appfs-mkfs '$(DESTDIR)$(bindir)/'
	cp appfsd.8 '$(DESTDIR)$(mandir)/'

# Internal target to publish appfs application to AppFS
appfs-$(APPFS_VERSION).cpio: appfs-cache appfs-cert appfs-mkfs
	rm -rf __TMP__
	mkdir -p __TMP__/appfs/noarch-noarch/$(APPFS_VERSION)/bin
	cp appfs-cache appfs-cert appfs-mkfs __TMP__/appfs/noarch-noarch/$(APPFS_VERSION)/bin
	fossil cat -r packages build > __TMP__/appfs/noarch-noarch/$(APPFS_VERSION)/bin/appfs-build
	chmod 755 __TMP__/appfs/noarch-noarch/$(APPFS_VERSION)/bin/*
	( cd __TMP__ && find appfs/noarch-noarch/$(APPFS_VERSION) | cpio --owner 0:0 -H newc -o ) > appfs-$(APPFS_VERSION).cpio.new
	rm -rf __TMP__
	mv appfs-$(APPFS_VERSION).cpio.new appfs-$(APPFS_VERSION).cpio

clean:
	rm -f appfsd appfsd.o
	rm -f appfsd.tcl.h
	rm -f sha1.o sha1.tcl.h
	rm -f pki.tcl.new pki.tcl.h

distclean: clean
	rm -f appfs-$(APPFS_VERSION).cpio

mrproper: distclean
	rm -f pki.tcl

.PHONY: all install clean distclean mrproper

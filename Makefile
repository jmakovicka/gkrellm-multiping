ifeq ($(INSTALLROOT),)
INSTALLROOT = /usr/local
endif
ifeq ($(PLUGINDIR),)
PLUGINDIR=$(INSTALLROOT)/lib/gkrellm2/plugins
endif
ifeq ($(HELPERDIR),)
HELPERDIR=$(INSTALLROOT)/lib/gkrellm2/exec
endif
VERSION = 2.0.16
PKGNAME = gkrellm-multiping
CC = gcc

DISTFILES = \
	AUTHORS Makefile multiping.c pinger.c decal_multiping_status.xpm \
	Themes ChangeLog README \
	debian/changelog debian/control debian/copyright \
	debian/gkrellm-multiping.postinst debian/rules \

DISTDIR = $(PKGNAME)-$(VERSION)

all: pinger multiping.so

pinger: pinger.c
	$(CC) -Wl,--as-needed pinger.c $(shell pkg-config glib-2.0 --cflags) \
		$(OPT) -lpthread $(shell pkg-config glib-2.0 --libs) -Wall -o pinger

multiping.o: multiping.c decal_multiping_status.xpm
	$(CC) -Wall -fPIC -Wall $(shell pkg-config gtk+-2.0 --cflags) $(OPT) \
	-DHELPERDIR=\"$(HELPERDIR)\" -DVERSION=\"$(VERSION)\" -c multiping.c

multiping.so: multiping.o
	$(CC) -shared -Wl,--as-needed multiping.o $(shell pkg-config gtk+-2.0 --libs) -o multiping.so

clean:
	rm -f *.o *.so pinger core

install: pinger multiping.so
	install -d $(PLUGINDIR)
	install -d $(HELPERDIR)
	install -c -s -m 644 multiping.so $(PLUGINDIR)
	install -c -s -m 4755 pinger $(HELPERDIR)
	echo "pinger helper is installed suid root"

dist:
	rm -rf $(DISTDIR)
	mkdir $(DISTDIR) $(DISTDIR)/debian
	for I in $(DISTFILES) ; do cp "$$I" $(DISTDIR)/$$I ; done
	tar zcf $(PKGNAME)-$(VERSION).tgz $(PKGNAME)-$(VERSION)
	rm -rf $(DISTDIR)

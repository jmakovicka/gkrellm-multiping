INSTALLDIR = /usr/local/lib/gkrellm2/plugins
VERSION = 2.0.7
PKGNAME = gkrellm-multiping
#OPT = -march=athlon -O2
#CC = gcc-3.0
CC = gcc

all: pinger multiping.so

pinger: pinger.c
	$(CC) `pkg-config glib-2.0 --cflags` $(OPT) -lpthread `pkg-config glib-2.0 --libs` -Wall -o pinger pinger.c

multiping.o: multiping.c decal_multiping_status.xpm
	$(CC) -Wall -fPIC -Wall `pkg-config gtk+-2.0 --cflags` $(OPT) -DVERSION=\"$(VERSION)\" -c multiping.c

multiping.so: multiping.o
	$(CC) -shared -Wl -ggdb `pkg-config gtk+-2.0 --libs`-o multiping.so multiping.o

clean:
	rm -f *.o *.so core

install: pinger multiping.so
	install -d $(INSTALLDIR)
	install -c -s -m 644 multiping.so $(INSTALLDIR)
	install -c -s -m 4755 pinger $(INSTALLDIR)
	echo "pinger helper is installed suid root"

dist:
	rm -rf $(PKGNAME)-$(VERSION)
	mkdir $(PKGNAME)-$(VERSION)
	cp AUTHORS Makefile multiping.c pinger.c decal_multiping_status.xpm Themes ChangeLog $(PKGNAME)-$(VERSION)/
	tar zcf $(PKGNAME)-$(VERSION).tgz $(PKGNAME)-$(VERSION)
	rm -rf $(PKGNAME)-$(VERSION)

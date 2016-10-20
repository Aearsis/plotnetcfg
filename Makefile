ifeq ($(jansson),)
LIBS=$(shell pkg-config --libs jansson)
else
LIBS=$(wildcard $(jansson)/src/.libs/libjansson.a $(jansson)/lib/libjansson.a)
INCLUDE=-I$(jansson)/src
endif

VERSION=$(shell git describe 2>/dev/null || cat version)
FLAGS=-D_GNU_SOURCE -DVERSION='"${VERSION}"' -std=c99 -W -Wall $(INCLUDE) $(CFLAGS)

OBJECTS=addr args ethtool frontend handler if label main master \
        match netlink netns route sysfs tunnel utils
HANDLERS=bond bridge iov openvswitch team veth vlan vxlan route
FRONTENDS=dot json

OBJ=$(OBJECTS:%=%.o) $(HANDLERS:%=handlers/%.o) $(FRONTENDS:%=frontends/%.o)

all: plotnetcfg

Makefile.dep: $(OBJ:.o=.c)
	$(CC) -M $(FLAGS) $(OBJ:.o=.c) | sed 's,\($*\)\.o[ :]*,\1.o $@: ,g' >$@

-include Makefile.dep

plotnetcfg: check-libs $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(LIBS)

%.o: %.c
	$(CC) -c $(FLAGS) -o $@ $<

clean:
	rm -f Makefile.dep plotnetcfg *.o handlers/*.o frontends/*.o

install: plotnetcfg
	install -d $(DESTDIR)/usr/sbin/
	install plotnetcfg $(DESTDIR)/usr/sbin/
	install -d $(DESTDIR)/usr/share/man/man8/
	install -m 644 -t $(DESTDIR)/usr/share/man/man8/ plotnetcfg.8
	install -d $(DESTDIR)/usr/share/man/man5/
	install -m 644 plotnetcfg-json.5 $(DESTDIR)/usr/share/man/man5/

.PHONY: check-libs
check-libs:
	@if [ "$(LIBS)" = "" ]; then \
	echo "ERROR: libjansson not found."; \
	exit 1; \
	fi

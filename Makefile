CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
ARCHIVE_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libarchive)
ARCHIVE_LIBS ?= $(shell $(PKG_CONFIG) --libs libarchive)
PREFIX ?= /usr/local
CPPFLAGS += -D_GNU_SOURCE $(ARCHIVE_CFLAGS)
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -MMD -MP
SOURCE_DIR ?= $(if $(wildcard src/main.c),src,tests/src)
SOURCES = $(addprefix $(SOURCE_DIR)/,main.c package.c database.c repository.c)
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all check install install-repo clean static
all: pkgr

pkgr: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(ARCHIVE_LIBS) $(LDLIBS)

static:
	mkdir -p build
	$(CC) $(CPPFLAGS) $(filter-out -MMD -MP,$(CFLAGS)) -static -s $(LDFLAGS) \
	    -o build/pkgr $(SOURCES) $(ARCHIVE_LIBS) $(LDLIBS)

check: pkgr
	sh tests/smoke.sh

install: pkgr
	install -Dm755 pkgr "$(DESTDIR)$(PREFIX)/bin/pkgr"

install-repo:
	install -d "$(DESTDIR)/usr/share/pkgr/repo"
	install -m644 build/repo/*.pkgr.tar.gz "$(DESTDIR)/usr/share/pkgr/repo/"

clean:
	rm -f pkgr $(OBJECTS) $(OBJECTS:.o=.d)

-include $(OBJECTS:.o=.d)

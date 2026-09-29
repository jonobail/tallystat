NAME     := tallystat
VERSION  := 1.0.0
ARCH     := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)

PREFIX   ?= /usr/local
DESTDIR  ?=

CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -Wshadow -Wno-format-truncation -DVERSION=\"$(VERSION)\" \
            -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
LDFLAGS  += -pie -Wl,-z,relro,-z,now
LDLIBS   += -lm

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:src/%.c=build/%.o)

PKGDIR   := build/pkg
DEB      := dist/$(NAME)_$(VERSION)_$(ARCH).deb
REPO     := dist/apt

.PHONY: all clean install uninstall deb repo

all: $(NAME)

$(NAME): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: $(NAME)
	install -Dm755 $(NAME) $(DESTDIR)$(PREFIX)/bin/$(NAME)
	install -Dm644 man/$(NAME).1 $(DESTDIR)$(PREFIX)/share/man/man1/$(NAME).1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(NAME) $(DESTDIR)$(PREFIX)/share/man/man1/$(NAME).1

# Binary .deb built straight from the tree with dpkg-deb (no debhelper needed).
deb: $(NAME)
	rm -rf $(PKGDIR)
	install -Dm755 $(NAME) $(PKGDIR)/usr/bin/$(NAME)
	strip --strip-unneeded $(PKGDIR)/usr/bin/$(NAME)
	install -d $(PKGDIR)/usr/share/man/man1 $(PKGDIR)/usr/share/doc/$(NAME) $(PKGDIR)/DEBIAN
	gzip -9n < man/$(NAME).1 > $(PKGDIR)/usr/share/man/man1/$(NAME).1.gz
	install -m644 debian/copyright $(PKGDIR)/usr/share/doc/$(NAME)/copyright
	gzip -9n < debian/changelog > $(PKGDIR)/usr/share/doc/$(NAME)/changelog.Debian.gz
	sed -e 's/@VERSION@/$(VERSION)/' -e 's/@ARCH@/$(ARCH)/' \
	    -e "s/@LIBC@/$$(objdump -T $(NAME) | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)/" \
	    -e "s/@SIZE@/$$(du -sk --exclude=DEBIAN $(PKGDIR) | cut -f1)/" \
	    packaging/control.in > $(PKGDIR)/DEBIAN/control
	chmod -R u=rwX,go=rX $(PKGDIR) && chmod 755 $(PKGDIR)/usr/bin/$(NAME)
	mkdir -p dist
	dpkg-deb --root-owner-group -Zxz --build $(PKGDIR) $(DEB)
	@echo "built $(DEB)"

# Flat local apt repository so `apt-get install tallystat` works.
repo: deb
	mkdir -p $(REPO)
	cp $(DEB) $(REPO)/
	cd $(REPO) && dpkg-scanpackages --multiversion . /dev/null 2>/dev/null > Packages && gzip -9kf Packages
	@echo "apt repository ready in $(REPO)"

clean:
	rm -rf build dist $(NAME)

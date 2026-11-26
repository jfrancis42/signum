# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis, N0GQ
# signum: the P1/P2/P3 modem behind the classic host-mode interface (README.md)
# ./configure writes config.mk; without it these defaults apply
-include config.mk
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -pthread
CRYPTO   ?= no
# Encryption (docs/md/encryption.md), opt-in: ./configure --enable-encryption (CRYPTO = yes) needs OpenSSL's
# libcrypto and zstd; the default (CRYPTO = no) builds with no encryption code at all (SIGNUM_NO_CRYPTO). The code is compiled
# in with SIGNUM_CRYPTO (in DEFS).
CRYPTO_LIBS ?= -lcrypto -lzstd
LIBS     ?= -lasound $(if $(filter yes,$(CRYPTO)),$(CRYPTO_LIBS))
prefix     ?= /usr/local
bindir     ?= $(prefix)/bin
datadir    ?= $(prefix)/share
sysconfdir ?= $(prefix)/etc
mandir     ?= $(datadir)/man
VERSION  := $(shell cat VERSION 2>/dev/null || echo dev)
INC      := -Isrc -Isrc/modem -Isrc/radio -Isrc/util
DEFS     := -DSIGNUM_VERSION='"$(VERSION)"' -DSIGNUM_DATADIR='"$(datadir)/signum"' $(if $(filter yes,$(CRYPTO)),-DSIGNUM_CRYPTO,-DSIGNUM_NO_CRYPTO)
# The front ends (src/web/, the front-end side's): every .cpp there, else the stub.
WEB      := $(wildcard src/web/*.cpp)
FRONT    := $(if $(WEB),$(WEB),src/frontend_none.cpp)
# Objects (make -j builds them in parallel): the modem's own .cpp files (no DEFS, so a version change
# does not rebuild them), and the program's (with DEFS). Each object records the headers it read (-MMD -MP: build/**/*.d), so a header change rebuilds
# only what includes it; a change of compiler, flags or DEFS rebuilds everything (the .flags files).
DEP       = -MMD -MP
MODEM_OBJ  := $(patsubst src/%.cpp,build/obj/modem/%.o,$(wildcard src/modem/signum/*.cpp src/radio/*.cpp))
CRYPTO_OBJ := $(patsubst src/%.cpp,build/obj/%.o,$(wildcard src/crypto/*.cpp))
APP_OBJ    := $(patsubst src/%.cpp,build/obj/%.o,$(filter-out src/frontend_none.cpp,$(wildcard src/*.cpp)) $(FRONT)) \
              $(if $(filter yes,$(CRYPTO)),$(CRYPTO_OBJ))
# Real-time priority for whoever runs the binary: the file capabilities CAP_SYS_NICE (SCHED_FIFO for the
# audio loop and decoders) and CAP_IPC_LOCK (mlockall). Set as root, else through sudo when it needs no
# password; otherwise one line says how. NO_SETCAP=1 skips it (packaging, or not wanted).
CAPS     := cap_sys_nice,cap_ipc_lock+ep
define setcap
	@if [ -n "$(NO_SETCAP)" ]; then :; \
	elif ! command -v setcap >/dev/null 2>&1 && [ ! -x /usr/sbin/setcap ] && [ ! -x /sbin/setcap ]; then \
	  echo "note: setcap not found (libcap tools): $(1) runs without real-time priority"; \
	elif [ "$$(id -u)" = 0 ]; then PATH="$$PATH:/usr/sbin:/sbin" setcap $(CAPS) $(1); \
	elif sudo -n true 2>/dev/null; then sudo -n env PATH="$$PATH:/usr/sbin:/sbin" setcap $(CAPS) $(1); \
	else echo "note: for real-time priority run: sudo setcap $(CAPS) $(1)  (or: make rt)"; fi
endef

all: build/signum caps

# The capabilities, whenever the binary lacks them (a fresh link clears them)
caps: build/signum
	@if PATH="$$PATH:/usr/sbin:/sbin" getcap build/signum 2>/dev/null | grep -q cap_sys_nice; then :; else $(MAKE) -s --no-print-directory setcap-build; fi
setcap-build:
	$(call setcap,build/signum)

build/signum: $(APP_OBJ) $(MODEM_OBJ)
	$(CXX) $(CXXFLAGS) $(APP_OBJ) $(MODEM_OBJ) $(LDFLAGS) -o $@ $(LIBS)

build/obj/modem/%.o: src/%.cpp build/obj/modem.flags
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INC) $(DEP) -c $< -o $@
build/obj/%.o: src/%.cpp build/obj/app.flags
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INC) $(DEFS) $(DEP) -c $< -o $@

# The flags each set of objects was built with: rewritten (so its objects rebuild) only when they change
MODEM_FLAGS := $(CXX) $(CXXFLAGS) $(INC)
APP_FLAGS   := $(CXX) $(CXXFLAGS) $(INC) $(DEFS)
build/obj/modem.flags: FORCE
	@mkdir -p $(@D); [ "$$(cat $@ 2>/dev/null)" = '$(subst ','\'',$(MODEM_FLAGS))' ] || echo '$(subst ','\'',$(MODEM_FLAGS))' > $@
build/obj/app.flags: FORCE
	@mkdir -p $(@D); [ "$$(cat $@ 2>/dev/null)" = '$(subst ','\'',$(APP_FLAGS))' ] || echo '$(subst ','\'',$(APP_FLAGS))' > $@
-include $(shell find build -name '*.d' 2>/dev/null)

# The capabilities on the build tree's binary, asking for the sudo password if needed
rt: build/signum
	sudo env PATH="$$PATH:/usr/sbin:/sbin" setcap $(CAPS) build/signum
	@getcap build/signum 2>/dev/null || /usr/sbin/getcap build/signum

# Documentation in docs/, checked in (needs pandoc, LuaLaTeX and groff: docs/pdf/README.md): the user guide
# (every page in docs/md/, in the order of its index) and the front ends' WebSocket API as PDF, the man page
# as PDF and text
GUIDE_PAGES := docs/md/README.md $(shell sed -n 's/^| \[[^]]*\](\([a-z0-9-]*\.md\)).*/docs\/md\/\1/p' docs/md/README.md)
DOC_DEPS    := $(wildcard docs/pdf/*) $(wildcard docs/images/*) VERSION
docs: docs/signum-user-guide.pdf docs/signum-websocket-api.pdf docs/signum.1.pdf docs/signum.1.txt
docs/signum-user-guide.pdf: $(GUIDE_PAGES) $(DOC_DEPS)
	docs/pdf/build-pdf.sh -o $@ --title "Signum user guide" $(GUIDE_PAGES)
docs/signum-websocket-api.pdf: src/web/README.md $(DOC_DEPS)
	docs/pdf/build-pdf.sh -o $@ --title "Signum front ends and WebSocket API" src/web/README.md
docs/signum.1.pdf: dist/signum.1
	@mkdir -p $(@D)
	groff -man -Tpdf $< > $@.tmp && mv $@.tmp $@
docs/signum.1.txt: dist/signum.1
	@mkdir -p $(@D)
	groff -man -Tutf8 -P-cbou $< > $@.tmp && mv $@.tmp $@

clean:
	rm -rf build

install: build/signum
	install -d $(DESTDIR)$(bindir) $(DESTDIR)$(datadir)/signum/www $(DESTDIR)$(datadir)/signum/tui \
	  $(DESTDIR)$(datadir)/signum/examples $(DESTDIR)$(sysconfdir)/signum $(DESTDIR)$(mandir)/man1 \
	  $(DESTDIR)$(datadir)/doc/signum/docs/md $(DESTDIR)$(datadir)/doc/signum/docs/images
	install -m 755 build/signum $(DESTDIR)$(bindir)/signum
	$(if $(DESTDIR),@echo "note: DESTDIR set: the package should run setcap $(CAPS) on $(bindir)/signum",$(call setcap,$(DESTDIR)$(bindir)/signum))
	for s in signum-start signum-loopback; do \
	  sed 's|^datadir=@DATADIR@|datadir=$(datadir)/signum|' $$s > $(DESTDIR)$(bindir)/$$s && chmod 755 $(DESTDIR)$(bindir)/$$s; done
	cp -R www/. $(DESTDIR)$(datadir)/signum/www/
	install -m 755 tui/signum-tui tui/ws-shell-example.py $(DESTDIR)$(datadir)/signum/tui/
	install -m 644 dist/calibration.conf $(DESTDIR)$(sysconfdir)/signum/calibration.conf
	install -m 644 dist/signum.service $(DESTDIR)$(datadir)/signum/examples/signum.service
	install -m 644 dist/signum-cpufreq.service $(DESTDIR)$(datadir)/signum/examples/signum-cpufreq.service
	install -m 644 dist/rigctld-signum.service dist/pat-signum.service $(DESTDIR)$(datadir)/signum/examples/
	install -m 644 dist/signum.1 $(DESTDIR)$(mandir)/man1/signum.1
	install -m 644 COPYING README.md $(DESTDIR)$(datadir)/doc/signum/
	install -m 644 docs/*.pdf docs/signum.1.txt $(DESTDIR)$(datadir)/doc/signum/docs/
	install -m 644 docs/md/*.md $(DESTDIR)$(datadir)/doc/signum/docs/md/
	install -m 644 docs/images/*.png $(DESTDIR)$(datadir)/doc/signum/docs/images/

uninstall:
	rm -f $(DESTDIR)$(bindir)/signum $(DESTDIR)$(bindir)/signum-start $(DESTDIR)$(bindir)/signum-loopback
	rm -rf $(DESTDIR)$(datadir)/signum $(DESTDIR)$(datadir)/doc/signum
	rm -f $(DESTDIR)$(sysconfdir)/signum/calibration.conf $(DESTDIR)$(mandir)/man1/signum.1
	-rmdir $(DESTDIR)$(sysconfdir)/signum $(DESTDIR)$(mandir)/man1 $(DESTDIR)$(mandir) 2>/dev/null
	-rmdir -p $(DESTDIR)$(bindir) $(DESTDIR)$(datadir) $(DESTDIR)$(sysconfdir) 2>/dev/null; true

distclean: clean
	rm -f config.mk

.PHONY: all caps setcap-build rt docs clean install uninstall distclean FORCE
FORCE:

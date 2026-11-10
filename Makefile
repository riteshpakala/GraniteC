# GraniteC: Granite's agent kernel and the granite-agent/1 standard in C. Packages sit
# side by side so each reads against its Swift and Rust twins (PORTING.md maps every file).
# Plain GNU make + pkg-config, MaryOS's shape; make 3.81 (macOS) and 4.x alike.
#
#   make                 libgranitec.a (core + agents), and the granite command
#   make test            build and run <pkg>/tests/test_*.c, with the fixture runners
#   make install         PREFIX=/usr/local DESTDIR=…: the library, headers, granitec.pc, granite, the schemas
#   make uninstall       what install put there
#   make install-check   install into a scratch prefix and build the fixture agent against it
#   make lint            the rules a compiler cannot check (see the lint target)
#   make drift-check     Schema/ against a Granite checkout (GRANITE_DIR=../Granite)
#   make interop         this build against GraniteRust's (RUST_DIR=../GraniteRust)
#
# Knobs: WERROR=1 makes warnings errors; SAN=address,undefined or SAN=thread builds with a
# sanitizer, into its own folder. Every output goes under $(O), so the source tree may be a
# read-only mount. The flags GraniteC needs live in their own variables: a CFLAGS given on
# the command line adds to them and cannot drop them.
comma := ,
O ?= build$(if $(SAN),-$(subst $(comma),-,$(SAN)))
PREFIX ?= /usr/local
DESTDIR ?=
AR ?= ar
PKG_CONFIG ?= pkg-config
VERSION := 0.1.0
CLI_NAME ?= granite
GRANITE_COMMIT ?= $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
UNAME := $(shell uname -s)

CFLAGS ?= -O2 -g
GRANITE_WARN := -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
ifeq ($(WERROR),1)
GRANITE_WARN += -Werror
endif
GRANITE_SAN := $(if $(SAN),-fsanitize=$(SAN) -fno-omit-frame-pointer)
GRANITE_CFLAGS := -std=c11 $(GRANITE_WARN) -pthread -fPIC $(GRANITE_SAN)
GRANITE_CPPFLAGS := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DGRANITE_VERSION=\"$(VERSION)\"
# Darwin hides wait4, posix_spawn's flags and pthread_setname_np under _POSIX_C_SOURCE alone.
ifeq ($(UNAME),Darwin)
GRANITE_CPPFLAGS += -D_DARWIN_C_SOURCE
endif

# json-c is the one library GraniteC uses: 0.15 or later (Debian bookworm has 0.16).
HAVE_JSONC := $(shell $(PKG_CONFIG) --exists 'json-c >= 0.15' 2>/dev/null && echo 1)
JSONC_CFLAGS := $(shell $(PKG_CONFIG) --cflags json-c 2>/dev/null)
JSONC_LIBS := $(shell $(PKG_CONFIG) --libs json-c 2>/dev/null)

# core + agents are what an author links (libgranitec.a). host + jsonschema are the granite
# command's own (libgranitec-host.a, never installed).
LIB_PKGS := core agents
HOST_PKGS := host jsonschema
PKGS := $(LIB_PKGS) $(HOST_PKGS) cli

INCLUDES := $(foreach p,$(LIB_PKGS) $(HOST_PKGS),-I$(p)/include) -Itesting
COMPILE = $(CC) $(GRANITE_CPPFLAGS) $(CPPFLAGS) $(GRANITE_CFLAGS) $(CFLAGS) $(INCLUDES) $(JSONC_CFLAGS)
LINK_LIBS = $(JSONC_LIBS) -lm -pthread $(GRANITE_SAN) $(LDFLAGS)

# strip: foreach joins empty results with spaces, and make 3.81's $(if) does not strip them.
LIB_SRCS := $(strip $(foreach p,$(LIB_PKGS),$(wildcard $(p)/src/*.c)))
HOST_SRCS := $(strip $(foreach p,$(HOST_PKGS),$(wildcard $(p)/src/*.c)))
LIB := $(if $(LIB_SRCS),$(O)/lib/libgranitec.a)
HOST_LIB := $(if $(HOST_SRCS),$(O)/lib/libgranitec-host.a)
CLI := $(if $(wildcard cli/bin/granite.c),$(O)/bin/$(CLI_NAME))

# Test-only runners (tests/fixtures/*_runner.c): built with the tests, never installed.
FIXTURES := $(patsubst tests/fixtures/%.c,$(O)/fixtures/%,$(wildcard tests/fixtures/*_runner.c))

tests_of = $(patsubst $(1)/tests/%.c,$(O)/tests/$(1)/%,$(wildcard $(1)/tests/test_*.c))
ALL_TESTS := $(strip $(foreach p,$(PKGS),$(call tests_of,$(p))))
# What each package's tests link: the author's library, and the host's beneath the command's.
test_libs_core := $(LIB)
test_libs_agents := $(LIB)
test_libs_host := $(HOST_LIB) $(LIB)
test_libs_jsonschema := $(HOST_LIB) $(LIB)
test_libs_cli := $(HOST_LIB) $(LIB)

.PHONY: all test install uninstall install-check lint drift-check interop clean check-deps
all: $(LIB) $(CLI)

$(O)/obj/%.o: %.c
ifneq ($(HAVE_JSONC),1)
	$(error json-c 0.15 or later not found: install libjson-c-dev (Debian) or json-c (Homebrew))
endif
	@mkdir -p $(@D)
	$(COMPILE) -MMD -MP -c $< -o $@

$(O)/lib/libgranitec.a: $(patsubst %.c,$(O)/obj/%.o,$(LIB_SRCS))
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

$(O)/lib/libgranitec-host.a: $(patsubst %.c,$(O)/obj/%.o,$(HOST_SRCS))
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

# The command reports the commit it was built from; only its own object carries it.
$(O)/obj/cli/bin/granite.o: GRANITE_CPPFLAGS += -DGRANITE_COMMIT=\"$(GRANITE_COMMIT)\" -DGRANITE_CLI_NAME=\"$(CLI_NAME)\"
$(O)/bin/$(CLI_NAME): $(O)/obj/cli/bin/granite.o $(HOST_LIB) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(GRANITE_CFLAGS) $(CFLAGS) -o $@ $^ $(LINK_LIBS)

$(O)/fixtures/%: $(O)/obj/tests/fixtures/%.o $(LIB)
	@mkdir -p $(@D)
	$(CC) $(GRANITE_CFLAGS) $(CFLAGS) -o $@ $^ $(LINK_LIBS)

define package_tests
$$(O)/tests/$(1)/%: $$(O)/obj/$(1)/tests/%.o $$(test_libs_$(1))
	@mkdir -p $$(@D)
	$$(CC) $$(GRANITE_CFLAGS) $$(CFLAGS) -o $$@ $$^ $$(LINK_LIBS)
endef
$(foreach p,$(PKGS),$(eval $(call package_tests,$(p))))

# granitec as pkg-config sees it in this build tree: pkg-config prefers granitec-uninstalled.pc
# on its path, so an agent repository builds against this tree exactly as against an install.
UNINSTALLED_PC := $(O)/pkgconfig/granitec-uninstalled.pc
$(UNINSTALLED_PC): Makefile
	@mkdir -p $(@D)
	@printf '%s\n' 'Name: granitec' 'Description: GraniteC, in its build tree' 'Version: $(VERSION)' \
	  'commit=$(GRANITE_COMMIT)' 'Requires: json-c >= 0.15' \
	  'Cflags: -I$(CURDIR)/core/include -I$(CURDIR)/agents/include -pthread $(GRANITE_SAN)' \
	  'Libs: $(abspath $(O))/lib/libgranitec.a -pthread -lm $(GRANITE_SAN)' > $@

# Each test runs from the source root with where things are: GRANITE_SRC (fixtures, schemas),
# GRANITE_BUILD (the runners and the command this build made), and this tree on PKG_CONFIG_PATH.
test: $(ALL_TESTS) $(FIXTURES) $(CLI) $(UNINSTALLED_PC)
	@fail=0; for t in $(ALL_TESTS); do echo "== $$t"; \
	  GRANITE_SRC="$(CURDIR)" GRANITE_BUILD="$(abspath $(O))" GRANITE_CLI="$(abspath $(O))/bin/$(CLI_NAME)" \
	  PKG_CONFIG_PATH="$(abspath $(O))/pkgconfig$${PKG_CONFIG_PATH:+:$$PKG_CONFIG_PATH}" $$t || fail=1; \
	done; [ $$fail = 0 ] && echo "granitec: every test passed"

# What an author links and includes, and where install puts it. runs.h and every host and
# jsonschema header stay the command's own.
PUBLIC_HEADERS := $(wildcard core/include/granitec/*.h agents/include/granitec/*.h)
SHARED_SCHEMAS := $(wildcard Schema/*.json containers/schemas/*.json)
LIBDIR := $(DESTDIR)$(PREFIX)/lib
INCLUDEDIR := $(DESTDIR)$(PREFIX)/include/granitec
BINDIR := $(DESTDIR)$(PREFIX)/bin
SCHEMADIR := $(DESTDIR)$(PREFIX)/share/granitec/schemas
DOCDIR := $(DESTDIR)$(PREFIX)/share/doc/granitec

# granitec.pc is written by install itself, for the PREFIX and commit of that install: a file kept
# in the build tree would keep the PREFIX of whichever install made it first.
install: all
	install -d $(LIBDIR)/pkgconfig $(INCLUDEDIR) $(BINDIR) $(SCHEMADIR) $(DOCDIR)
	install -m 644 $(LIB) $(LIBDIR)/
	install -m 644 $(PUBLIC_HEADERS) $(INCLUDEDIR)/
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@VERSION@|$(VERSION)|' -e 's|@COMMIT@|$(GRANITE_COMMIT)|' \
	  granitec.pc.in > $(LIBDIR)/pkgconfig/granitec.pc
	chmod 644 $(LIBDIR)/pkgconfig/granitec.pc
	install -m 755 $(CLI) $(BINDIR)/
	install -m 644 $(SHARED_SCHEMAS) $(SCHEMADIR)/
	install -m 644 README.md PORTING.md Schema/GraniteAgent.md $(DOCDIR)/

uninstall:
	rm -f $(LIBDIR)/libgranitec.a $(LIBDIR)/pkgconfig/granitec.pc $(BINDIR)/$(CLI_NAME)
	rm -f $(foreach h,$(PUBLIC_HEADERS),$(INCLUDEDIR)/$(notdir $(h)))
	rm -f $(foreach s,$(SHARED_SCHEMAS),$(SCHEMADIR)/$(notdir $(s)))
	rm -f $(DOCDIR)/README.md $(DOCDIR)/PORTING.md $(DOCDIR)/GraniteAgent.md
	-rmdir $(INCLUDEDIR) $(SCHEMADIR) $(DESTDIR)$(PREFIX)/share/granitec $(DOCDIR) 2>/dev/null

install-check: all $(FIXTURES)
	@sh scripts/install-check.sh

# What no compiler checks. Only the lock takes a mutex: everything else goes through
# granite_lock, which counts what a thread holds. Only json.c frees a json-c tree: everything
# else calls granite_json_put, which refuses under a lock.
lint:
	@bad=$$(grep -l 'pthread_mutex_lock' /dev/null $$(find core agents host jsonschema cli -name '*.c' 2>/dev/null) 2>/dev/null | grep -v '^core/src/lock\.c$$'); \
	  if [ -n "$$bad" ]; then echo "lint: pthread_mutex_lock outside core/src/lock.c: $$bad"; exit 1; fi
	@bad=$$(grep -l 'json_object_put(' /dev/null $$(find core agents host jsonschema cli tests -name '*.c' 2>/dev/null) 2>/dev/null | grep -v '^agents/src/json\.c$$'); \
	  if [ -n "$$bad" ]; then echo "lint: json_object_put outside agents/src/json.c: $$bad"; exit 1; fi
	@echo "lint: clean"

# Schema/ is a verbatim copy of Granite's; drift-check says when it has fallen behind.
GRANITE_DIR ?= ../Granite
SCHEMA_FILES := GraniteAgent.md granite-agent-answer.schema.json granite-agent-entry.schema.json \
	granite-agent-installation.schema.json granite-agent-wire.schema.json
drift-check:
	@[ -d "$(GRANITE_DIR)/Schema" ] || { echo "drift-check: no Granite checkout at $(GRANITE_DIR)"; exit 1; }
	@for f in $(SCHEMA_FILES); do cmp -s "Schema/$$f" "$(GRANITE_DIR)/Schema/$$f" \
	  || { echo "drift-check: Schema/$$f differs from $(GRANITE_DIR)/Schema"; exit 1; }; done
	@echo "drift-check: Schema/ matches $(GRANITE_DIR)/Schema"

RUST_DIR ?= ../GraniteRust
interop: all $(FIXTURES) $(UNINSTALLED_PC)
	@sh scripts/interop.sh "$(abspath $(O))" "$(RUST_DIR)"

check-deps:
	@$(PKG_CONFIG) --print-errors --exists 'json-c >= 0.15' && echo "json-c $$($(PKG_CONFIG) --modversion json-c) ok"

clean:
	rm -rf $(O)

-include $(shell find $(O)/obj -name '*.d' 2>/dev/null)

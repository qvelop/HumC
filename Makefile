SHELL := /bin/sh

CC ?= cc
BUILD_DIR ?= build
HUMC_BIN := $(BUILD_DIR)/humc
SANITIZE_DIR ?= build-sanitize
SANITIZE_BIN := $(SANITIZE_DIR)/humc
PREFIX ?= $(HOME)/.local

CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=

SOURCES := bin/humc.c src/compiler.c
HEADERS := src/compiler.h
SMOKE_TEST := $(firstword $(wildcard test/test.sh test/test.sh))

.PHONY: all help test tests run install uninstall clean clean-all sanitize sanitize-test

all: $(HUMC_BIN)

help:
	@printf '%s\n' \
	  'HumC Make targets:' \
	  '  make                 Build build/humc' \
	  '  make test            Run tests' \
	  '  make run FILE=...    Translate, compile, and run a .humc file' \
	  '  make sanitize        Build with AddressSanitizer and UBSan' \
	  '  make sanitize-test   Run tests with sanitizers' \
	  '  make install         Install to PREFIX (default: ~/.local)' \
	  '  make uninstall       Remove installed humc' \
	  '  make clean           Remove generated compiler binaries' \
	  '  make clean-all       Remove build directories'

$(HUMC_BIN): $(SOURCES) $(HEADERS)
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o "$@" $(LDFLAGS) $(LDLIBS)

test: tests

tests: $(HUMC_BIN)
	@if [ -n "$(SMOKE_TEST)" ]; then \
	  CC="$(CC)" \
	  HUMC_BIN="$(abspath $(HUMC_BIN))" \
	  HUMC_SOURCE_DIR="$(CURDIR)" \
	    sh "$(CURDIR)/$(SMOKE_TEST)"; \
	else \
	  echo 'No smoke-test script found (expected test/test.sh or tests-smoke.sh).' >&2; \
	  exit 1; \
	fi

run: $(HUMC_BIN)
	@if [ -z "$(FILE)" ]; then \
	  echo 'Usage: make run FILE=examples/basic.humc [ARGS="..."]' >&2; \
	  exit 2; \
	fi
	"$(HUMC_BIN)" run "$(FILE)" $(ARGS)

$(SANITIZE_BIN): $(SOURCES) $(HEADERS)
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Wpedantic \
	  -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(SOURCES) -o "$@" -fsanitize=address,undefined $(LDLIBS)

sanitize: $(SANITIZE_BIN)

sanitize-test: $(SANITIZE_BIN)
	@if [ -n "$(SMOKE_TEST)" ]; then \
	  CC="$(CC)" \
	  HUMC_BIN="$(abspath $(SANITIZE_BIN))" \
	  HUMC_SOURCE_DIR="$(CURDIR)" \
	    sh "$(CURDIR)/$(SMOKE_TEST)"; \
	else \
	  echo 'No smoke-test script found.' >&2; \
	  exit 1; \
	fi

install: $(HUMC_BIN)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 "$(HUMC_BIN)" "$(DESTDIR)$(PREFIX)/bin/humc"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/humc"

clean:
	rm -f "$(HUMC_BIN)" "$(SANITIZE_BIN)" ./humc ./humalc

clean-all:
	rm -rf "$(BUILD_DIR)" "$(SANITIZE_DIR)"
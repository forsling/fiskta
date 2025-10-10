# Makefile for fiskta
VERSION ?= $(shell cat VERSION 2>/dev/null || echo "dev")

CC = cc
CFLAGS = -std=c11 -O3 -Wall -Wextra -Wconversion -Wshadow
CPPFLAGS = -DFISKTA_VERSION=\"$(VERSION)\" -D_POSIX_C_SOURCE=199309L
TARGET = fiskta
SRCDIR = src
BUILDDIR = build
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/parse.c $(SRCDIR)/engine.c $(SRCDIR)/iosearch.c $(SRCDIR)/reprog.c $(SRCDIR)/util.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=
INSTALL ?= install
INSTALL_PROGRAM ?= $(INSTALL) -m 0755

.PHONY: all clean test test-full debug install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) $(TARGET)

test: $(TARGET)
	@echo "Running comprehensive test suite..."
	python3 tests/run_tests.py

debug: CFLAGS = -std=c11 -g -O0 -Wall -Wextra -Wconversion -Wshadow -DDEBUG
debug: $(TARGET)

install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

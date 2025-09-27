# Makefile for fiskta v2.2
CC = cc
CFLAGS = -std=c11 -O3 -Wall -Wextra -Wconversion -Wshadow
TARGET = fiskta
SRCDIR = src
BUILDDIR = build
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/parse.c $(SRCDIR)/engine.c $(SRCDIR)/iosearch.c $(SRCDIR)/reprog.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

.PHONY: all clean test test-full debug

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

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
	cp $(TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)

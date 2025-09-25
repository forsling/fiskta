# Makefile for fiskta v2.2
CC = cc
CFLAGS = -std=c11 -O3 -Wall -Wextra -Wconversion -Wshadow
TARGET = fiskta
SRCDIR = src
BUILDDIR = build
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/parse.c $(SRCDIR)/engine.c $(SRCDIR)/iosearch.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

.PHONY: all clean test

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
	@echo "Testing basic functionality..."
	@echo "Hello World" | ./$(TARGET) take 11b -
	@echo "Testing find operation..."
	./$(TARGET) find "SEARCH" tests/search.txt
	@echo "Testing take until..."
	./$(TARGET) take until "SEARCH" tests/search.txt
	@echo "Testing multi-clause..."
	./$(TARGET) take 5b ";" take 5b tests/basic.txt
	@echo "All tests passed!"

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)

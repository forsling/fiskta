# Makefile for fiskta v2.2
CC = cc
CFLAGS = -std=c11 -O3 -Wall -Wextra -Wconversion -Wshadow
TARGET = fiskta
SRCDIR = src
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/parse.c $(SRCDIR)/engine.c $(SRCDIR)/iosearch.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

test: $(TARGET)
	@echo "Testing basic functionality..."
	@echo "Hello World" | ./$(TARGET) 'take +11b' -
	@echo "Testing find operation..."
	@echo "Line 1\nLine 2\nLine 3" | ./$(TARGET) 'find "Line 2"; take to match-end' -

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)

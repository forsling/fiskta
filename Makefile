# fiskta Makefile — alternative to zig build for those who prefer make.
# Cross-compilation is not supported; use `zig build release` for that.
#
# Targets:
#   make              Build optimized binary
#   make debug        Build debug binary (-O0 -g)
#   make release      Build stripped binary + static library + headers in dist/
#   make test         Run test suite
#   make fuzz         Run fuzzer (100 cases)
#   make clean        Remove build artifacts
#
# Variables:
#   CC=gcc            Override compiler (default: cc)
#   PREFIX=/usr/local Install prefix for headers/libs

CC       ?= cc
PREFIX   ?= /usr/local
VERSION  := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION  := $(patsubst v%,%,$(VERSION))

SRCS_LIB := src/parse.c src/fiskta.c src/engine.c src/fileio.c \
             src/search_literal.c src/regex_vm.c src/regex_prog.c src/util.c
SRCS_CLI := src/main.c $(SRCS_LIB)
OBJS_LIB := $(SRCS_LIB:.c=.o)
HEADERS  := src/fiskta.h src/fiskta_types.h

CFLAGS_COMMON := -std=c11 -Wall -Werror -Wextra -Wconversion -Wshadow \
                 -Wcast-qual -Wpointer-arith -Wbad-function-cast -Wundef \
                 -pedantic -Wcast-align -Wmissing-declarations -Wwrite-strings \
                 -Wstrict-aliasing=2 -D_POSIX_C_SOURCE=199309L \
                 -DFISKTA_VERSION=\"$(VERSION)\"

CFLAGS_OPT   := $(CFLAGS_COMMON) -O3
CFLAGS_DEBUG := $(CFLAGS_COMMON) -O0 -g -DDEBUG
CFLAGS_REL   := $(CFLAGS_COMMON) -O3 -ffunction-sections -fdata-sections \
                -fomit-frame-pointer -fno-stack-protector \
                -fno-unwind-tables -fno-asynchronous-unwind-tables

# Detect OS for platform-specific flags
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS_REL := -Wl,-dead_strip
    LIB_SHARED  := libfiskta.dylib
else
    LDFLAGS_REL := -Wl,--gc-sections
    LIB_SHARED  := libfiskta.so
endif

.PHONY: all debug release test fuzz clean

all: fiskta

fiskta: $(SRCS_CLI)
	$(CC) $(CFLAGS_OPT) $^ -o $@

debug: CFLAGS_OPT := $(CFLAGS_DEBUG)
debug: fiskta

# Release: stripped binary + static library + headers in dist/
release: dist/bin/fiskta dist/lib/libfiskta.a dist/include/fiskta.h
	@echo ""
	@echo "Release artifacts in dist/:"
	@ls -lh dist/bin/fiskta dist/lib/libfiskta.a
	@echo ""
	@SIZE=$$(stat -c%s dist/bin/fiskta 2>/dev/null || stat -f%z dist/bin/fiskta 2>/dev/null); \
	 if [ "$$SIZE" -gt 153600 ]; then \
	     echo "WARNING: binary is $$(( SIZE / 1024 ))K (target: <150K)"; \
	 else \
	     echo "Binary size: $$(( SIZE / 1024 ))K (OK)"; \
	 fi

dist/bin/fiskta: $(SRCS_CLI) | dist/bin
	$(CC) $(CFLAGS_REL) $(LDFLAGS_REL) -s $^ -o $@

dist/lib/libfiskta.a: $(OBJS_LIB) | dist/lib
	$(AR) rcs $@ $^
	@rm -f $(OBJS_LIB)

# Compile object files for static library
%.o: %.c
	$(CC) $(CFLAGS_REL) -fPIC -c $< -o $@

dist/include/fiskta.h: $(HEADERS) | dist/include
	cp $(HEADERS) dist/include/

dist/bin dist/lib dist/include:
	mkdir -p $@

test: fiskta
	python3 tools/test.py --exe ./fiskta | grep -v '\[PASS\]'

fuzz: fiskta
	python3 tools/fuzz.py --fiskta-path ./fiskta --cases 100

clean:
	rm -f fiskta $(OBJS_LIB)
	rm -rf dist/

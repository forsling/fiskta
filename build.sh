#!/bin/bash
set -e

VERSION=$(cat VERSION 2>/dev/null || echo "dev")
CC=${CC:-cc}

DEBUG=0
if [ "$1" = "--debug" ] || [ "$1" = "-g" ]; then
    DEBUG=1
fi

CFLAGS="-std=c11 -Wall -Wextra -Wconversion -Wshadow -Wcast-qual -Wpointer-arith -Wbad-function-cast -Wundef"
if [ $DEBUG -eq 1 ]; then
    CFLAGS="$CFLAGS -g -O0 -DDEBUG"
else
    CFLAGS="$CFLAGS -O3"
fi

echo "Building fiskta $VERSION..."
$CC $CFLAGS -DFISKTA_VERSION=\"$VERSION\" -D_POSIX_C_SOURCE=199309L \
    src/main.c src/cli_help.c src/parse.c src/engine.c src/iosearch.c src/reprog.c src/util.c \
    -o fiskta

echo "Built ./fiskta"

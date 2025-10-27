#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fileio.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> // for off_t

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef FISKTA_STDIN_SPOOL
#define FISKTA_STDIN_SPOOL (256 * 1024)
#endif

/**********************
 * FILE I/O OPERATIONS
 **********************/

enum Err io_open(File* io, const char* path,
    unsigned char* search_buf, size_t search_buf_cap)
{
    memset(io, 0, sizeof(*io));

    if (strcmp(path, "-") == 0) {
        // Spool stdin to temp file
        io->f = tmpfile();
        if (!io->f) {
            return E_IO;
        }

#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif

        // Copy stdin to temp file in chunks
        unsigned char buf[FISKTA_STDIN_SPOOL];
        // size will be determined with ftello after writing

        while (1) {
            size_t n = fread(buf, 1, sizeof(buf), stdin);
            if (n == 0) {
                if (ferror(stdin)) {
                    fclose(io->f);
                    return E_IO;
                }
                break;
            }

            size_t written = fwrite(buf, 1, n, io->f);
            if (written != n) {
                fclose(io->f);
                return E_IO;
            }
        }

        if (fflush(io->f) != 0) {
            fclose(io->f);
            return E_IO;
        }
        if (fseeko(io->f, 0, SEEK_END) != 0) {
            fclose(io->f);
            return E_IO;
        }
        off_t sz = ftello(io->f);
        if (sz < 0) {
            fclose(io->f);
            return E_IO;
        }
        io->size = (i64)sz;
        if (fseek(io->f, 0, SEEK_SET) != 0) {
            fclose(io->f);
            return E_IO;
        }
    } else {
        io->f = fopen(path, "rb");
        if (!io->f) {
            return E_IO;
        }

        if (fseeko(io->f, 0, SEEK_END) != 0) {
            fclose(io->f);
            return E_IO;
        }
        off_t sz = ftello(io->f);
        if (sz < 0) {
            fclose(io->f);
            return E_IO;
        }
        io->size = (i64)sz;
        if (fseek(io->f, 0, SEEK_SET) != 0) {
            fclose(io->f);
            return E_IO;
        }
    }

    // Wire arena-backed buffers
    io->buf = search_buf;
    io->buf_cap = search_buf_cap;

    // Initialize LRU line index cache (counts are embedded)
    io->line_idx_gen = 0;
    for (i32 i = 0; i < IDX_MAX_BLOCKS; ++i) {
        io->line_idx[i].in_use = false;
        io->line_idx[i].gen = 0;
        io->line_idx[i].sub_count = 0;
    }

    return E_OK;
}

void io_close(File* io)
{
    if (io->f) {
        fclose(io->f);
        io->f = NULL;
    }

    io->size = 0;
    io->buf_cap = 0;
}

// Full reset of File state for clause execution
void io_reset_full(File* io)
{
    if (!io) {
        return;
    }
    if (io->f) {
        // 64-bit safe seek to BOF
        if (fseeko(io->f, 0, SEEK_SET) != 0) {
            // Best effort: clear any error state
            clearerr(io->f);
        }
    }

    // Reset all line index cache but preserve arena-owned slabs
    io->line_idx_gen = 0;
    for (int i = 0; i < IDX_MAX_BLOCKS; i++) {
        io->line_idx[i].in_use = false;
        io->line_idx[i].gen = 0;
        io->line_idx[i].block_lo = 0;
        io->line_idx[i].block_hi = 0;
        io->line_idx[i].sub_count = 0;
        // Don't reset lf_counts array - it's embedded in the struct
    }
}

enum Err io_emit(File* io, i64 start, i64 end, FILE* out)
{
    if (start >= end) {
        return E_OK;
    }
    if (start < 0 || end > io->size) {
        return E_IO; // outside file is an error
    }

    if (fseeko(io->f, start, SEEK_SET) != 0) {
        return E_IO;
    }

    i64 remaining = end - start;
    while (remaining > 0) {
        size_t chunk_size = (remaining > (i64)io->buf_cap) ? io->buf_cap : (size_t)remaining;
        size_t n = fread(io->buf, 1, chunk_size, io->f);
        if (n == 0) {
            if (ferror(io->f)) {
                return E_IO;
            }
            break; // shouldn't happen with bounded ranges, but be defensive
        }

        size_t written = fwrite(io->buf, 1, n, out);
        if (written != n) {
            return E_IO;
        }

        remaining -= (i64)n;
    }

    return E_OK;
}

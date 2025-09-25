// iosearch.c
#include "iosearch.h"
#include <stdlib.h>
#include <string.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// Forward declarations for Two-Way algorithm
static enum Err two_way_forward(const unsigned char *text, size_t text_len,
                               const unsigned char *needle, size_t needle_len,
                               i64 *ms, i64 *me);
static enum Err two_way_backward(const unsigned char *text, size_t text_len,
                                const unsigned char *needle, size_t needle_len,
                                i64 *ms, i64 *me);

enum Err io_open(File *io, const char *path) {
  memset(io, 0, sizeof(*io));
  
  if (strcmp(path, "-") == 0) {
    // Spool stdin to temp file
    io->f = tmpfile();
    if (!io->f) return E_IO;
    
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    
    // Copy stdin to temp file in chunks
    unsigned char buf[256 * 1024]; // 256 KiB chunks
    size_t total_read = 0;
    
    while (1) {
      size_t n = fread(buf, 1, sizeof(buf), stdin);
      if (n == 0) break;
      
      size_t written = fwrite(buf, 1, n, io->f);
      if (written != n) {
        fclose(io->f);
        return E_IO;
      }
      total_read += n;
    }
    
    io->size = total_read;
    rewind(io->f);
  } else {
    io->f = fopen(path, "rb");
    if (!io->f) return E_IO;
    
    // Get file size
    if (fseeko(io->f, 0, SEEK_END) != 0) {
      fclose(io->f);
      return E_IO;
    }
    io->size = ftello(io->f);
    if (io->size < 0) {
      fclose(io->f);
      return E_IO;
    }
    rewind(io->f);
  }
  
  // Allocate search buffer
  size_t buf_size = FW_WIN > (BK_BLK + OVERLAP_MAX) ? FW_WIN : (BK_BLK + OVERLAP_MAX);
  io->buf = malloc(buf_size);
  if (!io->buf) {
    fclose(io->f);
    return E_OOM;
  }
  io->buf_cap = buf_size;
  
  return E_OK;
}

void io_close(File *io) {
  if (io->f) {
    fclose(io->f);
    io->f = NULL;
  }
  if (io->buf) {
    free(io->buf);
    io->buf = NULL;
  }
  io->size = 0;
  io->buf_cap = 0;
}

enum Err io_emit(File *io, i64 start, i64 end, FILE *out) {
  if (start >= end || start < 0 || end > io->size) {
    return E_OK; // Empty or invalid range, nothing to emit
  }
  
  if (fseeko(io->f, start, SEEK_SET) != 0) {
    return E_IO;
  }
  
  i64 remaining = end - start;
  while (remaining > 0) {
    size_t chunk_size = remaining > io->buf_cap ? io->buf_cap : (size_t)remaining;
    size_t n = fread(io->buf, 1, chunk_size, io->f);
    if (n == 0) break;
    
    size_t written = fwrite(io->buf, 1, n, out);
    if (written != n) {
      return E_IO;
    }
    
    remaining -= n;
  }
  
  return E_OK;
}

enum Err io_line_start(File *io, i64 pos, i64 *out) {
  if (pos < 0 || pos > io->size) {
    return E_LOC_RESOLVE;
  }
  
  if (pos == 0) {
    *out = 0;
    return E_OK;
  }
  
  // Find last LF before pos
  i64 search_start = pos - 1;
  if (search_start > FW_WIN) {
    search_start = pos - FW_WIN;
  }
  
  if (fseeko(io->f, search_start, SEEK_SET) != 0) {
    return E_IO;
  }
  
  size_t read_size = pos - search_start;
  size_t n = fread(io->buf, 1, read_size, io->f);
  if (n == 0) {
    *out = 0;
    return E_OK;
  }
  
  // Search backwards for LF
  for (i64 i = n - 1; i >= 0; i--) {
    if (io->buf[i] == '\n') {
      *out = search_start + i + 1;
      return E_OK;
    }
  }
  
  *out = search_start;
  return E_OK;
}

enum Err io_line_end(File *io, i64 pos, i64 *out) {
  if (pos < 0 || pos > io->size) {
    return E_LOC_RESOLVE;
  }
  
  if (pos >= io->size) {
    *out = io->size;
    return E_OK;
  }
  
  // Find next LF at or after pos
  i64 search_end = pos + FW_WIN;
  if (search_end > io->size) {
    search_end = io->size;
  }
  
  if (fseeko(io->f, pos, SEEK_SET) != 0) {
    return E_IO;
  }
  
  size_t read_size = search_end - pos;
  size_t n = fread(io->buf, 1, read_size, io->f);
  if (n == 0) {
    *out = io->size;
    return E_OK;
  }
  
  // Search forwards for LF
  for (size_t i = 0; i < n; i++) {
    if (io->buf[i] == '\n') {
      *out = pos + i + 1;
      return E_OK;
    }
  }
  
  *out = io->size;
  return E_OK;
}

enum Err io_step_lines_from(File *io, i64 start_line_start, int delta, i64 *out_line_start) {
  if (start_line_start < 0 || start_line_start > io->size) {
    return E_LOC_RESOLVE;
  }
  
  i64 current = start_line_start;
  
  if (delta > 0) {
    // Move forward by delta lines
    for (int i = 0; i < delta; i++) {
      i64 line_end;
      enum Err err = io_line_end(io, current, &line_end);
      if (err != E_OK) return err;
      
      if (line_end >= io->size) {
        *out_line_start = io->size;
        return E_OK;
      }
      current = line_end;
    }
  } else if (delta < 0) {
    // Move backward by |delta| lines
    for (int i = 0; i < -delta; i++) {
      if (current == 0) {
        *out_line_start = 0;
        return E_OK;
      }
      
      i64 line_start;
      enum Err err = io_line_start(io, current - 1, &line_start);
      if (err != E_OK) return err;
      current = line_start;
    }
  }
  
  *out_line_start = current;
  return E_OK;
}

enum Err io_find_window(File *io, i64 win_lo, i64 win_hi,
                        const unsigned char *needle, size_t nlen,
                        enum Dir dir, i64 *ms, i64 *me) {
  if (nlen == 0) return E_BAD_NEEDLE;
  if (win_lo >= win_hi || win_lo < 0 || win_hi > io->size) {
    return E_NO_MATCH;
  }
  
  // Clamp window to file bounds
  win_lo = clamp64(win_lo, 0, io->size);
  win_hi = clamp64(win_hi, 0, io->size);
  
  if (win_lo >= win_hi) {
    return E_NO_MATCH;
  }
  
  if (dir == DIR_FWD) {
    // Forward search: read window and use Two-Way
    i64 window_size = win_hi - win_lo;
    if (window_size > io->buf_cap) {
      window_size = io->buf_cap;
    }
    
    if (fseeko(io->f, win_lo, SEEK_SET) != 0) {
      return E_IO;
    }
    
    size_t n = fread(io->buf, 1, window_size, io->f);
    if (n == 0) {
      return E_NO_MATCH;
    }
    
    i64 local_ms, local_me;
    enum Err err = two_way_forward(io->buf, n, needle, nlen, &local_ms, &local_me);
    if (err != E_OK) {
      return err;
    }
    
    *ms = win_lo + local_ms;
    *me = win_lo + local_me;
    return E_OK;
    
  } else {
    // Backward search: scan blocks backwards
    i64 best_ms = -1;
    i64 best_me = -1;
    
    // Calculate overlap
    size_t overlap = nlen > 0 ? nlen - 1 : 0;
    if (overlap < OVERLAP_MIN) overlap = OVERLAP_MIN;
    if (overlap > OVERLAP_MAX) overlap = OVERLAP_MAX;
    
    // Scan backwards in blocks
    for (i64 pos = win_hi; pos > win_lo; pos -= (BK_BLK - overlap)) {
      i64 block_hi = pos;
      i64 block_lo = block_hi - BK_BLK;
      if (block_lo < win_lo) block_lo = win_lo;
      
      i64 block_size = block_hi - block_lo;
      if (block_size <= 0) break;
      
      if (fseeko(io->f, block_lo, SEEK_SET) != 0) {
        return E_IO;
      }
      
      size_t n = fread(io->buf, 1, block_size, io->f);
      if (n == 0) break;
      
      // Find all matches in this block
      i64 search_pos = 0;
      while (search_pos < n) {
        i64 local_ms, local_me;
        enum Err err = two_way_forward(io->buf + search_pos, n - search_pos, 
                                     needle, nlen, &local_ms, &local_me);
        if (err != E_OK) break;
        
        i64 global_ms = block_lo + search_pos + local_ms;
        i64 global_me = block_lo + search_pos + local_me;
        
        // Check if match is within our window
        if (global_ms >= win_lo && global_me <= win_hi) {
          // Keep the rightmost match (largest ms)
          if (global_ms > best_ms) {
            best_ms = global_ms;
            best_me = global_me;
          }
        }
        
        search_pos += local_ms + 1; // Move past this match
      }
    }
    
    if (best_ms >= 0) {
      *ms = best_ms;
      *me = best_me;
      return E_OK;
    }
    
    return E_NO_MATCH;
  }
}

// Two-Way algorithm implementation (simplified)
static enum Err two_way_forward(const unsigned char *text, size_t text_len,
                               const unsigned char *needle, size_t needle_len,
                               i64 *ms, i64 *me) {
  if (needle_len == 0 || needle_len > text_len) {
    return E_NO_MATCH;
  }
  
  // Simple naive search implementation
  const unsigned char *found = NULL;
  
  // Naive search
  for (size_t i = 0; i <= text_len - needle_len; i++) {
    if (memcmp(text + i, needle, needle_len) == 0) {
      found = text + i;
      break;
    }
  }
  
  if (found) {
    *ms = found - text;
    *me = *ms + needle_len;
    return E_OK;
  }
  
  return E_NO_MATCH;
}

static enum Err two_way_backward(const unsigned char *text, size_t text_len,
                                const unsigned char *needle, size_t needle_len,
                                i64 *ms, i64 *me) {
  if (needle_len == 0 || needle_len > text_len) {
    return E_NO_MATCH;
  }
  
  // Find the rightmost occurrence
  i64 best_ms = -1;
  i64 best_me = -1;
  
  for (size_t i = 0; i <= text_len - needle_len; i++) {
    if (memcmp(text + i, needle, needle_len) == 0) {
      best_ms = i;
      best_me = i + needle_len;
    }
  }
  
  if (best_ms >= 0) {
    *ms = best_ms;
    *me = best_me;
    return E_OK;
  }
  
  return E_NO_MATCH;
}

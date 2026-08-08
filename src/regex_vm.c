#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "regex_vm.h"
#include "error.h"
#include "fiskta.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

/********************
 * REGEX NFA ENGINE *
 ********************/

typedef struct {
    ReThread* v;
    int n, cap;
} ReList;

static inline int cls_has(const ReClass* c, unsigned char ch)
{
    return (c->bits[ch >> 3] >> (ch & 7)) & 1;
}

static void rlist_init(ReList* l, ReThread* buf, int cap)
{
    l->v = buf;
    l->n = 0;
    l->cap = cap;
}
static inline void rlist_clear(ReList* l) { l->n = 0; }
static inline void seen_clear_bytes(unsigned char* seen, size_t bytes) { memset(seen, 0, bytes); }

// Number of 32-bit signature slots per pc in the seen table.
// 8 slots for robustness with nested quantifiers
#ifndef RE_SEEN_SLOTS
#define RE_SEEN_SLOTS 8
#endif

// Fast 32-bit signature over the counter array. Guarantees non-zero.
// Only hashes active counters (up to highest non-zero index) to reduce aliasing.
static inline u32 re_counters_sig(const int* cnt, int n)
{
    // Find highest non-zero counter
    int active_n = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (cnt[i] != 0) {
            active_n = i + 1;
            break;
        }
    }

    // FNV-1a with a tiny avalanche; counters are small ints (0..big)
    u32 h = 2166136261u;
    for (int i = 0; i < active_n; i++) {
        h ^= (u32)cnt[i];
        h *= 16777619u;
        // mix a bit to decorrelate low variance
        h ^= h >> 13;
        h *= 0x9E3779B1u;
    }
    if (h == 0)
        h = 1; // reserve 0 for "empty"
    return h;
}

// Probe up to RE_SEEN_SLOTS 32-bit entries for this pc. If found, return 1.
// If an empty slot (0), store and return 0. If full, do a deterministic replace.
static inline int re_seen_hit_or_set(unsigned char* seen, int pc, u32 sig)
{
    // Base pointer to this pc's slot array
    // Note: seen buffer is allocated with alignof(u32) in runtime.c, so this cast is safe
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
    u32* slots = (u32*)(seen + ((size_t)pc * RE_SEEN_SLOTS * sizeof(u32)));
#pragma GCC diagnostic pop

    // Exact match or empty slot fast path
    for (int i = 0; i < RE_SEEN_SLOTS; i++) {
        u32 v = slots[i];
        if (v == sig) {
            return 1; // already visited
        }
        if (v == 0) {
            slots[i] = sig;
            return 0; // new
        }
    }

    // All slots full with different signatures: replace a slot deterministically
    int idx = (int)(sig % RE_SEEN_SLOTS);
    slots[idx] = sig;
    return 0;
}

// Merge an already-closed thread into a list, keeping the first equivalent
// (pc, counter state). Callers order candidates by search direction so the
// leftmost/rightmost start wins without retaining input-sized start history.
static enum FisktaErr merge_thread(const FisktaReProg* p, ReList* l,
    const ReThread* thread, unsigned char* seen)
{
    u32 sig = (p->counter_count > 0)
        ? re_counters_sig(thread->counters, p->counter_count)
        : 1;
    if (re_seen_hit_or_set(seen, thread->pc, sig)) {
        return FISKTA_E_OK;
    }
    if (l->n >= l->cap) {
        error_set(FISKTA_E_CAPACITY, -1,
            "regex: exceeded internal NFA thread limit (%d threads)", l->cap);
        return FISKTA_E_CAPACITY;
    }
    l->v[l->n++] = *thread;
    return FISKTA_E_OK;
}

// Maximum recursion depth to prevent stack overflow with pathological patterns
#define MAX_EPSILON_RECURSION_DEPTH 500

// Ordered epsilon-closure push. Sets *match_found if RI_MATCH reachable for current pos and min_start.
// Returns FISKTA_E_OOM if thread list capacity is exceeded.
// Returns FISKTA_E_CAPACITY if recursion depth exceeds limit.
static enum FisktaErr add_thread_ordered(const FisktaReProg* p, ReList* l, int pc, i64 start,
    i64 pos, i64 win_lo, i64 win_hi, i64 file_size,
    unsigned char* seen, int* match_found, i64 min_start,
    unsigned char curr_char, unsigned char prev_char,
    int at_bol, int at_eol, const int* counters, u64 priority, int depth,
    u64* work_count, u64 work_budget)
{
    // Guard against work budget exhaustion (step-count explosion from nested quantifiers)
    if (++(*work_count) > work_budget) {
        error_set(FISKTA_E_CAPACITY, -1,
            "regex: work budget exceeded (pattern too complex or adversarial)");
        return FISKTA_E_CAPACITY;
    }

    // Guard against stack overflow from pathological patterns like ((x*){0}){999,}
    if (depth > MAX_EPSILON_RECURSION_DEPTH) {
        error_set(FISKTA_E_CAPACITY, -1,
            "regex: recursion depth exceeded (pattern too deeply nested)");
        return FISKTA_E_CAPACITY;
    }

    // Local counter state for this thread
    int local_counters[MAX_RE_COUNTERS];
    int counter_count = p->counter_count;
    if (counter_count > 0) {
        memcpy(local_counters, counters, (size_t)counter_count * sizeof(int));
    }

    while (1) {
        if (pc < 0 || pc >= p->nins) {
            return FISKTA_E_OK;
        }
        // Dedup by (pc, counter_signature)
        u32 sig = (counter_count > 0) ? re_counters_sig(local_counters, counter_count) : 1;
        if (re_seen_hit_or_set(seen, pc, sig)) {
            return FISKTA_E_OK; // Already visited this (pc, counter_state) combination
        }

        ReInst* i = &p->ins[pc];
        switch (i->op) {
        case RI_SPLIT: {
            // Snapshot counter state so both branches see the same starting values
            int saved_counters[MAX_RE_COUNTERS];
            if (counter_count > 0) {
                memcpy(saved_counters, local_counters, (size_t)counter_count * sizeof(int));
            }

            // Lazy-only penalty scheme:
            // - Extract flags: bit0=repeat, bit1=lazy
            // - Only apply +1 penalty to y-branch when BOTH repeat AND lazy are set
            // - Greedy quantifiers and alternation get no penalty (priorities tie, longer match wins)
            int repeat = (i->ch & 0x01) != 0;
            int lazy = (i->ch & 0x02) != 0;

            u64 prio_x, prio_y;
            if (repeat && lazy) {
                // Lazy quantifier SPLIT: x=exit (preferred), y=loop (penalized)
                prio_x = priority; // X-branch: no penalty (lazy exit)
                prio_y = priority + 1; // Y-branch: +1 penalty (lazy loop)
            } else {
                // Greedy quantifier or alternation: no penalty
                prio_x = priority;
                prio_y = priority;
            }

            // Explore X-branch first (always preferred by compiler's x/y assignment)
            enum FisktaErr err = add_thread_ordered(p, l, i->x, start, pos, win_lo, win_hi, file_size, seen, match_found, min_start, curr_char, prev_char, at_bol, at_eol, local_counters, prio_x, depth + 1, work_count, work_budget);
            if (err != FISKTA_E_OK) {
                return err;
            }

            // Restore counters before exploring the alternate branch
            if (counter_count > 0) {
                memcpy(local_counters, saved_counters, (size_t)counter_count * sizeof(int));
            }

            // Continue with Y-branch
            priority = prio_y;
            pc = i->y;
            continue;
        }
        case RI_JMP:
            pc = i->x;
            continue;
        case RI_COUNTER_RESET:
            // Reset counter to 0 and continue
            if (i->x >= 0 && i->x < MAX_RE_COUNTERS) {
                local_counters[i->x] = 0;
            }
            pc++;
            continue;
        case RI_COUNTER_INC:
            // Increment counter and continue to next instruction
            if (i->x >= 0 && i->x < MAX_RE_COUNTERS) {
                local_counters[i->x]++;
            }
            pc++;
            continue;
        case RI_COUNTER_CHECK:
            // If counter[x] >= y, fail this thread; else continue
            if (i->x >= 0 && i->x < MAX_RE_COUNTERS) {
                if (local_counters[i->x] >= i->y) {
                    // Counter limit reached, fail this thread
                    return FISKTA_E_OK;
                }
            }
            pc++;
            continue;
        case RI_COUNTER_CHECK_MIN:
            // If counter[x] < y, fail this thread; else continue
            if (i->x >= 0 && i->x < MAX_RE_COUNTERS) {
                if (local_counters[i->x] < i->y) {
                    // Minimum not met, fail this thread
                    return FISKTA_E_OK;
                }
            }
            pc++;
            continue;
        case RI_BOL:
            if (at_bol) {
                pc++;
                continue;
            }
            return FISKTA_E_OK;
        case RI_EOL:
            if (at_eol) {
                pc++;
                continue;
            }
            return FISKTA_E_OK;
        case RI_MATCH:
            if (start == min_start) {
                *match_found = 1;
            }
// DEBUG: Print match info
#ifdef DEBUG_PRIORITY
            fprintf(stderr, "MATCH: start=%lld, pos=%lld, priority=%llu\n", (long long)start, (long long)pos, (unsigned long long)priority);
#endif
            // Add the thread to the list so consumption step can detect it
            if (l->n >= l->cap) {
                error_set(FISKTA_E_CAPACITY, -1,
                    "regex: exceeded internal NFA thread limit (%d threads)", l->cap);
                return FISKTA_E_CAPACITY; // Thread list is full
            }
            l->v[l->n].pc = pc;
            l->v[l->n].start = start;
            if (counter_count > 0) {
                memcpy(l->v[l->n].counters, local_counters, (size_t)counter_count * sizeof(int));
            }
            l->v[l->n].priority = priority;
            l->n++;
            return FISKTA_E_OK;
        case RI_CHAR:
        case RI_ANY:
        case RI_CLASS:
            // consuming; add once
            if (l->n >= l->cap) {
                error_set(FISKTA_E_CAPACITY, -1,
                    "regex: exceeded internal NFA thread limit (%d threads)", l->cap);
                return FISKTA_E_CAPACITY; // Thread list is full
            }
            l->v[l->n].pc = pc;
            l->v[l->n].start = start;
            if (counter_count > 0) {
                memcpy(l->v[l->n].counters, local_counters, (size_t)counter_count * sizeof(int));
            }
            l->v[l->n].priority = priority;
            l->n++;
            return FISKTA_E_OK;
        default:
            return FISKTA_E_OK;
        }
    }
}

enum FisktaErr regex_search_window(File* io, i64 win_lo, i64 win_hi,
    const FisktaReProg* re, enum Dir dir, i64* ms, i64* me)
{
    if (!re || re->nins <= 0) {
        return FISKTA_E_PARSE;
    }
    if (win_lo < 0) {
        win_lo = 0;
    }
    if (win_hi > io->size) {
        win_hi = io->size;
    }
    if (win_lo > win_hi) {
        return FISKTA_E_NO_MATCH;
    }

    // Tail byte prefetching for cheap lookahead
    unsigned char tail1 = 0;
    unsigned char tail2 = 0;
    int tails = 0;

    // Use arena-backed scratch provided at startup
    const int nins = re->nins;
    const int cap = io->re.cap;
    if (cap <= 0 || !io->re.curr || !io->re.next || !io->re.seen_curr || !io->re.seen_next) {
        return FISKTA_E_OOM;
    }
    const size_t need_seen = (size_t)nins * RE_SEEN_SLOTS * sizeof(u32);
    if (need_seen > io->re.seen_bytes) {
        error_set(FISKTA_E_CAPACITY, -1,
            "regex: seen buffer too small (need %zu bytes, have %zu); increase re_ins_estimate",
            need_seen, io->re.seen_bytes);
        return FISKTA_E_CAPACITY; // Seen buffer not large enough for regex
    }

    ReThread* curr_buf = io->re.curr;
    ReThread* next_buf = io->re.next;
    unsigned char* seen_curr = io->re.seen_curr;
    unsigned char* seen_next = io->re.seen_next;
    ReList curr;
    ReList next;
    rlist_init(&curr, curr_buf, cap);
    rlist_init(&next, next_buf, cap);
    seen_clear_bytes(seen_curr, need_seen);
    seen_clear_bytes(seen_next, need_seen);

    i64 best_ms = -1;
    i64 best_me = -1;
    u64 best_priority = UINT64_MAX; // Worst priority (higher = worse)

    // Work budget: prevent step-count explosion from nested quantifiers
    u64 work_count = 0;
    u64 work_budget = io->re.work_budget;

    i64 pos = win_lo;
    i64 block_lo = win_lo;
    i64 block_hi = win_lo;
    size_t n = 0;
    // carry previous byte across block boundaries
    unsigned char prev_c = 0;
    int have_prev = 0;

    // Initialize previous character correctly for anchor semantics
    if (win_lo > 0) {
        unsigned char b;
        size_t n_read;
        enum FisktaErr read_err = io_read_at(io, win_lo - 1, &b, 1, &n_read);
        if (read_err != FISKTA_E_OK) {
            return read_err;
        }
        if (n_read == 1) {
            prev_c = b;
            have_prev = 1;
        }
    }

    for (;;) {
        // Refill buffer if needed for consumption step
        if (pos == block_hi && pos < win_hi) {
            block_lo = pos;
            block_hi = block_lo + (i64)io->buf_cap;
            if (block_hi > win_hi) {
                block_hi = win_hi;
            }
            enum FisktaErr read_err = io_read_at(io, block_lo, io->buf, (size_t)(block_hi - block_lo), &n);
            if (read_err != FISKTA_E_OK) {
                return read_err;
            }
            block_hi = block_lo + (i64)n; // clamp to bytes actually in buf

            // Prefetch up to two "tail" bytes beyond the block for cheap lookahead
            tail1 = tail2 = 0;
            tails = 0;
            if (block_hi < win_hi) {
                unsigned char t[2];
                size_t bytes_to_read = (win_hi - block_hi) >= 2 ? 2 : (size_t)(win_hi - block_hi);
                size_t m;
                enum FisktaErr tail_err = io_read_at(io, block_hi, t, bytes_to_read, &m);
                if (tail_err != FISKTA_E_OK) {
                    return tail_err;
                }
                if (m >= 1) {
                    tail1 = t[0];
                    tails = 1;
                }
                if (m >= 2) {
                    tail2 = t[1];
                    tails = 2;
                }
            }
        }

        // current/previous chars at position pos
        unsigned char curr_c = (pos < win_hi) ? io->buf[pos - block_lo] : 0;

        // Compute next1/next2 using tail bytes for cheap lookahead
        unsigned char next1 = 0;
        unsigned char next2 = 0;
        if (pos + 1 < block_hi) {
            next1 = io->buf[pos + 1 - block_lo];
        } else if (pos + 1 == block_hi && tails >= 1) {
            next1 = tail1;
        }

        if (pos + 2 < block_hi) {
            next2 = io->buf[pos + 2 - block_lo];
        } else if (pos + 2 == block_hi && tails >= 1) {
            next2 = tail1;
        } else if (pos + 2 == block_hi + 1 && tails >= 2) {
            next2 = tail2;
        }

        unsigned char prev_char = have_prev ? prev_c : 0;

        // Anchor state at the current position, before consuming curr_c.
        int at_bol = (pos == win_lo) || (have_prev && prev_c == '\n');
        int at_eol = (pos == win_hi) || (curr_c == '\n')
            || (curr_c == '\r' && next1 == '\n');

        // Anchor state after consuming curr_c, at position pos + 1.
        int next_at_bol = curr_c == '\n';
        int next_at_eol = (pos + 1 == win_hi) || (next1 == '\n')
            || (next1 == '\r' && next2 == '\n');

        // Merge a new attempt at every input position with attempts already in
        // flight. Equivalent states keep the earlier start for forward search
        // and the later start for backward search, bounding the list by NFA
        // state rather than by input length.
        rlist_clear(&next);
        seen_clear_bytes(seen_next, need_seen);
        int zero_counters[MAX_RE_COUNTERS] = { 0 };
        int ignored_match = 0;
        enum FisktaErr err;

        if (dir == DIR_FWD) {
            for (int i = 0; i < curr.n; i++) {
                err = merge_thread(re, &next, &curr.v[i], seen_next);
                if (err != FISKTA_E_OK) {
                    return err;
                }
            }
            // Once a forward match exists, later starts cannot improve it.
            if (best_ms < 0) {
                err = add_thread_ordered(re, &next, 0, pos, pos, win_lo, win_hi, io->size,
                    seen_next, &ignored_match, pos, curr_c, prev_char, at_bol, at_eol,
                    zero_counters, 0ULL, 0, &work_count, work_budget);
                if (err != FISKTA_E_OK) {
                    return err;
                }
            }
        } else {
            err = add_thread_ordered(re, &next, 0, pos, pos, win_lo, win_hi, io->size,
                seen_next, &ignored_match, pos, curr_c, prev_char, at_bol, at_eol,
                zero_counters, 0ULL, 0, &work_count, work_budget);
            if (err != FISKTA_E_OK) {
                return err;
            }
            for (int i = 0; i < curr.n; i++) {
                err = merge_thread(re, &next, &curr.v[i], seen_next);
                if (err != FISKTA_E_OK) {
                    return err;
                }
            }
        }

        ReList tmp_l = curr;
        curr = next;
        next = tmp_l;
        unsigned char* tmpb = seen_curr;
        seen_curr = seen_next;
        seen_next = tmpb;

        // Record matches at this position, then discard MATCH threads. Active
        // starts that can no longer beat the best result are discarded too.
        int write_idx = 0;
        for (int i = 0; i < curr.n; i++) {
            int pc = curr.v[i].pc;
            i64 st = curr.v[i].start;
            if (pc >= 0 && pc < re->nins && re->ins[pc].op == RI_MATCH) {
                u64 priority = curr.v[i].priority;
                int better_start = best_ms < 0
                    || (dir == DIR_FWD ? st < best_ms : st > best_ms);
                if (better_start || (st == best_ms
                        && (priority < best_priority
                            || (priority == best_priority && pos > best_me)))) {
                    best_ms = st;
                    best_me = pos;
                    best_priority = priority;
                }
                continue;
            }
            if (best_ms >= 0
                && (dir == DIR_FWD ? st > best_ms : st < best_ms)) {
                continue;
            }
            curr.v[write_idx++] = curr.v[i];
        }
        curr.n = write_idx;

        if (dir == DIR_FWD && best_ms >= 0 && curr.n == 0) {
            *ms = best_ms;
            *me = best_me;
            return FISKTA_E_OK;
        }

        if (pos == win_hi) {
            break; // nothing to consume
        }

        unsigned char c = curr_c;
        // Build next from curr by consuming c
        rlist_clear(&next);
        seen_clear_bytes(seen_next, need_seen);

        for (int i = 0; i < curr.n; i++) {
            int pc = curr.v[i].pc;
            i64 st = curr.v[i].start;
            ReInst* inst = &re->ins[pc];
            switch (inst->op) {
            case RI_CHAR:
                if (c == inst->ch) {
                    err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, st, next1, c, next_at_bol, next_at_eol, curr.v[i].counters, curr.v[i].priority, 0,
                        &work_count, work_budget);
                    if (err != FISKTA_E_OK) {
                        return err;
                    }
                }
                break;
            case RI_ANY:
                if (c != '\n') { // dot ≠ newline
                    err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, st, next1, c, next_at_bol, next_at_eol, curr.v[i].counters, curr.v[i].priority, 0,
                        &work_count, work_budget);
                    if (err != FISKTA_E_OK) {
                        return err;
                    }
                }
                break;
            case RI_CLASS:
                if (inst->cls_idx >= 0 && inst->cls_idx < re->nclasses && cls_has(&re->classes[inst->cls_idx], c)) {
                    err = add_thread_ordered(re, &next, pc + 1, st, pos + 1, win_lo, win_hi, io->size, seen_next, &(int) { 0 }, st, next1, c, next_at_bol, next_at_eol, curr.v[i].counters, curr.v[i].priority, 0,
                        &work_count, work_budget);
                    if (err != FISKTA_E_OK) {
                        return err;
                    }
                }
                break;
            default:
                // Shouldn't be here; closure should have removed epsilons
                break;
            }
        }

        // Advance: swap lists (do NOT swap raw buffers; swap the structs)
        tmp_l = curr;
        curr = next;
        next = tmp_l;
        tmpb = seen_curr;
        seen_curr = seen_next;
        seen_next = tmpb;
        seen_clear_bytes(seen_curr, need_seen);
        // advance and carry previous char
        prev_c = curr_c;
        have_prev = (pos < win_hi);
        pos++;
    }

    if (best_ms >= 0) {
        *ms = best_ms;
        *me = best_me;
        return FISKTA_E_OK;
    }
    return FISKTA_E_NO_MATCH;
}

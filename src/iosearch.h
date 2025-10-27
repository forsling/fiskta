#pragma once
#include "fiskta.h"
#include "regex_prog.h"
#include "fileio.h"
#include "search_literal.h"
#include "regex_vm.h"
#include <stdio.h>

// Search buffer size constants
#ifndef FISKTA_FW_WIN
#define FISKTA_FW_WIN (6 * 1024 * 1024)
#endif
#ifndef FISKTA_BK_BLK
#define FISKTA_BK_BLK (3 * 1024 * 1024)
#endif
#ifndef FISKTA_OVERLAP_MIN
#define FISKTA_OVERLAP_MIN (4 * 1024)
#endif
#ifndef FISKTA_OVERLAP_MAX
#define FISKTA_OVERLAP_MAX (64 * 1024)
#endif

enum {
    FW_WIN = FISKTA_FW_WIN,
    BK_BLK = FISKTA_BK_BLK,
    OVERLAP_MIN = FISKTA_OVERLAP_MIN,
    OVERLAP_MAX = FISKTA_OVERLAP_MAX
};

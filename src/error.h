#pragma once

#include "fiskta.h"
#include <stdbool.h>

enum { ERROR_DETAIL_MESSAGE_MAX = 160 };

typedef struct {
    enum Err err;
    i32 position; // Optional contextual position (e.g., token index); -1 if unused
    char message[ERROR_DETAIL_MESSAGE_MAX];
} ErrorDetail;

void error_detail_reset(void);
void error_detail_set(enum Err err, i32 position, const char* fmt, ...);
bool error_detail_has(void);
const ErrorDetail* error_detail_last(void);

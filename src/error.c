#include "error.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static ErrorDetail detail_state = { .err = E_OK, .position = -1, .message = { 0 } };

void error_detail_reset(void)
{
    detail_state.err = E_OK;
    detail_state.position = -1;
    detail_state.message[0] = '\0';
}

void error_detail_set(enum Err err, i32 position, const char* fmt, ...)
{
    if (!fmt) {
        return;
    }

    detail_state.err = err;
    detail_state.position = position;

    va_list args;
    va_start(args, fmt);
    vsnprintf(detail_state.message, ERROR_DETAIL_MESSAGE_MAX, fmt, args);
    va_end(args);
}

bool error_detail_has(void)
{
    return detail_state.message[0] != '\0';
}

const ErrorDetail* error_detail_last(void)
{
    if (!error_detail_has()) {
        return NULL;
    }
    return &detail_state;
}

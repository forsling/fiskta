#pragma once

#include "fiskta_types.h"

// Internal error reporting API
// Used by parse, regex, and engine code to set diagnostic information
void error_set(enum Err err, i32 position, const char* fmt, ...);

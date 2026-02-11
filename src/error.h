// error.h - Internal error reporting (not part of public API)
#pragma once

#include "fiskta_types.h"

// Internal error reporting hook used by parser, engine, and regex subsystems.
// NOT part of the public API -- do not include this header in fiskta.h.
void error_set(enum Err err, i32 position, const char* fmt, ...);

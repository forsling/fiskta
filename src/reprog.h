#pragma once
#include "fiskta.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RI_CHAR,
    RI_ANY,
    RI_CLASS,
    RI_BOL, // '^'  (true at win_lo or after \n)
    RI_EOL, // '$'  (true at win_hi or before \n)
    RI_SPLIT, // ordered epsilon: x then y (order encodes greediness)
    RI_JMP,
    RI_MATCH
} ReOp;

typedef struct {
    unsigned char bits[32]; // 256-bit ASCII bitmap
} ReClass;

typedef struct {
    ReOp op;
    int x, y; // for SPLIT/JMP
    unsigned char ch; // for CHAR
    int cls_idx; // for CLASS
} ReInst;

typedef struct ReProg {
    ReInst* ins;
    int nins;
    ReClass* classes;
    int nclasses;
} ReProg;

// Compile pattern into preallocated pools; advances *ins_used / *cls_used
enum Err re_compile_into(String pattern,
    ReProg* out,
    ReInst* ins_base, int ins_cap, int* ins_used,
    ReClass* cls_base, int cls_cap, int* cls_used);

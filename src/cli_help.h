#pragma once

enum FisktaExitCode {
    FISKTA_EXIT_OK = 0,
    FISKTA_EXIT_IO = 1,
    FISKTA_EXIT_PARSE = 2,
    FISKTA_EXIT_REGEX = 3,
    FISKTA_EXIT_RESOURCE = 4,
    FISKTA_EXIT_TIMEOUT = 5,
    FISKTA_EXIT_PROGRAM_FAIL = 9,
};

void print_usage(void);
void print_examples(void);

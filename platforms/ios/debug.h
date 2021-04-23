#ifndef DEBUG_H
#define DEBUG_H

#define SQLITE_TOKENIZER_DEBUG

#ifdef SQLITE_TOKENIZER_DEBUG
#include <stdio.h>
#define log_error(...) fprintf(stderr, __VA_ARGS__)
#define log_debug(...) printf(__VA_ARGS__)
#else
#define log_error(...)
#define log_debug(...)
#endif

#endif

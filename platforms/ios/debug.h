#define DEBUG = 1

#ifdef DEBUG
#define log_error(...) fprintf(stderr, __VA_ARGS__)
#define log_debug(...) printf(__VA_ARGS__)
#else
#define log_error(...)
#define log_debug(...)
#endif

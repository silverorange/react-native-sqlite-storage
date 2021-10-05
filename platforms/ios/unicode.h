#ifndef UNICODE_H
#define UNICODE_H

#include <sqlite3.h>

int sqlite3_unicode_init(sqlite3 *pDb, char **pzError,
                         const sqlite3_api_routines *pApi);

#endif

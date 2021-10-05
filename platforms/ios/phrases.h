#ifndef PHRASES_H
#define PHRASES_H

#include <sqlite3.h>

int sqlite3_phrases_init(sqlite3 *pDb, char **pzError,
                         const sqlite3_api_routines *pApi);

#endif

#ifndef SNOWBALL_H
#define SNOWBALL_H

#include <sqlite3.h>

int sqlite3_snowball_init(sqlite3 *pDb, char **pzError,
                          const sqlite3_api_routines *pApi);

#endif

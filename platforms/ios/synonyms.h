#ifndef SYNONYMS_H
#define SYNONYMS_H

#include <sqlite3.h>

int sqlite3_synonyms_init(sqlite3 *pDb, char **pzError,
                          const sqlite3_api_routines *pApi);

#endif

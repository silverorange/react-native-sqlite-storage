#ifndef STOPWORDS_H
#define STOPWORDS_H

#include <sqlite3.h>

int sqlite3_stopwords_init(sqlite3 *pDb, char **pzError,
                           const sqlite3_api_routines *pApi);

#endif

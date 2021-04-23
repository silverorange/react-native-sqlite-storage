#ifndef META_H
#define META_H

#include <sqlite3.h>

int meta_create_table(sqlite3 *pDb, const char *zTableName);
int meta_needs_update(sqlite3 *pDb, const char *zName, int nLastUpdate,
                      int *pnDate, const char *zTableName);

#endif

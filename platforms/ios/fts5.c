#include "fts5.h"
#include <stdio.h>

/*
** Return a pointer to the fts5_api pointer for database connection db.
** If an error occurs, return NULL and leave an error in the database
** handle (accessible using sqlite3_errcode()/errmsg()).
*/
fts5_api *fts5_api_from_db(sqlite3 *pDb) {
  fts5_api *pFts5Api = NULL;
  sqlite3_stmt *pStatement = NULL;

  if (SQLITE_OK ==
      sqlite3_prepare(pDb, "SELECT fts5(?1)", -1, &pStatement, NULL)) {
    // Note: This is not available in iOS < 12. We need to set min iOS version
    // to 12 to be able to use this API.
    sqlite3_bind_pointer(pStatement, 1, (void *)&pFts5Api, "fts5_api_ptr",
                         NULL);
    sqlite3_step(pStatement);
  }
  sqlite3_finalize(pStatement);
  return pFts5Api;
}

#ifndef FTS5_H
#define FTS5_H

#include <sqlite3.h>

/** Token flag to say token is the final token in the stream. */
#define FTS5_TOKEN_FINAL 0x02

/*
** Return a pointer to the fts5_api pointer for database connection db.
** If an error occurs, return NULL and leave an error in the database
** handle (accessible using sqlite3_errcode()/errmsg()).
*/
fts5_api *fts5_api_from_db(sqlite3 *pDb);

#endif

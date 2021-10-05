#include "meta.h"
#include "debug.h"
#include <sqlite3.h>
#include <stddef.h>
#include <string.h>

#define META_DEFAULT_TABLE_NAME "fts5_meta"

// if last updated time < database update time, return nDate, otherwise 0
int meta_needs_update(sqlite3 *pDb, const char *zName, int nLastUpdate,
                      int *pnDate, const char *zTableName) {
  int rc = SQLITE_OK;
  sqlite3_stmt *pStmt;
  int nRet = 0;

  if (zTableName == NULL) {
    zTableName = (const char *){META_DEFAULT_TABLE_NAME};
  }

  const char *zStatementTemplate = "SELECT ? < date AS needs_update, date "
                                   "FROM %s "
                                   "WHERE name = ?;";

  // Add +1 for null terminator, -2 for removing %s replacement.
  const size_t nStatementLength =
      strlen(zStatementTemplate) + strlen(zTableName) - 1;

  char *zStatementSql = (char *)sqlite3_malloc((int)nStatementLength);
  if (zStatementSql == NULL) {
    rc = SQLITE_NOMEM;
    log_error("[meta] Unable to allocate prepared statement string\n");
  } else {
    sqlite3_snprintf((int)nStatementLength, zStatementSql, zStatementTemplate,
                     zTableName);
    log_debug("[meta] Running SQL \"%s\"\n", zStatementSql);
    rc = sqlite3_prepare_v2(pDb, zStatementSql, (int)nStatementLength, &pStmt,
                            NULL);
    if (rc != SQLITE_OK) {
      log_error("[meta] Failed to prepare statement: %s\n",
                sqlite3_errmsg(pDb));
      sqlite3_free(zStatementSql);
      return rc;
    }

    rc = sqlite3_bind_int(pStmt, 1, nLastUpdate);
    if (rc != SQLITE_OK) {
      log_error("[meta] Failed to bind date value to prepared statement: %s\n",
                sqlite3_errmsg(pDb));
      sqlite3_finalize(pStmt);
      sqlite3_free(zStatementSql);
      return rc;
    }

    rc = sqlite3_bind_text(pStmt, 2, zName, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
      log_error("[meta] Failed to bind name value to prepared statement: %s\n",
                sqlite3_errmsg(pDb));
      sqlite3_finalize(pStmt);
      sqlite3_free(zStatementSql);
      return rc;
    }

    rc = sqlite3_step(pStmt);
    if (rc == SQLITE_ROW) {

      int bNeedsUpdate = sqlite3_column_int(pStmt, 0);
      if (bNeedsUpdate == 1) {
        nRet = sqlite3_column_int(pStmt, 1);
        log_debug("[meta] Got new timestamp %d from database for %s\n", nRet,
                  zName);
      }
    }

    rc = sqlite3_finalize(pStmt);
    if (rc != SQLITE_OK) {
      log_error("[meta] Failed to run prepared statement: %s\n",
                sqlite3_errmsg(pDb));
      sqlite3_free(zStatementSql);
      return rc;
    }

    sqlite3_free(zStatementSql);
  }

  *pnDate = nRet;

  return rc;
}

int meta_create_table(sqlite3 *pDb, const char *zTableName) {
  int rc = SQLITE_OK;

  if (zTableName == NULL) {
    zTableName = (const char *){META_DEFAULT_TABLE_NAME};
  }

  const char *zStatementTemplate = "CREATE TABLE IF NOT EXISTS %s ("
                                   "  name TEXT NOT NULL, "
                                   "  date INTEGER NOT NULL, "
                                   "  PRIMARY KEY (name)"
                                   ");";

  // Add +1 for null terminator, -2 for removing %s replacement.
  const size_t nStatementLength =
      strlen(zStatementTemplate) + strlen(zTableName) - 1;

  char *zStatementSql = (char *)sqlite3_malloc((int)nStatementLength);
  if (zStatementSql == NULL) {
    rc = SQLITE_NOMEM;
  } else {
    sqlite3_snprintf((int)nStatementLength, zStatementSql, zStatementTemplate,
                     zTableName);
    log_debug("[meta] Running SQL \"%s\"\n", zStatementSql);
    rc = sqlite3_exec(pDb, zStatementSql, NULL, NULL, NULL);
    sqlite3_free(zStatementSql);
  }

  if (rc != SQLITE_OK) {
    log_error("[meta] Failed to execute statement: %s\n", sqlite3_errmsg(pDb));
  } else {
    log_debug("[meta] Created table \"%s\"\n", zTableName);
  }

  return rc;
}

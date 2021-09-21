#include "stopwords.h"
#include "debug.h"
#include "meta.h"
#include "uthash.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

typedef struct StopwordsTokenizer StopwordsTokenizer;
struct StopwordsTokenizer {
  fts5_tokenizer tokenizer;                  /* Parent tokenizer module */
  Fts5Tokenizer *pTokenizer;                 /* Parent tokenizer instance */
  StopwordsTokenizerCreateContext *pContext; /* creation context */
};

typedef struct StopwordsCallbackContext StopwordsCallbackContext;
struct StopwordsCallbackContext {
  void *pCtx;
  int (*xToken)(void *, int, const char *, int, int, int);
  int flags;
  StopwordsHash *pStopwordsHash;
};

const char *STOPWORDS_DEFAULT_PARENT_TOKENIZER = "unicode";
const char *STOPWORDS_DEFAULT_TABLE_NAME = "fts5_stopwords";

#ifdef SQLITE_TOKENIZER_DEBUG
static void debug_stopwords_hash(StopwordsHash *pStopwordsHash) {
  printf("[stopwords] DATA: ");

  StopwordsHash *pStopword, *tmp = NULL;
  HASH_ITER(hh, pStopwordsHash, pStopword, tmp) {
    printf("%.*s ", pStopword->nWordLength, pStopword->pWord);
  }

  printf("\n");
}
#endif

static int stopwords_create_table(sqlite3 *pDb, const char *zTableName) {
  int rc = SQLITE_OK;

  // It would be nice in future to get the virtual table name from FTS5 and
  // create TableName_synonyms.
  if (zTableName == NULL) {
    zTableName = STOPWORDS_DEFAULT_TABLE_NAME;
  }

  const char *zStatementTemplate = "CREATE TABLE IF NOT EXISTS %s ("
                                   "  word TEXT NOT NULL, "
                                   "  PRIMARY KEY (word)"
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
    log_debug("[stopwords] Running SQL \"%s\"\n", zStatementSql);
    rc = sqlite3_exec(pDb, zStatementSql, NULL, NULL, NULL);
    sqlite3_free(zStatementSql);
  }

  if (rc != SQLITE_OK) {
    log_error("[stopwords] Failed to execute statement: %s\n",
              sqlite3_errmsg(pDb));
  } else {
    log_debug("[stopwords] Created \"%s\" table\n", zTableName);
  }

  return rc;
}

static int stopwords_fetch_all_into_hash(sqlite3 *pDb, const char *zTableName,
                                         StopwordsHash **ppStopwordsHash) {
  StopwordsHash *pRet = NULL;
  sqlite3_stmt *pStatement;
  int rc = SQLITE_OK;

  if (zTableName == NULL) {
    zTableName = STOPWORDS_DEFAULT_TABLE_NAME;
  }

  const char *zStatementTemplate = "SELECT word FROM %s ORDER BY word;";

  // Add +1 for null terminator, -2 for removing %s replacement.
  const size_t nStatementLength =
      strlen(zStatementTemplate) + strlen(zTableName) - 1;

  char *zStatementSql = (char *)sqlite3_malloc((int)nStatementLength);
  if (zStatementSql == NULL) {
    rc = SQLITE_NOMEM;
    *ppStopwordsHash = pRet;
    return rc;
  }

  sqlite3_snprintf((int)nStatementLength, zStatementSql, zStatementTemplate,
                   zTableName);

  log_debug("[stopwords] Running SQL \"%s\"\n", zStatementSql);

  rc = sqlite3_prepare_v2(pDb, zStatementSql, -1, &pStatement, 0);
  if (rc != SQLITE_OK) {
    log_error("[stopwords] Failed to execute statement: %s\n",
              sqlite3_errmsg(pDb));
    sqlite3_free(zStatementSql);
    *ppStopwordsHash = pRet;
    return rc;
  }

  log_debug("[stopwords] Fetched from \"%s\" table\n", zTableName);
  log_debug("[stopwords] FETCHED: ");

  int step = SQLITE_OK;
  while ((step = sqlite3_step(pStatement)) == SQLITE_ROW) {
    const char *zTempWord = (const char *)sqlite3_column_text(pStatement, 0);
    const unsigned int nLength = (unsigned int)strlen(zTempWord);

    // Get length-delimited string for hash key.
    char *pWord = (char *)sqlite3_malloc(sizeof(char) * nLength);
    strncpy(pWord, zTempWord, nLength);

    StopwordsHash *pStopword =
        (StopwordsHash *)sqlite3_malloc(sizeof(StopwordsHash));

    if (pStopword == NULL) {
      rc = SQLITE_NOMEM;
      // TODO break and free everthing, return null
    }
    pStopword->pWord = pWord;
    pStopword->nWordLength = nLength;
    HASH_ADD_KEYPTR(hh, pRet, pStopword->pWord, nLength, pStopword);
    log_debug("%.*s ", nLength, pWord);
  }

  log_debug("\n");
  sqlite3_finalize(pStatement);

  *ppStopwordsHash = pRet;
  return rc;
}

static void stopwords_context_delete_hash(StopwordsHash *pStopwordsHash) {
  if (pStopwordsHash) {
    StopwordsHash *pStopword, *tmp = NULL;

    log_debug("  freeing stopwords hash\n");
    log_debug("  DELETED: ");
    HASH_ITER(hh, pStopwordsHash, pStopword, tmp) {
      log_debug("%.*s ", pStopword->nWordLength, pStopword->pWord);
      HASH_DEL(pStopwordsHash, pStopword);
      sqlite3_free(pStopword->pWord);
      sqlite3_free(pStopword);
    }
    log_debug("\n");
  }
}

static int stopwords_context_update(StopwordsTokenizerCreateContext *pContext) {
  int rc = SQLITE_OK;
  int nLastUpdated = 0;

  rc = meta_needs_update(pContext->pDb, "stopwords", pContext->nLastUpdated,
                         &nLastUpdated, NULL);
  if (rc != SQLITE_OK) {
    log_error("[stopwords] Failed to check stopwords cache validity: %s\n",
              sqlite3_errmsg(pContext->pDb));
    return rc;
  }

  if (pContext->nLastUpdated == 0 || nLastUpdated > 0) {
    log_error("[stopwords] Updating stopwords context\n");
    stopwords_context_delete_hash(pContext->pStopwordsHash);
    rc = stopwords_fetch_all_into_hash(pContext->pDb, NULL,
                                       &(pContext->pStopwordsHash));
    if (rc == SQLITE_OK) {
#ifdef SQLITE_TOKENIZER_DEBUG
      debug_stopwords_hash(pContext->pStopwordsHash);
#endif
    } else {
      log_error("[stopwords] Failed to load stopwords: %s\n",
                sqlite3_errmsg(pContext->pDb));
    }
    pContext->nLastUpdated = nLastUpdated;
  }

  return rc;
}

int stopwords_context_create(sqlite3 *pDb, fts5_api *pFts5Api,
                             StopwordsTokenizerCreateContext **ppContext) {
  log_debug("[stopwords] Creating stopwords context\n");

  StopwordsTokenizerCreateContext *pRet = NULL;
  int rc = SQLITE_OK;

  rc = meta_create_table(pDb, NULL);
  if (rc != SQLITE_OK) {
    log_error("[stopwords] Failed to create tokenizer meta table: %s\n",
              sqlite3_errmsg(pDb));
  }

  rc = stopwords_create_table(pDb, NULL);
  if (rc != SQLITE_OK) {
    log_error("[stopwords] Failed to create stopwords table: %s\n",
              sqlite3_errmsg(pDb));
  }

  if (rc == SQLITE_OK) {
    pRet = (StopwordsTokenizerCreateContext *)sqlite3_malloc(
        sizeof(StopwordsTokenizerCreateContext));

    if (pRet) {
      memset(pRet, 0, sizeof(StopwordsTokenizerCreateContext));

      pRet->pStopwordsHash = NULL;
      pRet->pFts5Api = pFts5Api;
      pRet->nLastUpdated = 0;
      pRet->pDb = pDb;
    } else {
      rc = SQLITE_NOMEM;
    }
  }

  if (rc == SQLITE_OK) {
    log_debug("[stopwords] Created stopwords context\n");
  } else {
    log_error(
        "[stopwords] There was a problem creating the stopwords context\n");
    if (pRet) {
      stopwords_context_delete(pRet);
    }
    pRet = NULL;
  }

  *ppContext = (StopwordsTokenizerCreateContext *)pRet;
  return rc;
}

void stopwords_context_delete(StopwordsTokenizerCreateContext *pContext) {
  if (pContext) {
    log_debug("[stopwords] Deleting stopwords context\n");
    stopwords_context_delete_hash(pContext->pStopwordsHash);
    sqlite3_free(pContext);
  }
}

void stopwords_tokenizer_delete(Fts5Tokenizer *pTok) {
  if (pTok) {
    log_debug("[stopwords] Deleting stopwords tokenizer\n");
    StopwordsTokenizer *p = (StopwordsTokenizer *)pTok;
    if (p->pTokenizer) {
      p->tokenizer.xDelete(p->pTokenizer);
    }
    sqlite3_free(p);
  }
}

int stopwords_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                               Fts5Tokenizer **ppOut) {
  log_debug("[stopwords] Creating stopwords tokenizer\n");
  StopwordsTokenizerCreateContext *pCreateCtx =
      (StopwordsTokenizerCreateContext *)pCtx;

  fts5_api *pFts5Api = pCreateCtx->pFts5Api;
  StopwordsTokenizer *pRet;
  const char *zBase = STOPWORDS_DEFAULT_PARENT_TOKENIZER;
  void *pUserdata = 0;
  int rc = SQLITE_OK;

  if (nArg > 0) {
    zBase = azArg[0];
    log_debug("  stopwords tokenizer has base \"%s\"\n", zBase);
  }

  pRet = (StopwordsTokenizer *)sqlite3_malloc(sizeof(StopwordsTokenizer));
  if (pRet) {
    memset(pRet, 0, sizeof(StopwordsTokenizer));
    pRet->pContext = pCreateCtx;

    // set parent tokenizer module
    rc =
        pFts5Api->xFindTokenizer(pFts5Api, zBase, &pUserdata, &pRet->tokenizer);
  } else {
    rc = SQLITE_NOMEM;
  }

  if (rc == SQLITE_OK) {
    int nArg2 = (nArg > 0 ? nArg - 1 : 0);
    const char **azArg2 = (nArg2 ? &azArg[1] : NULL);

    // set parent tokenizer instance, pass through all args
    log_debug("  creating \"%s\" parent tokenizer for stopwords\n", zBase);
    rc = pRet->tokenizer.xCreate(pUserdata, azArg2, nArg2, &pRet->pTokenizer);
  }

  if (rc != SQLITE_OK) {
    log_error(
        "[stopwords] There was a problem creating the stopwords tokenizer\n");
    stopwords_tokenizer_delete((Fts5Tokenizer *)pRet);
    pRet = NULL;
  }

  log_debug("  created stopwords tokenizer\n");
  *ppOut = (Fts5Tokenizer *)pRet;
  return rc;
}

static int is_stopword(StopwordsHash *pStopwordsHash, const char *pToken,
                       int nToken) {
  if (nToken == 0) {
    return 0;
  }

  // Token strings may or may-not be null-terminated
  const size_t nTokenLength = pToken[nToken - 1] == '\0' ? nToken - 1 : nToken;

  StopwordsHash *pStopWord = NULL;
  HASH_FIND(hh, pStopwordsHash, pToken, nTokenLength, pStopWord);
  if (pStopWord != NULL) {
    return 1;
  }

  return 0;
}

static int stopwords_tokenize_callback(void *pCtx, int tflags,
                                       const char *pToken, int nToken,
                                       int iStart, int iEnd) {
  StopwordsCallbackContext *p = (StopwordsCallbackContext *)pCtx;

  if (is_stopword(p->pStopwordsHash, pToken, nToken) == 1) {
    return SQLITE_OK;
  }

  return p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);
}

int stopwords_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                 int flags, const char *pText, int nText,
                                 int (*xToken)(void *, int, const char *,
                                               int nToken, int iStart,
                                               int iEnd)) {
  StopwordsTokenizer *p = (StopwordsTokenizer *)pTokenizer;
  StopwordsCallbackContext sCtx;

  stopwords_context_update(p->pContext);

  sCtx.pStopwordsHash = p->pContext->pStopwordsHash;
  sCtx.xToken = xToken;
  sCtx.pCtx = pCtx;
  sCtx.flags = flags;

  return p->tokenizer.xTokenize(p->pTokenizer, (void *)&sCtx, flags, pText,
                                nText, stopwords_tokenize_callback);
}

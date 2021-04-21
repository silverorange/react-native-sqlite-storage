#include "synonyms.h"
#include "debug.h"
#include "utarray.h"
#include "uthash.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

typedef struct SynonymsTokenizer SynonymsTokenizer;
struct SynonymsTokenizer {
  fts5_tokenizer tokenizer;    /* Parent tokenizer module */
  Fts5Tokenizer *pTokenizer;   /* Parent tokenizer instance */
  SynonymsHash *pSynonymsHash; /* hash-table of loaded synonyms */
};

typedef struct SynonymsCallbackContext SynonymsCallbackContext;
struct SynonymsCallbackContext {
  void *pCtx;
  int (*xToken)(void *, int, const char *, int, int, int);
  int flags;
  SynonymsHash *pSynonymsHash;
};

const char *SYNONYMS_DEFAULT_SYNONYMS_TABLE_NAME = "fts5_synonyms";
const char *SYNONYMS_DEFAULT_PARENT_TOKENIZER = "porter";

#ifdef DEBUG
static void debug_hash(SynonymsHash *pSynonymsHash) {
  SynonymsHash *pSynonym, *tmp = NULL;
  HASH_ITER(hh, pSynonymsHash, pSynonym, tmp) {
    char **p = NULL;
    printf("\n%s\n", pSynonym->zWord);
    while ((p = (char **)utarray_next(pSynonym->zExpansions, p))) {
      printf(" -> %s\n", *p);
    }
  }
}
#endif

static int synonyms_create_table(sqlite3 *pDb, const char *zTableName) {
  int rc = SQLITE_OK;

  // It would be nice in future to get the virtual table name from FTS5 and
  // create TableName_synonyms.
  if (zTableName == NULL) {
    zTableName = SYNONYMS_DEFAULT_SYNONYMS_TABLE_NAME;
  }

  const char *zStatementTemplate = "CREATE TABLE IF NOT EXISTS %s ("
                                   "  word TEXT NOT NULL, "
                                   "  expansion TEXT NOT NULL, "
                                   "  PRIMARY KEY (word, expansion)"
                                   ");";

  // Add +1 for null terminator, -2 for removing %s replacement.
  const size_t nStatementLength =
      strlen(zStatementTemplate) + strlen(zTableName) - 1;

  char *zStatementSql = sqlite3_malloc((int)nStatementLength);
  if (zStatementSql == NULL) {
    rc = SQLITE_NOMEM;
  } else {
    sqlite3_snprintf((int)nStatementLength, zStatementSql, zStatementTemplate,
                     zTableName);
    log_debug("running %s\n", zStatementSql);
    rc = sqlite3_exec(pDb, zStatementSql, NULL, NULL, NULL);
    sqlite3_free(zStatementSql);
  }

  if (rc != SQLITE_OK) {
    log_error("Failed to execute statement: %s\n", sqlite3_errmsg(pDb));
  } else {
    log_debug("created synonym table\n");
  }

  return rc;
}

static int synonyms_fetch_all_into_hash(sqlite3 *pDb, const char *zTableName,
                                        SynonymsHash **ppSynonymsHash) {
  SynonymsHash *pRet = NULL;
  sqlite3_stmt *pStatement;
  int rc = SQLITE_OK;

  if (zTableName == NULL) {
    zTableName = SYNONYMS_DEFAULT_SYNONYMS_TABLE_NAME;
  }

  const char *zStatementTemplate =
      "SELECT word, expansion FROM %s ORDER BY word;";

  // Add +1 for null terminator, -2 for removing %s replacement.
  const size_t nStatementLength =
      strlen(zStatementTemplate) + strlen(zTableName) - 1;

  char *zStatementSql = sqlite3_malloc((int)nStatementLength);
  if (zStatementSql == NULL) {
    rc = SQLITE_NOMEM;
    *ppSynonymsHash = pRet;
    return rc;
  }

  sqlite3_snprintf((int)nStatementLength, zStatementSql, zStatementTemplate,
                   zTableName);

  log_debug("running %s\n", zStatementSql);

  rc = sqlite3_prepare_v2(pDb, zStatementSql, -1, &pStatement, 0);
  if (rc != SQLITE_OK) {
    log_error("Failed to execute statement: %s\n", sqlite3_errmsg(pDb));
    sqlite3_free(zStatementSql);
    *ppSynonymsHash = pRet;
    return rc;
  }

  log_debug("fetched from synonym table\n");

  int step = SQLITE_OK;
  while ((step = sqlite3_step(pStatement)) == SQLITE_ROW) {
    char *zWord = strdup((const char *)sqlite3_column_text(pStatement, 0));
    char *zSynonym = strdup((const char *)sqlite3_column_text(pStatement, 1));

    SynonymsHash *pSynonym = NULL;
    HASH_FIND_STR(pRet, zWord, pSynonym);
    if (pSynonym == NULL) {
      // New synonym, we need to create a hash entry.
      pSynonym = sqlite3_malloc(sizeof(SynonymsHash));
      if (pSynonym == NULL) {
        rc = SQLITE_NOMEM;
        // TODO break and free everthing, return null
      }
      pSynonym->zWord = zWord;
      utarray_new(pSynonym->zExpansions, &ut_str_icd);
      utarray_push_back(pSynonym->zExpansions, &zSynonym);
      HASH_ADD_KEYPTR(hh, pRet, pSynonym->zWord, strlen(pSynonym->zWord),
                      pSynonym);
    } else {
      // Existing synonym, add expansion string.
      utarray_push_back(pSynonym->zExpansions, &zSynonym);
    }

    log_debug("fetched %s -> %s\n", zWord, zSynonym);
  }

  sqlite3_finalize(pStatement);

  *ppSynonymsHash = pRet;
  return rc;
}

int synonyms_context_create(sqlite3 *pDb, fts5_api *pFts5Api,
                            SynonymsTokenizerCreateContext **ppContext) {
  log_debug("\ncreating synonyms tokenizer context\n\n");

  SynonymsTokenizerCreateContext *pRet = NULL;
  SynonymsHash *pSynonymsHash = NULL;
  int rc = SQLITE_OK;

  rc = synonyms_create_table(pDb, NULL);
  if (rc != SQLITE_OK) {
    log_error("Failed to create synonyms table: %s\n", sqlite3_errmsg(pDb));
  }

  rc = synonyms_fetch_all_into_hash(pDb, NULL, &pSynonymsHash);
  if (rc == SQLITE_OK) {
#ifdef DEBUG
    debug_hash(pSynonymsHash);
#endif
  } else {
    log_error("Failed to load synonyms: %s\n", sqlite3_errmsg(pDb));
  }

  if (rc == SQLITE_OK) {
    pRet = sqlite3_malloc(sizeof(SynonymsTokenizerCreateContext));
    if (pRet) {
      memset(pRet, 0, sizeof(SynonymsTokenizerCreateContext));
      pRet->pSynonymsHash = pSynonymsHash;
      pRet->pFts5Api = pFts5Api;
    } else {
      rc = SQLITE_NOMEM;
    }
  }

  if (rc == SQLITE_OK) {
    log_debug("created synonyms context\n");
  } else {
    log_error("there was a problem creating the context\n");
    if (pRet) {
      synonyms_context_delete(pRet);
    }
    pRet = NULL;
  }

  *ppContext = (SynonymsTokenizerCreateContext *)pRet;
  return rc;
}

void synonyms_context_delete(SynonymsTokenizerCreateContext *pContext) {
  if (pContext) {
    log_debug("\ndeleting synonyms context\n");

    SynonymsHash *pSynonym, *tmp = NULL;

    log_debug("-> freeing synonyms hash\n");
    HASH_ITER(hh, pContext->pSynonymsHash, pSynonym, tmp) {
      log_debug("  -> freeing expansions array for %s\n", pSynonym->zWord);
      utarray_free(pSynonym->zExpansions);
      log_debug("  -> deleting hash entry for %s\n", pSynonym->zWord);
      HASH_DEL(pContext->pSynonymsHash, pSynonym);
      sqlite3_free(pSynonym->zWord);
      sqlite3_free(pSynonym);
    }

    sqlite3_free(pContext);
  }
}

void synonyms_tokenizer_delete(Fts5Tokenizer *pTok) {
  if (pTok) {
    log_debug("\ndeleting synonyms tokenizer\n");
    SynonymsTokenizer *p = (SynonymsTokenizer *)pTok;
    if (p->pTokenizer) {
      p->tokenizer.xDelete(p->pTokenizer);
    }
    sqlite3_free(p);
  }
}

int synonyms_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                              Fts5Tokenizer **ppOut) {
  log_debug("\ncreating synonyms tokenizer\n\n");

  SynonymsTokenizerCreateContext *pCreateCtx =
      (SynonymsTokenizerCreateContext *)pCtx;
  fts5_api *pFts5Api = pCreateCtx->pFts5Api;
  SynonymsHash *pSynonymsHash = pCreateCtx->pSynonymsHash;
  SynonymsTokenizer *pRet;
  const char *zBase = SYNONYMS_DEFAULT_PARENT_TOKENIZER;
  void *pUserdata = 0;
  int rc = SQLITE_OK;

  if (nArg > 0) {
    zBase = azArg[0];
    log_debug("synonyms tokenizer has base %s\n", zBase);
  }

  pRet = sqlite3_malloc(sizeof(SynonymsTokenizer));
  if (pRet) {
    memset(pRet, 0, sizeof(SynonymsTokenizer));
    pRet->pSynonymsHash = pSynonymsHash;

    // set parent tokenizer module
    rc =
        pFts5Api->xFindTokenizer(pFts5Api, zBase, &pUserdata, &pRet->tokenizer);
  } else {
    rc = SQLITE_NOMEM;
  }

  if (rc == SQLITE_OK) {
    int nArg2 = (nArg > 0 ? nArg - 1 : 0);
    const char **azArg2 = (nArg2 ? &azArg[1] : NULL);

    log_debug("creating %s parent tokenizer for synonyms\n", zBase);
    // set parent tokenizer instance, pass through all args
    rc = pRet->tokenizer.xCreate(pUserdata, azArg2, nArg2, &pRet->pTokenizer);
  }

  if (rc != SQLITE_OK) {
    log_debug("there was a problem creating the tokenizer\n");
    synonyms_tokenizer_delete((Fts5Tokenizer *)pRet);
    pRet = NULL;
  }

  log_debug("created synonyms tokenizer\n");
  *ppOut = (Fts5Tokenizer *)pRet;
  return rc;
}

static int synonyms_tokenize_callback(void *pCtx, int tflags,
                                      const char *pToken, int nToken,
                                      int iStart, int iEnd) {
  SynonymsCallbackContext *p = (SynonymsCallbackContext *)pCtx;

  // create synonyms in queries
  if (p->flags == FTS5_TOKENIZE_QUERY) {
    log_debug("Query context, expanding synonyms for %.*s\n", nToken, pToken);
    int rc;

    // add source token
    rc = p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);

    // Don't look for synonyms for stop-words.
    if (nToken > 0 && (nToken > 1 || pToken[0] != '\0')) {
      // Token string may or may not be null-terminated.
      int nWordSize = pToken[nToken - 1] == '\0' ? nToken : nToken + 1;
      char *zWord = sqlite3_malloc(nWordSize);
      zWord = strncpy(zWord, pToken, nToken);

      // If string was not null-terminated, ensure it is. We need a
      // null-terminated string to do our hash lookup.
      zWord[nWordSize - 1] = '\0';

      // look up any synonyms
      SynonymsHash *pSynonym = NULL;
      HASH_FIND_STR(p->pSynonymsHash, zWord, pSynonym);
      if (pSynonym != NULL) {
        log_debug("found synonyms for %s\n", zWord);
        char **azExpansions = NULL;
        while ((azExpansions = (char **)utarray_next(pSynonym->zExpansions,
                                                     azExpansions))) {
          rc = p->xToken(p->pCtx, FTS5_TOKEN_COLOCATED, *azExpansions,
                         (int)strlen(*azExpansions), iStart, iEnd);
        }
      }

      sqlite3_free(zWord);
    }

    return rc;
  }

  // pass through with no synonyms
  return p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);
}

int synonyms_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                int flags, const char *pText, int nText,
                                int (*xToken)(void *, int, const char *,
                                              int nToken, int iStart,
                                              int iEnd)) {
  SynonymsTokenizer *p = (SynonymsTokenizer *)pTokenizer;
  SynonymsCallbackContext sCtx;

  sCtx.pSynonymsHash = p->pSynonymsHash;
  sCtx.xToken = xToken;
  sCtx.pCtx = pCtx;
  sCtx.flags = flags;

  return p->tokenizer.xTokenize(p->pTokenizer, (void *)&sCtx, flags, pText,
                                nText, synonyms_tokenize_callback);
}

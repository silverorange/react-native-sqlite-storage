#include <sqlite3ext.h>
#include <string.h>

#include "debug.h"
#include "fts5.h"
#include "meta.h"
#include "synonyms.h"
#include "utarray.h"
#include "uthash.h"

#define SYNONYMS_DEFAULT_PARENT_TOKENIZER "phrases"
#define SYNONYMS_DEFAULT_TABLE_NAME "fts5_synonyms"

typedef struct SynonymsHash SynonymsHash;
struct SynonymsHash {
  char *pWord;
  unsigned int nWordLength;
  UT_array *pExpansions;
  UT_hash_handle hh;
};

typedef struct SynonymsTokenizerCreateContext SynonymsTokenizerCreateContext;
struct SynonymsTokenizerCreateContext {
  SynonymsHash *pSynonymsHash; /* hash of loaded synonyms */
  fts5_api *pFts5Api;          /* fts5 api */
  sqlite3 *pDb;                /* database, so we can update the hash table */
  int nLastUpdated;            /* last updated timestamp */
};

typedef struct SynonymsTokenizer SynonymsTokenizer;
struct SynonymsTokenizer {
  fts5_tokenizer tokenizer;                 /* Parent tokenizer module */
  Fts5Tokenizer *pTokenizer;                /* Parent tokenizer instance */
  SynonymsTokenizerCreateContext *pContext; /* creation context */
};

typedef struct SynonymsCallbackContext SynonymsCallbackContext;
struct SynonymsCallbackContext {
  void *pCtx;
  int (*xToken)(void *, int, const char *, int, int, int);
  int flags;
  SynonymsHash *pSynonymsHash;
};

#ifdef SQLITE_TOKENIZER_DEBUG
static void debug_synonyms_hash(SynonymsHash *pSynonymsHash) {
  SynonymsHash *pSynonym, *tmp = NULL;
  HASH_ITER(hh, pSynonymsHash, pSynonym, tmp) {
    char **p = NULL;
    log_debug("\n  %.*s\n", pSynonym->nWordLength, pSynonym->pWord);
    while ((p = (char **)utarray_next(pSynonym->pExpansions, p))) {
      log_debug("   -> %s\n", *p);
    }
  }
}
#endif

static int synonyms_create_table(sqlite3 *pDb, const char *zTableName) {
  int rc = SQLITE_OK;

  // It would be nice in future to get the virtual table name from FTS5 and
  // create TableName_synonyms.
  if (zTableName == NULL) {
    zTableName = (const char *)SYNONYMS_DEFAULT_TABLE_NAME;
  }

  const char *zStatementTemplate = "CREATE TABLE IF NOT EXISTS %s ("
                                   "  word TEXT NOT NULL, "
                                   "  expansion TEXT NOT NULL, "
                                   "  PRIMARY KEY (word, expansion)"
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
    log_debug("[synonyms] Running SQL \"%s\"\n", zStatementSql);
    rc = sqlite3_exec(pDb, zStatementSql, NULL, NULL, NULL);
    sqlite3_free(zStatementSql);
  }

  if (rc != SQLITE_OK) {
    log_error("[synonyms] Failed to execute statement: %s\n",
              sqlite3_errmsg(pDb));
  } else {
    log_debug("[synonyms] Created \"%s\" table\n", zTableName);
  }

  return rc;
}

static int synonyms_fetch_all_into_hash(sqlite3 *pDb, const char *zTableName,
                                        SynonymsHash **ppSynonymsHash) {
  SynonymsHash *pRet = NULL;
  sqlite3_stmt *pStatement;
  int rc = SQLITE_OK;

  if (zTableName == NULL) {
    zTableName = (const char *)SYNONYMS_DEFAULT_TABLE_NAME;
  }

  const char *zStatementTemplate =
      "SELECT word, expansion FROM %s ORDER BY word;";

  // Add +1 for null terminator, -2 for removing %s replacement.
  const size_t nStatementLength =
      strlen(zStatementTemplate) + strlen(zTableName) - 1;

  char *zStatementSql = (char *)sqlite3_malloc((int)nStatementLength);
  if (zStatementSql == NULL) {
    rc = SQLITE_NOMEM;
    *ppSynonymsHash = pRet;
    return rc;
  }

  sqlite3_snprintf((int)nStatementLength, zStatementSql, zStatementTemplate,
                   zTableName);

  log_debug("[synonyms] Running SQL \"%s\"\n", zStatementSql);

  rc = sqlite3_prepare_v2(pDb, zStatementSql, -1, &pStatement, 0);
  if (rc != SQLITE_OK) {
    log_error("[synonyms] Failed to execute statement: %s\n",
              sqlite3_errmsg(pDb));
    sqlite3_free(zStatementSql);
    *ppSynonymsHash = pRet;
    return rc;
  }

  log_debug("[synonyms] Fetched data from \"%s\" table\n", zTableName);

  int step = SQLITE_OK;
  while ((step = sqlite3_step(pStatement)) == SQLITE_ROW) {
    const char *zTempWord = (const char *)sqlite3_column_text(pStatement, 0);
    char *zSynonym = strdup((const char *)sqlite3_column_text(pStatement, 1));
    const unsigned int nLength = (unsigned int)strlen(zTempWord);

    // Get length-delimited string for hash key.
    char *pWord = (char *)sqlite3_malloc(sizeof(char) * nLength);
    strncpy(pWord, zTempWord, nLength);

    SynonymsHash *pSynonym = NULL;
    HASH_FIND(hh, pRet, pWord, nLength, pSynonym);
    if (pSynonym == NULL) {
      // New synonym, we need to create a hash entry.
      pSynonym = (SynonymsHash *)sqlite3_malloc(sizeof(SynonymsHash));
      if (pSynonym == NULL) {
        rc = SQLITE_NOMEM;
        // TODO break and free everthing, return null
      }
      pSynonym->pWord = pWord;
      pSynonym->nWordLength = nLength;
      utarray_new(pSynonym->pExpansions, &ut_str_icd);
      utarray_push_back(pSynonym->pExpansions, &zSynonym);
      HASH_ADD_KEYPTR(hh, pRet, pSynonym->pWord, nLength, pSynonym);
    } else {
      // Existing synonym, add expansion string.
      utarray_push_back(pSynonym->pExpansions, &zSynonym);
    }

    log_debug("  %.*s -> %s\n", nLength, pWord, zSynonym);
  }

  sqlite3_finalize(pStatement);

  *ppSynonymsHash = pRet;
  return rc;
}

static void synonyms_context_delete_hash(SynonymsHash *pSynonymsHash) {
  if (pSynonymsHash) {
    SynonymsHash *pSynonym, *tmp = NULL;

    log_debug("  freeing synonyms hash\n");
    HASH_ITER(hh, pSynonymsHash, pSynonym, tmp) {
      log_debug("  - freeing expansions array for \"%.*s\"\n",
                pSynonym->nWordLength, pSynonym->pWord);
      utarray_free(pSynonym->pExpansions);
      log_debug("    deleting hash entry for \"%.*s\"\n", pSynonym->nWordLength,
                pSynonym->pWord);
      HASH_DEL(pSynonymsHash, pSynonym);
      sqlite3_free(pSynonym->pWord);
      sqlite3_free(pSynonym);
    }
  }
}

static int synonyms_context_update(SynonymsTokenizerCreateContext *pContext) {
  int rc = SQLITE_OK;
  int nLastUpdated = 0;

  rc = meta_needs_update(pContext->pDb, "synonyms", pContext->nLastUpdated,
                         &nLastUpdated, NULL);
  if (rc != SQLITE_OK) {
    log_error("[synonyms] Failed to check synonym cache validity: %s\n",
              sqlite3_errmsg(pContext->pDb));
    return rc;
  }

  if (pContext->nLastUpdated == 0 || nLastUpdated > 0) {
    log_error("[synonyms] Updating synonyms context\n");
    synonyms_context_delete_hash(pContext->pSynonymsHash);
    rc = synonyms_fetch_all_into_hash(pContext->pDb, NULL,
                                      &(pContext->pSynonymsHash));
    if (rc == SQLITE_OK) {
#ifdef SQLITE_TOKENIZER_DEBUG
      debug_synonyms_hash(pContext->pSynonymsHash);
#endif
    } else {
      log_error("[synonyms] Failed to load synonyms: %s\n",
                sqlite3_errmsg(pContext->pDb));
    }
    pContext->nLastUpdated = nLastUpdated;
  }

  return rc;
}

static void synonyms_context_delete(SynonymsTokenizerCreateContext *pContext) {
  if (pContext) {
    log_debug("[synonyms] Deleting synonyms context\n");
    synonyms_context_delete_hash(pContext->pSynonymsHash);
    sqlite3_free(pContext);
  }
}

static int synonyms_context_create(sqlite3 *pDb, fts5_api *pFts5Api,
                                   SynonymsTokenizerCreateContext **ppContext) {
  log_debug("[synonyms] Creating synonyms context\n");

  SynonymsTokenizerCreateContext *pRet = NULL;
  int rc = SQLITE_OK;

  rc = meta_create_table(pDb, NULL);
  if (rc != SQLITE_OK) {
    log_error("[synonyms] Failed to create tokenizer meta table: %s\n",
              sqlite3_errmsg(pDb));
  }

  rc = synonyms_create_table(pDb, NULL);
  if (rc != SQLITE_OK) {
    log_error("[synonyms] Failed to create synonyms table: %s\n",
              sqlite3_errmsg(pDb));
  }

  if (rc == SQLITE_OK) {
    pRet = (SynonymsTokenizerCreateContext *)sqlite3_malloc(
        sizeof(SynonymsTokenizerCreateContext));

    if (pRet) {
      memset(pRet, 0, sizeof(SynonymsTokenizerCreateContext));

      pRet->pSynonymsHash = NULL;
      pRet->pFts5Api = pFts5Api;
      pRet->nLastUpdated = 0;
      pRet->pDb = pDb;
    } else {
      rc = SQLITE_NOMEM;
    }
  }

  if (rc == SQLITE_OK) {
    log_debug("[synonyms] Created synonyms context\n");
  } else {
    log_error("[synonyms] There was a problem creating the synonyms context\n");
    if (pRet) {
      synonyms_context_delete(pRet);
    }
    pRet = NULL;
  }

  *ppContext = (SynonymsTokenizerCreateContext *)pRet;
  return rc;
}

static void synonyms_tokenizer_delete(Fts5Tokenizer *pTok) {
  if (pTok) {
    log_debug("[synonyms] Deleting synonyms tokenizer\n");
    SynonymsTokenizer *p = (SynonymsTokenizer *)pTok;
    if (p->pTokenizer) {
      p->tokenizer.xDelete(p->pTokenizer);
    }
    sqlite3_free(p);
  }
}

static int synonyms_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                                     Fts5Tokenizer **ppOut) {
  log_debug("[synonyms] Creating synonyms tokenizer\n");

  SynonymsTokenizerCreateContext *pCreateCtx =
      (SynonymsTokenizerCreateContext *)pCtx;
  fts5_api *pFts5Api = pCreateCtx->pFts5Api;
  SynonymsTokenizer *pRet;
  const char *zBase = (const char *)SYNONYMS_DEFAULT_PARENT_TOKENIZER;
  void *pUserdata = 0;
  int rc = SQLITE_OK;

  if (nArg > 0) {
    zBase = azArg[0];
    log_debug("  synonyms tokenizer has base \"%s\"\n", zBase);
  }

  pRet = (SynonymsTokenizer *)sqlite3_malloc(sizeof(SynonymsTokenizer));
  if (pRet) {
    memset(pRet, 0, sizeof(SynonymsTokenizer));
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

    log_debug("  creating \"%s\" parent tokenizer for synonyms\n", zBase);
    // set parent tokenizer instance, pass through all args
    rc = pRet->tokenizer.xCreate(pUserdata, azArg2, nArg2, &pRet->pTokenizer);
  }

  if (rc != SQLITE_OK) {
    log_error(
        "[synonyms] There was a problem creating the synonyms tokenizer\n");
    synonyms_tokenizer_delete((Fts5Tokenizer *)pRet);
    pRet = NULL;
  }

  log_debug("  created synonyms tokenizer\n");
  *ppOut = (Fts5Tokenizer *)pRet;
  return rc;
}

static int synonyms_tokenize_callback(void *pCtx, int tflags,
                                      const char *pToken, int nToken,
                                      int iStart, int iEnd) {
  SynonymsCallbackContext *p = (SynonymsCallbackContext *)pCtx;

  // create synonyms in queries
  if (p->flags == FTS5_TOKENIZE_QUERY) {
    log_debug("[synonyms] Expanding synonyms for \"%.*s\"\n", nToken, pToken);
    int rc;

    // add source token
    rc = p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);

    // Don't look for synonyms for stop-words.
    if (nToken > 0 && (nToken > 1 || pToken[0] != '\0')) {
      // Token string may or may not be null-terminated.
      unsigned int nWordLength =
          pToken[nToken - 1] == '\0' ? nToken - 1 : nToken;

      // look up any synonyms
      SynonymsHash *pSynonym = NULL;
      HASH_FIND(hh, p->pSynonymsHash, pToken, nWordLength, pSynonym);
      if (pSynonym != NULL) {
        log_debug("  found synonyms for \"%.*s\"\n", nWordLength, pToken);
        char **azExpansions = NULL;
        while ((azExpansions = (char **)utarray_next(pSynonym->pExpansions,
                                                     azExpansions))) {
          rc = p->xToken(p->pCtx, FTS5_TOKEN_COLOCATED, *azExpansions,
                         (int)strlen(*azExpansions), iStart, iEnd);
        }
      }
    }

    return rc;
  }

  // pass through with no synonyms
  return p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);
}

static int synonyms_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                       int flags, const char *pText, int nText,
                                       int (*xToken)(void *, int, const char *,
                                                     int nToken, int iStart,
                                                     int iEnd)) {
  SynonymsTokenizer *p = (SynonymsTokenizer *)pTokenizer;
  SynonymsCallbackContext sCtx;

  if (flags == FTS5_TOKENIZE_QUERY) {
    if (synonyms_context_update(p->pContext) != SQLITE_OK) {
      // TODO
    };
  }

  sCtx.pSynonymsHash = p->pContext->pSynonymsHash;
  sCtx.xToken = xToken;
  sCtx.pCtx = pCtx;
  sCtx.flags = flags;

  return p->tokenizer.xTokenize(p->pTokenizer, (void *)&sCtx, flags, pText,
                                nText, synonyms_tokenize_callback);
}

#ifdef _WIN32
__declspec(dllexport)
#endif

    int sqlite3_synonyms_init(sqlite3 *pDb, char **pzError,
                              const sqlite3_api_routines *pApi) {

  SQLITE_EXTENSION_INIT2(pApi);

  fts5_api *pFtsApi = fts5_api_from_db(pDb);

  static fts5_tokenizer sTokenizer = {synonyms_tokenizer_create,
                                      synonyms_tokenizer_delete,
                                      synonyms_tokenizer_tokenize};

  SynonymsTokenizerCreateContext *pContext;
  synonyms_context_create(pDb, pFtsApi, &pContext);

  if (pFtsApi) {
    pFtsApi->xCreateTokenizer(pFtsApi, "synonyms", (void *)pContext,
                              &sTokenizer,
                              (void (*)(void *))(&synonyms_context_delete));
    return SQLITE_OK;
  }

  *pzError = sqlite3_mprintf("Can't find FTS5 extension.");

  return SQLITE_ERROR;
}

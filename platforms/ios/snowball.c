#include <libstemmer.h>
#include <sqlite3ext.h>
#include <string.h>

#include "debug.h"
#include "fts5.h"
#include "snowball.h"

#define SNOWBALL_MIN_TOKEN_LEN (3)
#define SNOWBALL_MAX_TOKEN_LEN (64)
#define SNOWBALL_DEFAULT_LANGUAGE "english"
#define SNOWBALL_DEFAULT_PARENT_TOKENIZER "stopwords"

typedef struct SnowballCallbackContext SnowballCallbackContext;
struct SnowballCallbackContext {
  void *pCtx;

  fts5_tokenizer nextTokenizerModule;
  Fts5Tokenizer *nextTokenizerInstance;

  int (*xToken)(void *, int, const char *, int, int, int);

  struct sb_stemmer **stemmers;
};

static int is_valid_language(const char *name) {
  const char **languages;

  languages = sb_stemmer_list();
  while (*languages != NULL) {
    if (strcasecmp(*languages, name) == 0) {
      return 1;
    }
    languages++;
  }
  return 0;
}

static int process_list_languages(const char **azArg, int nArg, int *nextArg,
                                  SnowballCallbackContext *snow) {
  const char **azLanguages;
  const char *azDefaultLanguages[] = {SNOWBALL_DEFAULT_LANGUAGE};
  int i;

  // find the position of the last language in the list
  for (i = 0; i < nArg; i++) {
    if (!is_valid_language(azArg[i])) {
      break;
    }
  }

  *nextArg = i;
  int nLanguages = i;

  if (nLanguages == 0) {
    azLanguages = azDefaultLanguages;
    nLanguages = 1;
  } else {
    azLanguages = azArg;
  }

  log_debug("[snowball] language is %s\n", azLanguages[0]);

  snow->stemmers = (struct sb_stemmer **)sqlite3_malloc(
      (nLanguages + 1) * sizeof(struct sb_stemmer *));

  if (!snow->stemmers) {
    return SQLITE_NOMEM;
  }

  snow->stemmers[nLanguages] = NULL; // terminate the list

  for (i = 0; i < nLanguages; i++) {
    // UTF-8 encoding is used by default.
    snow->stemmers[i] = sb_stemmer_new(azLanguages[i], NULL);
    if (!snow->stemmers[i]) {
      return SQLITE_ERROR;
    }
  }

  return SQLITE_OK;
}

static void snowball_tokenizer_delete(Fts5Tokenizer *pTok) {
  if (pTok) {
    SnowballCallbackContext *p = (SnowballCallbackContext *)pTok;

    if (p->stemmers) {
      for (int i = 0; p->stemmers[i] != NULL; i++) {
        sb_stemmer_delete(p->stemmers[i]);
      }

      sqlite3_free(p->stemmers);
    }

    if (p->nextTokenizerInstance) {
      p->nextTokenizerModule.xDelete(p->nextTokenizerInstance);
    }

    sqlite3_free(p);
  }
}

static int snowball_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                                     Fts5Tokenizer **ppOut) {
  SnowballCallbackContext *result;
  fts5_api *pApi = (fts5_api *)pCtx;
  void *pUserdata = 0;
  int rc = SQLITE_OK;
  int nextArg;
  const char *zBase = (const char *){SNOWBALL_DEFAULT_PARENT_TOKENIZER};
  result = (SnowballCallbackContext *)sqlite3_malloc(
      sizeof(SnowballCallbackContext));

  if (result) {
    memset(result, 0, sizeof(SnowballCallbackContext));
    rc = process_list_languages(azArg, nArg, &nextArg, result);
  } else {
    rc = SQLITE_NOMEM;
  }

  if (rc == SQLITE_OK) {
    if (nArg > nextArg) {
      zBase = azArg[nextArg];
    }
    rc = pApi->xFindTokenizer(pApi, zBase, &pUserdata,
                              &result->nextTokenizerModule);
  }

  if (rc == SQLITE_OK) {
    int nArg2 = (nArg > nextArg + 1 ? nArg - nextArg - 1 : 0);
    const char **azArg2 = (nArg2 ? &azArg[nextArg + 1] : 0);
    rc = result->nextTokenizerModule.xCreate(pUserdata, azArg2, nArg2,
                                             &result->nextTokenizerInstance);
  }

  if (rc != SQLITE_OK) {
    snowball_tokenizer_delete((Fts5Tokenizer *)result);
    result = NULL;
  }

  *ppOut = (Fts5Tokenizer *)result;
  return rc;
}

static int snowball_tokenizer_callback(void *pCtx, int tflags,
                                       const char *pToken, int nToken,
                                       int iStart, int iEnd) {
  SnowballCallbackContext *p = (SnowballCallbackContext *)pCtx;

  if (nToken > SNOWBALL_MAX_TOKEN_LEN || nToken <= SNOWBALL_MIN_TOKEN_LEN) {
    return p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);
  }
  log_debug("[snowball] stemming %.*s to ", nToken, pToken);
  int nBuf = nToken;
  sb_symbol *stemmed = NULL;
  struct sb_stemmer **stemmer;
  char tokenBuf[SNOWBALL_MAX_TOKEN_LEN];

  memcpy(tokenBuf, pToken, nToken);
  stemmer = p->stemmers;

  while (*stemmer) {
    stemmed =
        (sb_symbol *)sb_stemmer_stem(*stemmer, (unsigned char *)tokenBuf, nBuf);
    nBuf = sb_stemmer_length(*stemmer);
    if (nBuf != nToken) {
      break;
    }
    stemmer++;
  }

  log_debug("'%.*s'\n", nBuf, stemmed);

  return p->xToken(p->pCtx, tflags, (char *)stemmed, nBuf, iStart, iEnd);
}

static int snowball_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                       int flags, const char *pText, int nText,
                                       int (*xToken)(void *, int, const char *,
                                                     int nToken, int iStart,
                                                     int iEnd)) {
  SnowballCallbackContext *p = (SnowballCallbackContext *)pTokenizer;

  p->xToken = xToken;
  p->pCtx = pCtx;

  return p->nextTokenizerModule.xTokenize(p->nextTokenizerInstance, (void *)p,
                                          flags, pText, nText,
                                          snowball_tokenizer_callback);
}

#ifdef _WIN32
__declspec(dllexport)
#endif

    int sqlite3_snowball_init(sqlite3 *pDb, char **pzError,
                              const sqlite3_api_routines *pApi) {

  SQLITE_EXTENSION_INIT2(pApi);

  fts5_api *pFtsApi = fts5_api_from_db(pDb);

  static fts5_tokenizer sTokenizer = {snowball_tokenizer_create,
                                      snowball_tokenizer_delete,
                                      snowball_tokenizer_tokenize};

  if (pFtsApi) {
    pFtsApi->xCreateTokenizer(pFtsApi, "snowball", pFtsApi, &sTokenizer, NULL);
    return SQLITE_OK;
  }

  *pzError = sqlite3_mprintf("Can't find FTS5 extension.");

  return SQLITE_ERROR;
}

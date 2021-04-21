#include "stopwords.h"
#include "debug.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

typedef struct StopWordsTokenizer StopWordsTokenizer;
struct StopWordsTokenizer {
  fts5_tokenizer tokenizer;  /* Parent tokenizer module */
  Fts5Tokenizer *pTokenizer; /* Parent tokenizer instance */
};

typedef struct StopWordsCallbackContext StopWordsCallbackContext;
struct StopWordsCallbackContext {
  void *pCtx;
  int (*xToken)(void *, int, const char *, int, int, int);
  int flags;
};

const char *STOP_WORDS_DEFAULT_PARENT_TOKENIZER = "porter";

void stop_words_tokenizer_delete(Fts5Tokenizer *pTok) {
  if (pTok) {
    log_debug("\ndeleting stop-words tokenizer\n");
    StopWordsTokenizer *p = (StopWordsTokenizer *)pTok;
    if (p->pTokenizer) {
      p->tokenizer.xDelete(p->pTokenizer);
    }
    sqlite3_free(p);
  }
}

int stop_words_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                                Fts5Tokenizer **ppOut) {
  log_debug("\ncreating stop-words tokenizer\n\n");
  StopWordsTokenizerCreateContext *pCreateCtx =
      (StopWordsTokenizerCreateContext *)pCtx;

  fts5_api *pFts5Api = pCreateCtx->pFts5Api;
  StopWordsTokenizer *pRet;
  const char *zBase = STOP_WORDS_DEFAULT_PARENT_TOKENIZER;
  void *pUserdata = 0;
  int rc = SQLITE_OK;

  if (nArg > 0) {
    zBase = azArg[0];
    log_debug("stopwords tokenizer has base %s\n", zBase);
  }

  pRet = sqlite3_malloc(sizeof(StopWordsTokenizer));
  if (pRet) {
    memset(pRet, 0, sizeof(StopWordsTokenizer));

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
    rc = pRet->tokenizer.xCreate(pUserdata, azArg2, nArg2, &pRet->pTokenizer);
  }

  if (rc != SQLITE_OK) {
    log_debug("there was a problem creating the tokenizer\n");
    stop_words_tokenizer_delete((Fts5Tokenizer *)pRet);
    pRet = NULL;
  }

  log_debug("created stop-words tokenizer\n");
  *ppOut = (Fts5Tokenizer *)pRet;
  return rc;
}

const char *const azStopWords[] = {
    "a",    "an",   "and",  "are",  "as",   "at",    "be",   "but",   "by",
    "for",  "if",   "in",   "into", "is",   "it",    "no",   "not",   "of",
    "on",   "or",   "such", "that", "the",  "their", "then", "there", "these",
    "they", "this", "to",   "was",  "will", "with"};

static int is_stop_word(const char *pToken, int nToken) {
  const int nLength = sizeof(azStopWords) / sizeof(azStopWords[0]);
  // Token strings may or may-not be null-terminated
  const size_t nTokenLength =
      pToken[nToken - 1] == '\0' ? strlen(pToken) : nToken;

  for (int i = 0; i < nLength; i++) {
    if (strlen(azStopWords[i]) == nTokenLength &&
        strncmp(pToken, azStopWords[i], nToken) == 0) {
      log_debug("stop word -> %.*s\n", nToken, pToken);

      return 1;
    }
  }

  return 0;
}

static int stop_words_tokenize_callback(void *pCtx, int tflags,
                                        const char *pToken, int nToken,
                                        int iStart, int iEnd) {
  StopWordsCallbackContext *p = (StopWordsCallbackContext *)pCtx;

  if (is_stop_word(pToken, nToken) == 1) {
    return SQLITE_OK;
  }

  return p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);
}

int stop_words_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                  int flags, const char *pText, int nText,
                                  int (*xToken)(void *, int, const char *,
                                                int nToken, int iStart,
                                                int iEnd)) {
  StopWordsTokenizer *p = (StopWordsTokenizer *)pTokenizer;
  StopWordsCallbackContext sCtx;

  sCtx.xToken = xToken;
  sCtx.pCtx = pCtx;
  sCtx.flags = flags;

  return p->tokenizer.xTokenize(p->pTokenizer, (void *)&sCtx, flags, pText,
                                nText, stop_words_tokenize_callback);
}

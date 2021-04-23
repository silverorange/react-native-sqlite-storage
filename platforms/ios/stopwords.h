#ifndef STOPWORDS_H
#define STOPWORDS_H

#include "utarray.h"
#include <sqlite3.h>

typedef struct StopWordsTokenizerCreateContext StopWordsTokenizerCreateContext;
struct StopWordsTokenizerCreateContext {
  fts5_api *pFts5Api; /* fts5 api */
  sqlite3 *pDb;       /* database, so we can update the hash table */
  UT_array *paWords;  /* list of stopwords */
  int nLastUpdated;   /* last updated date */
};
int stopwords_context_create(sqlite3 *pDb, fts5_api *pFts5Api,
                             StopWordsTokenizerCreateContext **ppContext);

void stopwords_context_delete(StopWordsTokenizerCreateContext *pContext);

void stopwords_tokenizer_delete(Fts5Tokenizer *pTok);

int stopwords_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                               Fts5Tokenizer **ppOut);

int stopwords_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                 int flags, const char *pText, int nText,
                                 int (*xToken)(void *, int, const char *,
                                               int nToken, int iStart,
                                               int iEnd));

#endif

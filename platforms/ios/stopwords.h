#ifndef STOPWORDS_H
#define STOPWORDS_H

#include "uthash.h"
#include <sqlite3.h>

typedef struct StopwordsHash StopwordsHash;
struct StopwordsHash {
  char *pWord;
  int nWordLength;
  UT_hash_handle hh;
};

typedef struct StopwordsTokenizerCreateContext StopwordsTokenizerCreateContext;
struct StopwordsTokenizerCreateContext {
  fts5_api *pFts5Api;            /* fts5 api */
  sqlite3 *pDb;                  /* database, so we can update the hash table */
  StopwordsHash *pStopwordsHash; /* list of stopwords */
  int nLastUpdated;              /* last updated date */
};

int stopwords_context_create(sqlite3 *pDb, fts5_api *pFts5Api,
                             StopwordsTokenizerCreateContext **ppContext);

void stopwords_context_delete(StopwordsTokenizerCreateContext *pContext);

void stopwords_tokenizer_delete(Fts5Tokenizer *pTok);

int stopwords_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                               Fts5Tokenizer **ppOut);

int stopwords_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                 int flags, const char *pText, int nText,
                                 int (*xToken)(void *, int, const char *,
                                               int nToken, int iStart,
                                               int iEnd));

#endif

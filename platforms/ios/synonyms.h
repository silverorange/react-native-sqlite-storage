#ifndef SYNONYMS_H
#define SYNONYMS_H

#include "utarray.h"
#include "uthash.h"
#include <sqlite3.h>

typedef struct SynonymsHash SynonymsHash;
struct SynonymsHash {
  char *pWord;
  int nWordLength;
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

int synonyms_context_create(sqlite3 *pDb, fts5_api *pFts5Api,
                            SynonymsTokenizerCreateContext **ppContext);
void synonyms_context_delete(SynonymsTokenizerCreateContext *pContext);

void synonyms_tokenizer_delete(Fts5Tokenizer *pTok);

int synonyms_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                              Fts5Tokenizer **ppOut);

int synonyms_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                int flags, const char *pText, int nText,
                                int (*xToken)(void *, int, const char *,
                                              int nToken, int iStart,
                                              int iEnd));

#endif

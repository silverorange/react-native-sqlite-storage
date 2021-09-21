#ifndef PHRASES_H
#define PHRASES_H

#include "uthash.h"
#include <sqlite3.h>

typedef struct PhrasesBufferEntry PhrasesBufferEntry;
struct PhrasesBufferEntry {
  char *pToken;
  int nToken;
  int iStart;
  int iEnd;
};

typedef struct PhrasesHash PhrasesHash;
struct PhrasesHash {
  unsigned int nPhraseLength; /** source phrase character count. */
  unsigned int nRootLength;   /** root phrase character count. */
  unsigned int nPhraseWordCount;
  char *pPhrase; /** source phrase that is collapsed. */
  char *pRoot;   /** root phrase that is emitted for source phrase. */
  UT_hash_handle hh;
};

typedef struct PhrasesTokenizerCreateContext PhrasesTokenizerCreateContext;
struct PhrasesTokenizerCreateContext {
  PhrasesHash *pPhrasesHash;    /** hash of loaded phrases */
  fts5_api *pFts5Api;           /** fts5 api */
  sqlite3 *pDb;                 /** database, so we can update the hash table */
  unsigned int nMaxPhraseWords; /** largest number of words in source phrases */
  int nLastUpdated;             /** last updated timestamp */
  PhrasesBufferEntry *paBuffer; /** token buffer used during tokenization */
};

int phrases_context_create(sqlite3 *pDb, fts5_api *pFts5Api,
                           PhrasesTokenizerCreateContext **ppContext);
void phrases_context_delete(PhrasesTokenizerCreateContext *pContext);

void phrases_tokenizer_delete(Fts5Tokenizer *pTok);

int phrases_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                             Fts5Tokenizer **ppOut);

int phrases_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx, int flags,
                               const char *pText, int nText,
                               int (*xToken)(void *, int, const char *,
                                             int nToken, int iStart, int iEnd));

#endif

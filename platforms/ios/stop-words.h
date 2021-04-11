#ifndef STOP_WORDS_H
#define STOP_WORDS_H

#include <sqlite3.h>

typedef struct StopWordsTokenizerCreateContext StopWordsTokenizerCreateContext;
struct StopWordsTokenizerCreateContext {
  fts5_api *pFts5Api; /* fts5 api */
};

void stop_words_tokenizer_delete(Fts5Tokenizer *pTok);

int stop_words_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                                Fts5Tokenizer **ppOut);

int stop_words_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx,
                                  int flags, const char *pText, int nText,
                                  int (*xToken)(void *, int, const char *,
                                                int nToken, int iStart,
                                                int iEnd));
fts5_tokenizer stop_words_tokenizer;

#endif

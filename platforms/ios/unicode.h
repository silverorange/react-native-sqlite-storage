#ifndef UNICODE_H
#define UNICODE_H

#include <sqlite3.h>

void unicode_tokenizer_delete(Fts5Tokenizer *pTok);

int unicode_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                             Fts5Tokenizer **ppOut);

int unicode_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx, int flags,
                               const char *pText, int nText,
                               int (*xToken)(void *, int, const char *,
                                             int nToken, int iStart, int iEnd));

#endif

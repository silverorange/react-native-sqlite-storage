#include "phrases.h"
#include "debug.h"
#include "fts5.h"
#include "meta.h"
#include "uthash.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

typedef struct PhrasesTokenizer PhrasesTokenizer;
struct PhrasesTokenizer {
  fts5_tokenizer tokenizer;                /* Parent tokenizer module */
  Fts5Tokenizer *pTokenizer;               /* Parent tokenizer instance */
  PhrasesTokenizerCreateContext *pContext; /* creation context */
};

typedef struct PhrasesCallbackContext PhrasesCallbackContext;
struct PhrasesCallbackContext {
  void *pCtx;
  int (*xToken)(void *, int, const char *, int, int, int);
  int flags;
  unsigned int nQueryLength;
  PhrasesHash *pPhrasesHash;
  PhrasesBufferEntry *paBuffer;
  unsigned int iBufferStart;
  unsigned int nBufferLength;
  unsigned int nMaxPhraseWords;
};

const char *PHRASES_DEFAULT_PHRASES_TABLE_NAME = "fts5_phrases";
const char *PHRASES_DEFAULT_PARENT_TOKENIZER = "stopwords";

#ifdef SQLITE_TOKENIZER_DEBUG
static void debug_phrases_hash(PhrasesHash *pPhrasesHash) {
  PhrasesHash *pPhrasePair, *tmp = NULL;
  HASH_ITER(hh, pPhrasesHash, pPhrasePair, tmp) {
    printf("  %.*s -> %.*s\n", pPhrasePair->nPhraseLength, pPhrasePair->pPhrase,
           pPhrasePair->nRootLength, pPhrasePair->pRoot);
  }
}
#endif

static int phrases_create_table(sqlite3 *pDb, const char *zTableName) {
  int rc = SQLITE_OK;

  // It would be nice in future to get the virtual table name from FTS5 and
  // create TableName_phrases.
  if (zTableName == NULL) {
    zTableName = PHRASES_DEFAULT_PHRASES_TABLE_NAME;
  }

  const char *zStatementTemplate = "CREATE TABLE IF NOT EXISTS %s ("
                                   "  phrase TEXT NOT NULL, "
                                   "  root TEXT NOT NULL, "
                                   "  PRIMARY KEY (phrase, root)"
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
    log_debug("[phrases] Running SQL \"%s\"\n", zStatementSql);
    rc = sqlite3_exec(pDb, zStatementSql, NULL, NULL, NULL);
    sqlite3_free(zStatementSql);
  }

  if (rc != SQLITE_OK) {
    log_error("[phrases] Failed to execute statement: %s\n",
              sqlite3_errmsg(pDb));
  } else {
    log_debug("[phrases] Created \"%s\" table\n", zTableName);
  }

  return rc;
}

static int phrases_fetch_all_into_hash(sqlite3 *pDb, const char *zTableName,
                                       PhrasesHash **ppPhrasesHash) {
  PhrasesHash *pRet = NULL;
  sqlite3_stmt *pStatement;
  int rc = SQLITE_OK;

  if (zTableName == NULL) {
    zTableName = PHRASES_DEFAULT_PHRASES_TABLE_NAME;
  }

  const char *zStatementTemplate = "SELECT phrase, root FROM %s ORDER BY root;";

  // Add +1 for null terminator, -2 for removing %s replacement.
  const size_t nStatementLength =
      strlen(zStatementTemplate) + strlen(zTableName) - 1;

  char *zStatementSql = (char *)sqlite3_malloc((int)nStatementLength);
  if (zStatementSql == NULL) {
    rc = SQLITE_NOMEM;
    *ppPhrasesHash = pRet;
    return rc;
  }

  sqlite3_snprintf((int)nStatementLength, zStatementSql, zStatementTemplate,
                   zTableName);

  log_debug("[phrases] Running SQL \"%s\"\n", zStatementSql);

  rc = sqlite3_prepare_v2(pDb, zStatementSql, -1, &pStatement, 0);
  if (rc != SQLITE_OK) {
    log_error("[phrases] Failed to execute statement: %s\n",
              sqlite3_errmsg(pDb));
    sqlite3_free(zStatementSql);
    *ppPhrasesHash = pRet;
    return rc;
  }

  log_debug("[phrases] Fetched data from \"%s\" table\n", zTableName);

  int step = SQLITE_OK;
  while ((step = sqlite3_step(pStatement)) == SQLITE_ROW) {
    const char *zTempPhrase = (const char *)sqlite3_column_text(pStatement, 0);
    const char *zTempRoot = (const char *)sqlite3_column_text(pStatement, 1);
    const unsigned int nPhraseLength = (unsigned int)strlen(zTempPhrase);
    const unsigned int nRootLength = (unsigned int)strlen(zTempRoot);

    // Get length-delimited string for hash key.
    char *pPhrase = (char *)sqlite3_malloc(sizeof(char) * nPhraseLength);
    strncpy(pPhrase, zTempPhrase, nPhraseLength);

    // Get word count of phrase so we can track the maximum phrase length to
    // set the size of the context buffer.
    unsigned int nPhraseWordCount = 1;
    unsigned int i;
    for (i = 0; i < nPhraseLength; i++) {
      if (pPhrase[i] == ' ') {
        nPhraseWordCount++;
      }
    }

    // Get length-delimited string for phrase root.
    char *pRoot = (char *)sqlite3_malloc(sizeof(char) * nRootLength);
    strncpy(pRoot, zTempRoot, nRootLength);

    PhrasesHash *pPhrasePair =
        (PhrasesHash *)sqlite3_malloc(sizeof(PhrasesHash));
    if (pPhrasePair == NULL) {
      rc = SQLITE_NOMEM;
      // TODO break and free everthing, return null
    }
    pPhrasePair->pPhrase = pPhrase;
    pPhrasePair->nPhraseLength = nPhraseLength;
    pPhrasePair->nPhraseWordCount = nPhraseWordCount;
    pPhrasePair->pRoot = pRoot;
    pPhrasePair->nRootLength = nRootLength;

    HASH_ADD_KEYPTR(hh, pRet, pPhrasePair->pPhrase, nPhraseLength, pPhrasePair);

    log_debug("  %.*s -> %.*s\n", nPhraseLength, pPhrase, nRootLength, pRoot);
  }

  sqlite3_finalize(pStatement);

  *ppPhrasesHash = pRet;
  return rc;
}

static void phrases_context_delete_hash(PhrasesHash *pPhrasesHash) {
  if (pPhrasesHash) {
    PhrasesHash *pPhrasePair, *tmp = NULL;

    log_debug("  freeing phrases hash\n");
    HASH_ITER(hh, pPhrasesHash, pPhrasePair, tmp) {
      log_debug("    deleting hash entry for \"%.*s\"\n",
                pPhrasePair->nPhraseLength, pPhrasePair->pPhrase);
      HASH_DEL(pPhrasesHash, pPhrasePair);
      sqlite3_free(pPhrasePair->pPhrase);
      sqlite3_free(pPhrasePair->pRoot);
      sqlite3_free(pPhrasePair);
    }
  }
}

static int phrases_context_update(PhrasesTokenizerCreateContext *pContext) {
  int rc = SQLITE_OK;
  int nLastUpdated = 0;

  rc = meta_needs_update(pContext->pDb, "phrases", pContext->nLastUpdated,
                         &nLastUpdated, NULL);
  if (rc != SQLITE_OK) {
    log_error("[phrases] Failed to check phrase cache validity: %s\n",
              sqlite3_errmsg(pContext->pDb));
    return rc;
  }

  if (pContext->nLastUpdated == 0 || nLastUpdated > 0) {
    log_error("[phrases] Updating phrases context\n");
    phrases_context_delete_hash(pContext->pPhrasesHash);
    rc = phrases_fetch_all_into_hash(pContext->pDb, NULL,
                                     &(pContext->pPhrasesHash));
    if (rc == SQLITE_OK) {
#ifdef SQLITE_TOKENIZER_DEBUG
      debug_phrases_hash(pContext->pPhrasesHash);
#endif

      // Get maximum number of words in all source phrases to set required
      // buffer size.
      PhrasesHash *pPhrasePair, *tmp = NULL;
      unsigned int nMaxPhraseWords = 0;
      HASH_ITER(hh, pContext->pPhrasesHash, pPhrasePair, tmp) {
        if (pPhrasePair->nPhraseWordCount > nMaxPhraseWords) {
          nMaxPhraseWords = pPhrasePair->nPhraseWordCount;
        }
      }
      log_debug("[phrases] Maximum root phrase words is %d\n", nMaxPhraseWords);

      // Update buffer size if it changed.
      // TODO: Verify the tokenizer can't run concurrently and this single
      // buffer is safe to use. Otherwise we need to make a buffer where we
      // create the callback context.
      if (pContext->nMaxPhraseWords != nMaxPhraseWords) {
        // Free existing buffer.
        if (pContext->paBuffer != NULL) {
          sqlite3_free(pContext->paBuffer);
        }

        pContext->nMaxPhraseWords = nMaxPhraseWords;

        // Allocate a new buffer based on the max word count.
        pContext->paBuffer = (PhrasesBufferEntry *)sqlite3_malloc(
            sizeof(PhrasesBufferEntry) * (nMaxPhraseWords + 1));
      }
    } else {
      log_error("[phrases] Failed to load phrases: %s\n",
                sqlite3_errmsg(pContext->pDb));
    }
    pContext->nLastUpdated = nLastUpdated;
  }

  return rc;
}

int phrases_context_create(sqlite3 *pDb, fts5_api *pFts5Api,
                           PhrasesTokenizerCreateContext **ppContext) {
  log_debug("[phrases] Creating phrases context\n");

  PhrasesTokenizerCreateContext *pRet = NULL;
  int rc = SQLITE_OK;

  rc = meta_create_table(pDb, NULL);
  if (rc != SQLITE_OK) {
    log_error("[phrases] Failed to create tokenizer meta table: %s\n",
              sqlite3_errmsg(pDb));
  }

  rc = phrases_create_table(pDb, NULL);
  if (rc != SQLITE_OK) {
    log_error("[phrases] Failed to create phrases table: %s\n",
              sqlite3_errmsg(pDb));
  }

  if (rc == SQLITE_OK) {
    pRet = (PhrasesTokenizerCreateContext *)sqlite3_malloc(
        sizeof(PhrasesTokenizerCreateContext));

    if (pRet) {
      memset(pRet, 0, sizeof(PhrasesTokenizerCreateContext));

      pRet->pPhrasesHash = NULL;
      pRet->pFts5Api = pFts5Api;
      pRet->nLastUpdated = 0;
      pRet->pDb = pDb;
    } else {
      rc = SQLITE_NOMEM;
    }
  }

  if (rc == SQLITE_OK) {
    log_debug("[phrases] Created phrases context\n");
  } else {
    log_error("[phrases] There was a problem creating the phrases context\n");
    if (pRet) {
      phrases_context_delete(pRet);
    }
    pRet = NULL;
  }

  *ppContext = (PhrasesTokenizerCreateContext *)pRet;
  return rc;
}

void phrases_context_delete(PhrasesTokenizerCreateContext *pContext) {
  if (pContext) {
    log_debug("[phrases] Deleting phrases context\n");
    phrases_context_delete_hash(pContext->pPhrasesHash);
    sqlite3_free(pContext->paBuffer);
    sqlite3_free(pContext);
  }
}

void phrases_tokenizer_delete(Fts5Tokenizer *pTok) {
  if (pTok) {
    log_debug("[phrases] Deleting phrases tokenizer\n");
    PhrasesTokenizer *p = (PhrasesTokenizer *)pTok;
    if (p->pTokenizer) {
      p->tokenizer.xDelete(p->pTokenizer);
    }
    sqlite3_free(p);
  }
}

int phrases_tokenizer_create(void *pCtx, const char **azArg, int nArg,
                             Fts5Tokenizer **ppOut) {
  log_debug("[phrases] Creating phrases tokenizer\n");

  PhrasesTokenizerCreateContext *pCreateCtx =
      (PhrasesTokenizerCreateContext *)pCtx;
  fts5_api *pFts5Api = pCreateCtx->pFts5Api;
  PhrasesTokenizer *pRet;
  const char *zBase = PHRASES_DEFAULT_PARENT_TOKENIZER;
  void *pUserdata = 0;
  int rc = SQLITE_OK;

  if (nArg > 0) {
    zBase = azArg[0];
    log_debug("  phrases tokenizer has base \"%s\"\n", zBase);
  }

  pRet = (PhrasesTokenizer *)sqlite3_malloc(sizeof(PhrasesTokenizer));
  if (pRet) {
    memset(pRet, 0, sizeof(PhrasesTokenizer));
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

    log_debug("  creating \"%s\" parent tokenizer for phrases\n", zBase);
    // set parent tokenizer instance, pass through all args
    rc = pRet->tokenizer.xCreate(pUserdata, azArg2, nArg2, &pRet->pTokenizer);
  }

  if (rc != SQLITE_OK) {
    log_error("[phrases] There was a problem creating the phrases tokenizer\n");
    phrases_tokenizer_delete((Fts5Tokenizer *)pRet);
    pRet = NULL;
  }

  log_debug("  created phrases tokenizer\n");
  *ppOut = (Fts5Tokenizer *)pRet;
  return rc;
}

/**
 * @param p the callback context.
 * @param iOffset offset. A negative offset is counted from the end of the
 * buffer.
 */
unsigned int phrases_buffer_index(PhrasesCallbackContext *p, int iOffset) {
  if (iOffset < 0) {
    iOffset = p->nBufferLength + iOffset;
  }

  if (iOffset < 0) {
    iOffset = 0;
  }

  return (p->iBufferStart + (unsigned int)iOffset) % (p->nMaxPhraseWords + 1);
}

unsigned int phrases_buffer_next_index(PhrasesCallbackContext *p,
                                       unsigned int iCurrentIndex) {
  return (iCurrentIndex + 1) % (p->nMaxPhraseWords + 1);
}

void phrases_buffer_match(PhrasesCallbackContext *p,
                          PhrasesHash **pMatchedPhrase,
                          unsigned int *nMatchedPhraseWords) {
  // This is the largest number of words we should check in the current buffer.
  // If is the smaller of the current buffer length and the maximum phrase
  // length.
  unsigned int iLength = (p->nMaxPhraseWords < p->nBufferLength)
                             ? p->nMaxPhraseWords
                             : p->nBufferLength;

  // Initialize starting phrase length to 1 word (or 0 if the buffer is empty).
  unsigned int nMaxPhraseLength = (iLength > 0) ? iLength - 1 : 0;
  unsigned int i;
  unsigned int iIndex;

  // Offsets within the largest buffered phrase string. These are used to
  // do the hash lookup against the known phrases.
  unsigned int *paStarts;
  unsigned int *paLengths;

  paStarts = (unsigned int *)sqlite3_malloc(sizeof(int) * iLength);
  paLengths = (unsigned int *)sqlite3_malloc(sizeof(int) * iLength);

  // Allocate a single string with the longest phrase we need to check.
  iIndex = phrases_buffer_index(p, -iLength);
  for (i = 0; i < iLength; i++) {
    PhrasesBufferEntry *pBufferEntry = &(p->paBuffer[iIndex]);
    nMaxPhraseLength += pBufferEntry->nToken;
    iIndex = phrases_buffer_next_index(p, iIndex);
  }

  // Build the string by copying buffer token values into the new string.
  char *pMaxPhrase = (char *)sqlite3_malloc((int)nMaxPhraseLength);

  unsigned int iPos = 0;
  iIndex = phrases_buffer_index(p, -iLength);
  for (i = 0; i < iLength; i++) {
    if (i > 0) {
      // Add a space between words.
      pMaxPhrase[iPos] = ' ';
      iPos++;
    }

    PhrasesBufferEntry *pBufferEntry = &(p->paBuffer[iIndex]);
    strncpy(pMaxPhrase + iPos, pBufferEntry->pToken, pBufferEntry->nToken);

    // Add offsets in reverse order so we can check the shortest phrases first.
    paStarts[iLength - 1 - i] = iPos;
    paLengths[iLength - 1 - i] = nMaxPhraseLength - iPos;

    iPos += pBufferEntry->nToken;
    iIndex = phrases_buffer_next_index(p, iIndex);
  }

  // Check for matched phrases in the hash table using the string and offsets
  // we built.
  for (i = 0; i < iLength; i++) {
    PhrasesHash *pPhrasePair = NULL;

    HASH_FIND(hh, p->pPhrasesHash, pMaxPhrase + paStarts[i], paLengths[i],
              pPhrasePair);
    if (pPhrasePair != NULL) {
      log_debug("[phrases] found \"%.*s\" > \"%.*s\"\n",
                pPhrasePair->nPhraseLength, pPhrasePair->pPhrase,
                pPhrasePair->nRootLength, pPhrasePair->pRoot);

      *pMatchedPhrase = pPhrasePair;
      *nMatchedPhraseWords = i + 1;
      break;
    }
  }

  sqlite3_free(paStarts);
  sqlite3_free(paLengths);
  sqlite3_free(pMaxPhrase);
}

int phrases_buffer_flush(PhrasesCallbackContext *p, int tflags) {
  PhrasesBufferEntry *pBufferEntry;
  unsigned int i;
  int rc = SQLITE_OK;
  unsigned int iIndex = p->iBufferStart;
  for (i = 0; i < p->nBufferLength; i++) {
    pBufferEntry = &(p->paBuffer[iIndex]);

    // Emit a token for each remaining entry in the buffer.
    rc = p->xToken(p->pCtx, tflags, pBufferEntry->pToken, pBufferEntry->nToken,
                   pBufferEntry->iStart, pBufferEntry->iEnd);

    free(pBufferEntry->pToken);
    iIndex = phrases_buffer_next_index(p, iIndex);
  }

  // Reset buffer now that it is flushed.
  p->iBufferStart = 0;
  p->nBufferLength = 0;

  return rc;
}

static int phrases_tokenize_callback(void *pCtx, int tflags, const char *pToken,
                                     int nToken, int iStart, int iEnd) {
  PhrasesCallbackContext *p = (PhrasesCallbackContext *)pCtx;

  int rc = SQLITE_OK;

  // If we get to the final token in the source string and there are still
  // tokens in the buffer, we need to flush the buffer. The final token is
  // a marker token so we ignore it.
  if (tflags & FTS5_TOKEN_FINAL) {
    return phrases_buffer_flush(p, tflags ^ FTS5_TOKEN_FINAL);
  }

  // Push current token into the buffer. The buffer wraps around depending on
  // the index position.
  unsigned int iCurrentIndex = phrases_buffer_index(p, p->nBufferLength);
  PhrasesBufferEntry *pBufferEntry = &(p->paBuffer[iCurrentIndex]);

  pBufferEntry->iEnd = iEnd;
  pBufferEntry->iStart = iStart;
  pBufferEntry->nToken = nToken;

  // Note: The pToken passed to this function is not retained. We copy it so
  // it is retained in the buffer. We need to carefully remember to free it
  // when emiting a token or a root phrase.
  pBufferEntry->pToken = strndup(pToken, nToken);

  p->nBufferLength++;

  unsigned int nMatchedPhraseWords = 0;
  PhrasesHash *pMatchedPhrase = NULL;
  phrases_buffer_match(p, &pMatchedPhrase, &nMatchedPhraseWords);

  if (pMatchedPhrase != NULL) {
    // Get matched phrase token details.
    unsigned int iStartIndex = phrases_buffer_index(p, -nMatchedPhraseWords);
    unsigned int iOriginalStart = p->paBuffer[iStartIndex].iStart;
    unsigned int iOriginalEnd = pBufferEntry->iEnd;

    // Pop matched words from the buffer, then flush the buffer.
    p->nBufferLength -= nMatchedPhraseWords;
    if (p->nBufferLength < 0) {
      p->nBufferLength = 0; // Should not be possible.
    }

    // Free popped phrase words.
    unsigned int iIndex = iStartIndex;
    for (unsigned int i = 0; i < nMatchedPhraseWords; i++) {
      free(p->paBuffer[iIndex].pToken);
      iIndex = phrases_buffer_next_index(p, iIndex);
    }

    rc = phrases_buffer_flush(p, tflags);

    // Finally emit the matched root phrase. Split multiple words into
    // separate tokens.
    unsigned int iRootWordStart = 0;
    for (unsigned int i = 0; i <= pMatchedPhrase->nRootLength; i++) {
      if (i == pMatchedPhrase->nRootLength || pMatchedPhrase->pRoot[i] == ' ') {
        rc = p->xToken(p->pCtx, tflags, pMatchedPhrase->pRoot + iRootWordStart,
                       i - iRootWordStart, iOriginalStart, iOriginalEnd);

        iRootWordStart = i + 1;
      }
    }
  } else if (p->nBufferLength > p->nMaxPhraseWords) {
    // Emit token from start of buffer.
    pBufferEntry = &(p->paBuffer[p->iBufferStart]);
    rc = p->xToken(p->pCtx, tflags, pBufferEntry->pToken, pBufferEntry->nToken,
                   pBufferEntry->iStart, pBufferEntry->iEnd);

    free(pBufferEntry->pToken);

    // Then shift the buffer.
    p->iBufferStart = phrases_buffer_next_index(p, p->iBufferStart);
    p->nBufferLength--;
    if (p->nBufferLength < 0) {
      p->nBufferLength = 0; // This should not happen.
    }
  }

  // If we get to the last word in the source string and there are still
  // tokens in the buffer, we need to flush the buffer.
  if ((unsigned int)iEnd == p->nQueryLength && p->nBufferLength > 0) {
    rc = phrases_buffer_flush(p, tflags);
  }

  return rc;
}

int phrases_tokenizer_tokenize(Fts5Tokenizer *pTokenizer, void *pCtx, int flags,
                               const char *pText, int nText,
                               int (*xToken)(void *, int, const char *,
                                             int nToken, int iStart,
                                             int iEnd)) {
  PhrasesTokenizer *p = (PhrasesTokenizer *)pTokenizer;
  PhrasesCallbackContext sCtx;

  phrases_context_update(p->pContext);

  sCtx.pPhrasesHash = p->pContext->pPhrasesHash;
  sCtx.paBuffer = p->pContext->paBuffer;
  sCtx.iBufferStart = 0;
  sCtx.nBufferLength = 0;
  sCtx.nMaxPhraseWords = p->pContext->nMaxPhraseWords;
  sCtx.xToken = xToken;
  sCtx.pCtx = pCtx;
  sCtx.flags = flags;
  sCtx.nQueryLength = nText;

  return p->tokenizer.xTokenize(p->pTokenizer, (void *)&sCtx, flags, pText,
                                nText, phrases_tokenize_callback);
}

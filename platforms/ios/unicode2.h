#ifndef UNICODE_H
#define UNICODE_H

#ifndef SQLITE_AMALGAMATION

#ifndef UNUSED_PARAM
#define UNUSED_PARAM(X) (void)(X)
#endif

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned short u16;

#endif /* ifndef SQLITE_AMALGAMATION */

int so_sqlite3Fts5UnicodeIsdiacritic(int c);
int so_sqlite3Fts5UnicodeFold(int c, int bRemoveDiacritic);

int so_sqlite3Fts5UnicodeCatParse(const char *, u8 *);
int so_sqlite3Fts5UnicodeCategory(u32 iCode);
void so_sqlite3Fts5UnicodeAscii(u8 *, u8 *);

#endif

/* fts1 has a design flaw which can lead to database corruption (see
** below).  It is recommended not to use it any longer, instead use
** fts3 (or higher).  If you believe that your use of fts1 is safe,
** add -DSQLITE_ENABLE_BROKEN_FTS1=1 to your CFLAGS.
*/
#if (!defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS1)) \
        && !defined(SQLITE_ENABLE_BROKEN_FTS1)
#error fts1 has a design flaw and has been deprecated.
#endif
/* The flaw is that fts1 uses the content table's unaliased rowid as
** the unique docid.  fts1 embeds the rowid in the index it builds,
** and expects the rowid to not change.  The SQLite VACUUM operation
** will renumber such rowids, thereby breaking fts1.  If you are using
** fts1 in a system which has disabled VACUUM, then you can continue
** to use it safely.  Note that PRAGMA auto_vacuum does NOT disable
** VACUUM, though systems using auto_vacuum are unlikely to invoke
** VACUUM.
**
** fts1 should be safe even across VACUUM if you only insert documents
** and never delete.
*/

/* The author disclaims copyright to this source code.
 *
 * This is an SQLite module implementing full-text search.
 */

/*
** The code in this file is only compiled if:
**
**     * The FTS1 module is being built as an extension
**       (in which case SQLITE_CORE is not defined), or
**
**     * The FTS1 module is being built into the core of
**       SQLite (in which case SQLITE_ENABLE_FTS1 is defined).
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS1)

#if defined(SQLITE_ENABLE_FTS1) && !defined(SQLITE_CORE)
# define SQLITE_CORE 1
#endif

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "fts1.h"
#include "fts1_hash.h"
#include "fts1_tokenizer.h"
#include "sqlite3.h"
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1


#if 0
# define TRACE(A)  printf A; fflush(stdout)
#else
# define TRACE(A)
#endif

/* utility functions */

typedef struct StringBuffer {
  int len;      /* length, not including null terminator */
  int alloced;  /* Space allocated for s[] */ 
  char *s;      /* Content of the string */
} StringBuffer;

static void initStringBuffer(StringBuffer *sb){
  sb->len = 0;
  sb->alloced = 100;
  sb->s = malloc(100);
  sb->s[0] = '\0';
}

static void nappend(StringBuffer *sb, const char *zFrom, int nFrom){
  if( sb->len + nFrom >= sb->alloced ){
    sb->alloced = sb->len + nFrom + 100;
    sb->s = realloc(sb->s, sb->alloced+1);
    if( sb->s==0 ){
      initStringBuffer(sb);
      return;
    }
  }
  memcpy(sb->s + sb->len, zFrom, nFrom);
  sb->len += nFrom;
  sb->s[sb->len] = 0;
}
static void append(StringBuffer *sb, const char *zFrom){
  nappend(sb, zFrom, strlen(zFrom));
}

/* We encode variable-length integers in little-endian order using seven bits
 * per byte as follows:
**
** KEY:
**         A = 0xxxxxxx    7 bits of data and one flag bit
**         B = 1xxxxxxx    7 bits of data and one flag bit
**
**  7 bits - A
** 14 bits - BA
** 21 bits - BBA
** and so on.
*/

/* We may need up to VARINT_MAX bytes to store an encoded 64-bit integer. */
#define VARINT_MAX 10

/* Write a 64-bit variable-length integer to memory starting at p[0].
 * The length of data written will be between 1 and VARINT_MAX bytes.
 * The number of bytes written is returned. */
static int putVarint(char *p, sqlite_int64 v){
  unsigned char *q = (unsigned char *) p;
  sqlite_uint64 vu = v;
  do{
    *q++ = (unsigned char) ((vu & 0x7f) | 0x80);
    vu >>= 7;
  }while( vu!=0 );
  q[-1] &= 0x7f;  /* turn off high bit in final byte */
  assert( q - (unsigned char *)p <= VARINT_MAX );
  return (int) (q - (unsigned char *)p);
}

/* Read a 64-bit variable-length integer from memory starting at p[0].
 * Return the number of bytes read, or 0 on error.
 * The value is stored in *v. */
static int getVarint(const char *p, sqlite_int64 *v){
  const unsigned char *q = (const unsigned char *) p;
  sqlite_uint64 x = 0, y = 1;
  while( (*q & 0x80) == 0x80 ){
    x += y * (*q++ & 0x7f);
    y <<= 7;
    if( q - (unsigned char *)p >= VARINT_MAX ){  /* bad data */
      assert( 0 );
      return 0;
    }
  }
  x += y * (*q++);
  *v = (sqlite_int64) x;
  return (int) (q - (unsigned char *)p);
}

static int getVarint32(const char *p, int *pi){
 sqlite_int64 i;
 int ret = getVarint(p, &i);
 *pi = (int) i;
 assert( *pi==i );
 return ret;
}

/*** Document lists ***
 *
 * A document list holds a sorted list of varint-encoded document IDs.
 *
 * A doclist with type DL_POSITIONS_OFFSETS is stored like this:
 *
 * array {
 *   varint docid;
 *   array {
 *     varint position;     (delta from previous position plus POS_BASE)
 *     varint startOffset;  (delta from previous startOffset)
 *     varint endOffset;    (delta from startOffset)
 *   }
 * }
 *
 * Here, array { X } means zero or more occurrences of X, adjacent in memory.
 *
 * A position list may hold positions for text in multiple columns.  A position
 * POS_COLUMN is followed by a varint containing the index of the column for
 * following positions in the list.  Any positions appearing before any
 * occurrences of POS_COLUMN are for column 0.
 *
 * A doclist with type DL_POSITIONS is like the above, but holds only docids
 * and positions without offset information.
 *
 * A doclist with type DL_DOCIDS is like the above, but holds only docids
 * without positions or offset information.
 *
 * On disk, every document list has positions and offsets, so we don't bother
 * to serialize a doclist's type.
 * 
 * We don't yet delta-encode document IDs; doing so will probably be a
 * modest win.
 *
 * NOTE(shess) I've thought of a slightly (1%) better offset encoding.
 * After the first offset, estimate the next offset by using the
 * current token position and the previous token position and offset,
 * offset to handle some variance.  So the estimate would be
 * (iPosition*w->iStartOffset/w->iPosition-64), which is delta-encoded
 * as normal.  Offsets more than 64 chars from the estimate are
 * encoded as the delta to the previous start offset + 128.  An
 * additional tiny increment can be gained by using the end offset of
 * the previous token to make the estimate a tiny bit more precise.
*/

/* It is not safe to call isspace(), tolower(), or isalnum() on
** hi-bit-set characters.  This is the same solution used in the
** tokenizer.
*/
/* TODO(shess) The snippet-generation code should be using the
** tokenizer-generated tokens rather than doing its own local
** tokenization.
*/
/* TODO(shess) Is __isascii() a portable version of (c&0x80)==0? */
static int safe_isspace(char c){
  return (c&0x80)==0 ? isspace(c) : 0;
}
static int safe_tolower(char c){
  return (c&0x80)==0 ? tolower(c) : c;
}
static int safe_isalnum(char c){
  return (c&0x80)==0 ? isalnum(c) : 0;
}

typedef enum DocListType {
  DL_DOCIDS,              /* docids only */
  DL_POSITIONS,           /* docids + positions */
  DL_POSITIONS_OFFSETS    /* docids + positions + offsets */
} DocListType;

/*
** By default, only positions and not offsets are stored in the doclists.
** To change this so that offsets are stored too, compile with
**
**          -DDL_DEFAULT=DL_POSITIONS_OFFSETS
**
*/
#ifndef DL_DEFAULT
# define DL_DEFAULT DL_POSITIONS
#endif

typedef struct DocList {
  char *pData;
  int nData;
  DocListType iType;
  int iLastColumn;    /* the last column written */
  int iLastPos;       /* the last position written */
  int iLastOffset;    /* the last start offset written */
} DocList;

enum {
  POS_END = 0,        /* end of this position list */
  POS_COLUMN,         /* followed by new column number */
  POS_BASE
};

/* Initialize a new DocList to hold the given data. */
static void docListInit(DocList *d, DocListType iType,
                        const char *pData, int nData){
  d->nData = nData;
  if( nData>0 ){
    d->pData = malloc(nData);
    memcpy(d->pData, pData, nData);
  } else {
    d->pData = NULL;
  }
  d->iType = iType;
  d->iLastColumn = 0;
  d->iLastPos = d->iLastOffset = 0;
}

/* Create a new dynamically-allocated DocList. */
static DocList *docListNew(DocListType iType){
  DocList *d = (DocList *) malloc(sizeof(DocList));
  docListInit(d, iType, 0, 0);
  return d;
}

static void docListDestroy(DocList *d){
  free(d->pData);
#ifndef NDEBUG
  memset(d, 0x55, sizeof(*d));
#endif
}

static void docListDelete(DocList *d){
  docListDestroy(d);
  free(d);
}

static char *docListEnd(DocList *d){
  return d->pData + d->nData;
}

/* Append a varint to a DocList's data. */
static void appendVarint(DocList *d, sqlite_int64 i){
  char c[VARINT_MAX];
  int n = putVarint(c, i);
  d->pData = realloc(d->pData, d->nData + n);
  memcpy(d->pData + d->nData, c, n);
  d->nData += n;
}

static void docListAddDocid(DocList *d, sqlite_int64 iDocid){
  appendVarint(d, iDocid);
  if( d->iType>=DL_POSITIONS ){
    appendVarint(d, POS_END);  /* initially empty position list */
    d->iLastColumn = 0;
    d->iLastPos = d->iLastOffset = 0;
  }
}

/* helper function for docListAddPos and docListAddPosOffset */
static void addPos(DocList *d, int iColumn, int iPos){
  assert( d->nData>0 );
  --d->nData;  /* remove previous terminator */
  if( iColumn!=d->iLastColumn ){
    assert( iColumn>d->iLastColumn );
    appendVarint(d, POS_COLUMN);
    appendVarint(d, iColumn);
    d->iLastColumn = iColumn;
    d->iLastPos = d->iLastOffset = 0;
  }
  assert( iPos>=d->iLastPos );
  appendVarint(d, iPos-d->iLastPos+POS_BASE);
  d->iLastPos = iPos;
}

/* Add a position to the last position list in a doclist. */
static void docListAddPos(DocList *d, int iColumn, int iPos){
  assert( d->iType==DL_POSITIONS );
  addPos(d, iColumn, iPos);
  appendVarint(d, POS_END);  /* add new terminator */
}

/*
** Add a position and starting and ending offsets to a doclist.
**
** If the doclist is setup to handle only positions, then insert
** the position only and ignore the offsets.
*/
static void docListAddPosOffset(
  DocList *d,             /* Doclist under construction */
  int iColumn,            /* Column the inserted term is part of */
  int iPos,               /* Position of the inserted term */
  int iStartOffset,       /* Starting offset of inserted term */
  int iEndOffset          /* Ending offset of inserted term */
){
  assert( d->iType>=DL_POSITIONS );
  addPos(d, iColumn, iPos);
  if( d->iType==DL_POSITIONS_OFFSETS ){
    assert( iStartOffset>=d->iLastOffset );
    appendVarint(d, iStartOffset-d->iLastOffset);
    d->iLastOffset = iStartOffset;
    assert( iEndOffset>=iStartOffset );
    appendVarint(d, iEndOffset-iStartOffset);
  }
  appendVarint(d, POS_END);  /* add new terminator */
}

/*
** A DocListReader object is a cursor into a doclist.  Initialize
** the cursor to the beginning of the doclist by calling readerInit().
** Then use routines
**
**      peekDocid()
**      readDocid()
**      readPosition()
**      skipPositionList()
**      and so forth...
**
** to read information out of the doclist.  When we reach the end
** of the doclist, atEnd() returns TRUE.
*/
typedef struct DocListReader {
  DocList *pDoclist;  /* The document list we are stepping through */
  char *p;            /* Pointer to next unread byte in the doclist */
  int iLastColumn;
  int iLastPos;  /* the last position read, or -1 when not in a position list */
} DocListReader;

/*
** Initialize the DocListReader r to point to the beginning of pDoclist.
*/
static void readerInit(DocListReader *r, DocList *pDoclist){
  r->pDoclist = pDoclist;
  if( pDoclist!=NULL ){
    r->p = pDoclist->pData;
  }
  r->iLastColumn = -1;
  r->iLastPos = -1;
}

/*
** Return TRUE if we have reached then end of pReader and there is
** nothing else left to read.
*/
static int atEnd(DocListReader *pReader){
  return pReader->pDoclist==0 || (pReader->p >= docListEnd(pReader->pDoclist));
}

/* Peek at the next docid without advancing the read pointer. 
*/
static sqlite_int64 peekDocid(DocListReader *pReader){
  sqlite_int64 ret;
  assert( !atEnd(pReader) );
  assert( pReader->iLastPos==-1 );
  getVarint(pReader->p, &ret);
  return ret;
}

/* Read the next docid.   See also nextDocid().
*/
static sqlite_int64 readDocid(DocListReader *pReader){
  sqlite_int64 ret;
  assert( !atEnd(pReader) );
  assert( pReader->iLastPos==-1 );
  pReader->p += getVarint(pReader->p, &ret);
  if( pReader->pDoclist->iType>=DL_POSITIONS ){
    pReader->iLastColumn = 0;
    pReader->iLastPos = 0;
  }
  return ret;
}

/* Read the next position and column index from a position list.
 * Returns the position, or -1 at the end of the list. */
static int readPosition(DocListReader *pReader, int *iColumn){
  int i;
  int iType = pReader->pDoclist->iType;

  if( pReader->iLastPos==-1 ){
    return -1;
  }
  assert( !atEnd(pReader) );

  if( iType<DL_POSITIONS ){
    return -1;
  }
  pReader->p += getVarint32(pReader->p, &i);
  if( i==POS_END ){
    pReader->iLastColumn = pReader->iLastPos = -1;
    *iColumn = -1;
    return -1;
  }
  if( i==POS_COLUMN ){
    pReader->p += getVarint32(pReader->p, &pReader->iLastColumn);
    pReader->iLastPos = 0;
    pReader->p += getVarint32(pReader->p, &i);
    assert( i>=POS_BASE );
  }
  pReader->iLastPos += ((int) i)-POS_BASE;
  if( iType>=DL_POSITIONS_OFFSETS ){
    /* Skip over offsets, ignoring them for now. */
    int iStart, iEnd;
    pReader->p += getVarint32(pReader->p, &iStart);
    pReader->p += getVarint32(pReader->p, &iEnd);
  }
  *iColumn = pReader->iLastColumn;
  return pReader->iLastPos;
}

/* Skip past the end of a position list. */
static void skipPositionList(DocListReader *pReader){
  DocList *p = pReader->pDoclist;
  if( p && p->iType>=DL_POSITIONS ){
    int iColumn;
    while( readPosition(pReader, &iColumn)!=-1 ){}
  }
}

/* Skip over a docid, including its position list if the doclist has
 * positions. */
static void skipDocument(DocListReader *pReader){
  readDocid(pReader);
  skipPositionList(pReader);
}

/* Skip past all docids which are less than [iDocid].  Returns 1 if a docid
 * matching [iDocid] was found.  */
static int skipToDocid(DocListReader *pReader, sqlite_int64 iDocid){
  sqlite_int64 d = 0;
  while( !atEnd(pReader) && (d=peekDocid(pReader))<iDocid ){
    skipDocument(pReader);
  }
  return !atEnd(pReader) && d==iDocid;
}

/* Return the first document in a document list.
*/
static sqlite_int64 firstDocid(DocList *d){
  DocListReader r;
  readerInit(&r, d);
  return readDocid(&r);
}

#ifdef SQLITE_DEBUG
/*
** This routine is used for debugging purpose only.
**
** Write the content of a doclist to standard output.
*/
static void printDoclist(DocList *p){
  DocListReader r;
  const char *zSep = "";

  readerInit(&r, p);
  while( !atEnd(&r) ){
    sqlite_int64 docid = readDocid(&r);
    if( docid==0 ){
      skipPositionList(&r);
      continue;
    }
    printf("%s%lld", zSep, docid);
    zSep =  ",";
    if( p->iType>=DL_POSITIONS ){
      int iPos, iCol;
      const char *zDiv = "";
      printf("(");
      while( (iPos = readPosition(&r, &iCol))>=0 ){
        printf("%s%d:%d", zDiv, iCol, iPos);
        zDiv = ":";
      }
      printf(")");
    }
  }
  printf("\n");
  fflush(stdout);
}
#endif /* SQLITE_DEBUG */

/* Trim the given doclist to contain only positions in column
 * [iRestrictColumn]. */
static void docListRestrictColumn(DocList *in, int iRestrictColumn){
  DocListReader r;
  DocList out;

  assert( in->iType>=DL_POSITIONS );
  readerInit(&r, in);
  docListInit(&out, DL_POSITIONS, NULL, 0);

  while( !atEnd(&r) ){
    sqlite_int64 iDocid = readDocid(&r);
    int iPos, iColumn;

    docListAddDocid(&out, iDocid);
    while( (iPos = readPosition(&r, &iColumn)) != -1 ){
      if( iColumn==iRestrictColumn ){
        docListAddPos(&out, iColumn, iPos);
      }
    }
  }

  docListDestroy(in);
  *in = out;
}

/* Trim the given doclist by discarding any docids without any remaining
 * positions. */
static void docListDiscardEmpty(DocList *in) {
  DocListReader r;
  DocList out;

  /* TODO: It would be nice to implement this operation in place; that
   * could save a significant amount of memory in queries with long doclists. */
  assert( in->iType>=DL_POSITIONS );
  readerInit(&r, in);
  docListInit(&out, DL_POSITIONS, NULL, 0);

  while( !atEnd(&r) ){
    sqlite_int64 iDocid = readDocid(&r);
    int match = 0;
    int iPos, iColumn;
    while( (iPos = readPosition(&r, &iColumn)) != -1 ){
      if( !match ){
        docListAddDocid(&out, iDocid);
        match = 1;
      }
      docListAddPos(&out, iColumn, iPos);
    }
  }

  docListDestroy(in);
  *in = out;
}

/* Helper function for docListUpdate() and docListAccumulate().
** Splices a doclist element into the doclist represented by r,
** leaving r pointing after the newly spliced element.
*/
static void docListSpliceElement(DocListReader *r, sqlite_int64 iDocid,
                                 const char *pSource, int nSource){
  DocList *d = r->pDoclist;
  char *pTarget;
  int nTarget, found;

  found = skipToDocid(r, iDocid);

  /* Describe slice in d to place pSource/nSource. */
  pTarget = r->p;
  if( found ){
    skipDocument(r);
    nTarget = r->p-pTarget;
  }else{
    nTarget = 0;
  }

  /* The sense of the following is that there are three possibilities.
  ** If nTarget==nSource, we should not move any memory nor realloc.
  ** If nTarget>nSource, trim target and realloc.
  ** If nTarget<nSource, realloc then expand target.
  */
  if( nTarget>nSource ){
    memmove(pTarget+nSource, pTarget+nTarget, docListEnd(d)-(pTarget+nTarget));
  }
  if( nTarget!=nSource ){
    int iDoclist = pTarget-d->pData;
    d->pData = realloc(d->pData, d->nData+nSource-nTarget);
    pTarget = d->pData+iDoclist;
  }
  if( nTarget<nSource ){
    memmove(pTarget+nSource, pTarget+nTarget, docListEnd(d)-(pTarget+nTarget));
  }

  memcpy(pTarget, pSource, nSource);
  d->nData += nSource-nTarget;
  r->p = pTarget+nSource;
}

/* Insert/update pUpdate into the doclist. */
static void docListUpdate(DocList *d, DocList *pUpdate){
  DocListReader reader;

  assert( d!=NULL && pUpdate!=NULL );
  assert( d->iType==pUpdate->iType);

  readerInit(&reader, d);
  docListSpliceElement(&reader, firstDocid(pUpdate),
                       pUpdate->pData, pUpdate->nData);
}

/* Propagate elements from pUpdate to pAcc, overwriting elements with
** matching docids.
*/
static void docListAccumulate(DocList *pAcc, DocList *pUpdate){
  DocListReader accReader, updateReader;

  /* Handle edge cases where one doclist is empty. */
  assert( pAcc!=NULL );
  if( pUpdate==NULL || pUpdate->nData==0 ) return;
  if( pAcc->nData==0 ){
    pAcc->pData = malloc(pUpdate->nData);
    memcpy(pAcc->pData, pUpdate->pData, pUpdate->nData);
    pAcc->nData = pUpdate->nData;
    return;
  }

  readerInit(&accReader, pAcc);
  readerInit(&updateReader, pUpdate);

  while( !atEnd(&updateReader) ){
    char *pSource = updateReader.p;
    sqlite_int64 iDocid = readDocid(&updateReader);
    skipPositionList(&updateReader);
    docListSpliceElement(&accReader, iDocid, pSource, updateReader.p-pSource);
  }
}

/*
** Read the next docid off of pIn.  Return 0 if we reach the end.
*
* TODO: This assumes that docids are never 0, but they may actually be 0 since
* users can choose docids when inserting into a full-text table.  Fix this.
*/
static sqlite_int64 nextDocid(DocListReader *pIn){
  skipPositionList(pIn);
  return atEnd(pIn) ? 0 : readDocid(pIn);
}

/*
** pLeft and pRight are two DocListReaders that are pointing to
** positions lists of the same document: iDocid. 
**
** If there are no instances in pLeft or pRight where the position
** of pLeft is one less than the position of pRight, then this
** routine adds nothing to pOut.
**
** If there are one or more instances where positions from pLeft
** are exactly one less than positions from pRight, then add a new
** document record to pOut.  If pOut wants to hold positions, then
** include the positions from pRight that are one more than a
** position in pLeft.  In other words:  pRight.iPos==pLeft.iPos+1.
**
** pLeft and pRight are left pointing at the next document record.
*/
static void mergePosList(
  DocListReader *pLeft,    /* Left position list */
  DocListReader *pRight,   /* Right position list */
  sqlite_int64 iDocid,     /* The docid from pLeft and pRight */
  DocList *pOut            /* Write the merged document record here */
){
  int iLeftCol, iLeftPos = readPosition(pLeft, &iLeftCol);
  int iRightCol, iRightPos = readPosition(pRight, &iRightCol);
  int match = 0;

  /* Loop until we've reached the end of both position lists. */
  while( iLeftPos!=-1 && iRightPos!=-1 ){
    if( iLeftCol==iRightCol && iLeftPos+1==iRightPos ){
      if( !match ){
        docListAddDocid(pOut, iDocid);
        match = 1;
      }
      if( pOut->iType>=DL_POSITIONS ){
        docListAddPos(pOut, iRightCol, iRightPos);
      }
      iLeftPos = readPosition(pLeft, &iLeftCol);
      iRightPos = readPosition(pRight, &iRightCol);
    }else if( iRightCol<iLeftCol ||
              (iRightCol==iLeftCol && iRightPos<iLeftPos+1) ){
      iRightPos = readPosition(pRight, &iRightCol);
    }else{
      iLeftPos = readPosition(pLeft, &iLeftCol);
    }
  }
  if( iLeftPos>=0 ) skipPositionList(pLeft);
  if( iRightPos>=0 ) skipPositionList(pRight);
}

/* We have two doclists:  pLeft and pRight.
** Write the phrase intersection of these two doclists into pOut.
**
** A phrase intersection means that two documents only match
** if pLeft.iPos+1==pRight.iPos.
**
** The output pOut may or may not contain positions.  If pOut
** does contain positions, they are the positions of pRight.
*/
static void docListPhraseMerge(
  DocList *pLeft,    /* Doclist resulting from the words on the left */
  DocList *pRight,   /* Doclist for the next word to the right */
  DocList *pOut      /* Write the combined doclist here */
){
  DocListReader left, right;
  sqlite_int64 docidLeft, docidRight;

  readerInit(&left, pLeft);
  readerInit(&right, pRight);
  docidLeft = nextDocid(&left);
  docidRight = nextDocid(&right);

  while( docidLeft>0 && docidRight>0 ){
    if( docidLeft<docidRight ){
      docidLeft = nextDocid(&left);
    }else if( docidRight<docidLeft ){
      docidRight = nextDocid(&right);
    }else{
      mergePosList(&left, &right, docidLeft, pOut);
      docidLeft = nextDocid(&left);
      docidRight = nextDocid(&right);
    }
  }
}

/* We have two doclists:  pLeft and pRight.
** Write the intersection of these two doclists into pOut.
** Only docids are matched.  Position information is ignored.
**
** The output pOut never holds positions.
*/
static void docListAndMerge(
  DocList *pLeft,    /* Doclist resulting from the words on the left */
  DocList *pRight,   /* Doclist for the next word to the right */
  DocList *pOut      /* Write the combined doclist here */
){
  DocListReader left, right;
  sqlite_int64 docidLeft, docidRight;

  assert( pOut->iType<DL_POSITIONS );

  readerInit(&left, pLeft);
  readerInit(&right, pRight);
  docidLeft = nextDocid(&left);
  docidRight = nextDocid(&right);

  while( docidLeft>0 && docidRight>0 ){
    if( docidLeft<docidRight ){
      docidLeft = nextDocid(&left);
    }else if( docidRight<docidLeft ){
      docidRight = nextDocid(&right);
    }else{
      docListAddDocid(pOut, docidLeft);
      docidLeft = nextDocid(&left);
      docidRight = nextDocid(&right);
    }
  }
}

/* We have two doclists:  pLeft and pRight.
** Write the union of these two doclists into pOut.
** Only docids are matched.  Position information is ignored.
**
** The output pOut never holds positions.
*/
static void docListOrMerge(
  DocList *pLeft,    /* Doclist resulting from the words on the left */
  DocList *pRight,   /* Doclist for the next word to the right */
  DocList *pOut      /* Write the combined doclist here */
){
  DocListReader left, right;
  sqlite_int64 docidLeft, docidRight, priorLeft;

  readerInit(&left, pLeft);
  readerInit(&right, pRight);
  docidLeft = nextDocid(&left);
  docidRight = nextDocid(&right);

  while( docidLeft>0 && docidRight>0 ){
    if( docidLeft<=docidRight ){
      docListAddDocid(pOut, docidLeft);
    }else{
      docListAddDocid(pOut, docidRight);
    }
    priorLeft = docidLeft;
    if( docidLeft<=docidRight ){
      docidLeft = nextDocid(&left);
    }
    if( docidRight>0 && docidRight<=priorLeft ){
      docidRight = nextDocid(&right);
    }
  }
  while( docidLeft>0 ){
    docListAddDocid(pOut, docidLeft);
    docidLeft = nextDocid(&left);
  }
  while( docidRight>0 ){
    docListAddDocid(pOut, docidRight);
    docidRight = nextDocid(&right);
  }
}

/* We have two doclists:  pLeft and pRight.
** Write into pOut all documents that occur in pLeft but not
** in pRight.
**
** Only docids are matched.  Position information is ignored.
**
** The output pOut never holds positions.
*/
static void docListExceptMerge(
  DocList *pLeft,    /* Doclist resulting from the words on the left */
  DocList *pRight,   /* Doclist for the next word to the right */
  DocList *pOut      /* Write the combined doclist here */
){
  DocListReader left, right;
  sqlite_int64 docidLeft, docidRight, priorLeft;

  readerInit(&left, pLeft);
  readerInit(&right, pRight);
  docidLeft = nextDocid(&left);
  docidRight = nextDocid(&right);

  while( docidLeft>0 && docidRight>0 ){
    priorLeft = docidLeft;
    if( docidLeft<docidRight ){
      docListAddDocid(pOut, docidLeft);
    }
    if( docidLeft<=docidRight ){
      docidLeft = nextDocid(&left);
    }
    if( docidRight>0 && docidRight<=priorLeft ){
      docidRight = nextDocid(&right);
    }
  }
  while( docidLeft>0 ){
    docListAddDocid(pOut, docidLeft);
    docidLeft = nextDocid(&left);
  }
}

static char *string_dup_n(const char *s, int n){
  char *str = malloc(n + 1);
  memcpy(str, s, n);
  str[n] = '\0';
  return str;
}

/* Duplicate a string; the caller must free() the returned string.
 * (We don't use strdup() since it is not part of the standard C library and
 * may not be available everywhere.) */
static char *string_dup(const char *s){
  return string_dup_n(s, strlen(s));
}

/* Format a string, replacing each occurrence of the % character with
 * zDb.zName.  This may be more convenient than sqlite_mprintf()
 * when one string is used repeatedly in a format string.
 * The caller must free() the returned string. */
static char *string_format(const char *zFormat,
                           const char *zDb, const char *zName){
  const char *p;
  size_t len = 0;
  size_t nDb = strlen(zDb);
  size_t nName = strlen(zName);
  size_t nFullTableName = nDb+1+nName;
  char *result;
  char *r;

  /* first compute length needed */
  for(p = zFormat ; *p ; ++p){
    len += (*p=='%' ? nFullTableName : 1);
  }
  len += 1;  /* for null terminator */

  r = result = malloc(len);
  for(p = zFormat; *p; ++p){
    if( *p=='%' ){
      memcpy(r, zDb, nDb);
      r += nDb;
      *r++ = '.';
      memcpy(r, zName, nName);
      r += nName;
    } else {
      *r++ = *p;
    }
  }
  *r++ = '\0';
  assert( r == result + len );
  return result;
}

static int sql_exec(sqlite3 *db, const char *zDb, const char *zName,
                    const char *zFormat){
  char *zCommand = string_format(zFormat, zDb, zName);
  int rc;
  TRACE(("FTS1 sql: %s\n", zCommand));
  rc = sqlite3_exec(db, zCommand, NULL, 0, NULL);
  free(zCommand);
  return rc;
}

static int sql_prepare(sqlite3 *db, const char *zDb, const char *zName,
                       sqlite3_stmt **ppStmt, const char *zFormat){
  char *zCommand = string_format(zFormat, zDb, zName);
  int rc;
  TRACE(("FTS1 prepare: %s\n", zCommand));
  rc = sqlite3_prepare(db, zCommand, -1, ppStmt, NULL);
  free(zCommand);
  return rc;
}

/* end utility functions */

/* Forward reference */
typedef struct fulltext_vtab fulltext_vtab;

/* A single term in a query is represented by an instances of
** the following structure.
*/
typedef struct QueryTerm {
  short int nPhrase; /* How many following terms are part of the same phrase */
  short int iPhrase; /* This is the i-th term of a phrase. */
  short int iColumn; /* Column of the index that must match this term */
  signed char isOr;  /* this term is preceded by "OR" */
  signed char isNot; /* this term is preceded by "-" */
  char *pTerm;       /* text of the term.  '\000' terminated.  malloced */
  int nTerm;         /* Number of bytes in pTerm[] */
} QueryTerm;


/* A query string is parsed into a Query structure.
 *
 * We could, in theory, allow query strings to be complicated
 * nested expressions with precedence determined by parentheses.
 * But none of the major search engines do this.  (Perhaps the
 * feeling is that an parenthesized expression is two complex of
 * an idea for the average user to grasp.)  Taking our lead from
 * the major search engines, we will allow queries to be a list
 * of terms (with an implied AND operator) or phrases in double-quotes,
 * with a single optional "-" before each non-phrase term to designate
 * negation and an optional OR connector.
 *
 * OR binds more tightly than the implied AND, which is what the
 * major search engines seem to do.  So, for example:
 * 
 *    [one two OR three]     ==>    one AND (two OR three)
 *    [one OR two three]     ==>    (one OR two) AND three
 *
 * A "-" before a term matches all entries that lack that term.
 * The "-" must occur immediately before the term with in intervening
 * space.  This is how the search engines do it.
 *
 * A NOT term cannot be the right-hand operand of an OR.  If this
 * occurs in the query string, the NOT is ignored:
 *
 *    [one OR -two]          ==>    one OR two
 *
 */
typedef struct Query {
  fulltext_vtab *pFts;  /* The full text index */
  int nTerms;           /* Number of terms in the query */
  QueryTerm *pTerms;    /* Array of terms.  Space obtained from malloc() */
  int nextIsOr;         /* Set the isOr flag on the next inserted term */
  int nextColumn;       /* Next word parsed must be in this column */
  int dfltColumn;       /* The default column */
} Query;


/*
** An instance of the following structure keeps track of generated
** matching-word offset information and snippets.
*/
typedef struct Snippet {
  int nMatch;     /* Total number of matches */
  int nAlloc;     /* Space allocated for aMatch[] */
  struct snippetMatch { /* One entry for each matching term */
    char snStatus;       /* Status flag for use while constructing snippets */
    short int iCol;      /* The column that contains the match */
    short int iTerm;     /* The index in Query.pTerms[] of the matching term */
    short int nByte;     /* Number of bytes in the term */
    int iStart;          /* The offset to the first character of the term */
  } *aMatch;      /* Points to space obtained from malloc */
  char *zOffset;  /* Text rendering of aMatch[] */
  int nOffset;    /* strlen(zOffset) */
  char *zSnippet; /* Snippet text */
  int nSnippet;   /* strlen(zSnippet) */
} Snippet;


typedef enum QueryType {
  QUERY_GENERIC,   /* table scan */
  QUERY_ROWID,     /* lookup by rowid */
  QUERY_FULLTEXT   /* QUERY_FULLTEXT + [i] is a full-text search for column i*/
} QueryType;

/* TODO(shess) CHUNK_MAX controls how much data we allow in segment 0
** before we start aggregating into larger segments.  Lower CHUNK_MAX
** means that for a given input we have more individual segments per
** term, which means more rows in the table and a bigger index (due to
** both more rows and bigger rowids).  But it also reduces the average
** cost of adding new elements to the segment 0 doclist, and it seems
** to reduce the number of pages read and written during inserts.  256
** was chosen by measuring insertion times for a certain input (first
** 10k documents of Enron corpus), though including query performance
** in the decision may argue for a larger value.
*/
#define CHUNK_MAX 256

typedef enum fulltext_statement {
  CONTENT_INSERT_STMT,
  CONTENT_SELECT_STMT,
  CONTENT_UPDATE_STMT,
  CONTENT_DELETE_STMT,

  TERM_SELECT_STMT,
  TERM_SELECT_ALL_STMT,
  TERM_INSERT_STMT,
  TERM_UPDATE_STMT,
  TERM_DELETE_STMT,

  MAX_STMT                     /* Always at end! */
} fulltext_statement;

/* These must exactly match the enum above. */
/* TODO(adam): Is there some risk that a statement (in particular,
** pTermSelectStmt) will be used in two cursors at once, e.g.  if a
** query joins a virtual table to itself?  If so perhaps we should
** move some of these to the cursor object.
*/
static const char *const fulltext_zStatement[MAX_STMT] = {
  /* CONTENT_INSERT */ NULL,  /* generated in contentInsertStatement() */
  /* CONTENT_SELECT */ "select * from %_content where rowid = ?",
  /* CONTENT_UPDATE */ NULL,  /* generated in contentUpdateStatement() */
  /* CONTENT_DELETE */ "delete from %_content where rowid = ?",

  /* TERM_SELECT */
  "select rowid, doclist from %_term where term = ? and segment = ?",
  /* TERM_SELECT_ALL */
  "select doclist from %_term where term = ? order by segment",
  /* TERM_INSERT */
  "insert into %_term (rowid, term, segment, doclist) values (?, ?, ?, ?)",
  /* TERM_UPDATE */ "update %_term set doclist = ? where rowid = ?",
  /* TERM_DELETE */ "delete from %_term where rowid = ?",
};

/*
** A connection to a fulltext index is an instance of the following
** structure.  The xCreate and xConnect methods create an instance
** of this structure and xDestroy and xDisconnect free that instance.
** All other methods receive a pointer to the structure as one of their
** arguments.
*/
struct fulltext_vtab {
  sqlite3_vtab base;               /* Base class used by SQLite core */
  sqlite3 *db;                     /* The database connection */
  const char *zDb;                 /* logical database name */
  const char *zName;               /* virtual table name */
  int nColumn;                     /* number of columns in virtual table */
  char **azColumn;                 /* column names.  malloced */
  char **azContentColumn;          /* column names in content table; malloced */
  sqlite3_tokenizer *pTokenizer;   /* tokenizer for inserts and queries */

  /* Precompiled statements which we keep as long as the table is
  ** open.
  */
  sqlite3_stmt *pFulltextStatements[MAX_STMT];
};

/* 当core将要进行查询时，它会使用xOpen创建一个游标,这个结构体就是一个游标的例子
** 使用xClose销毁游标
** When the core wants to do a query, it create a cursor using a
** call to xOpen.  This structure is an instance of a cursor.  It
** is destroyed by xClose.
*/
typedef struct fulltext_cursor {
  sqlite3_vtab_cursor base;        /* Base class used by SQLite core */
  QueryType iCursorType;           /* Copy of sqlite3_index_info.idxNum */
  sqlite3_stmt *pStmt;             /* Prepared statement in use by the cursor 游标中使用的语句 */
  int eof;                         /* True if at End Of Results */
  Query q;                         /* Parsed query string 解析查询语句 */
  Snippet snippet;                 /* Cached snippet for the current row 存储当前行的段*/
  int iColumn;                     /* Column being searched 搜索列*/
  DocListReader result;  /* used when iCursorType == QUERY_FULLTEXT */ 
} fulltext_cursor;

static struct fulltext_vtab *cursor_vtab(fulltext_cursor *c){
  return (fulltext_vtab *) c->base.pVtab;
}

static const sqlite3_module fulltextModule;   /* forward declaration */

/* Append a list of strings separated by commas to a StringBuffer. 
** 在StringBuffer中附加一个字符串列表,并用逗号隔开
*/
static void appendList(StringBuffer *sb, int nString, char **azString){
  int i;
  for(i=0; i<nString; ++i){
    if( i>0 ) append(sb, ", "); //添加逗号
    append(sb, azString[i]);    //添加字符串azString[i]
  }
}

/* Return a dynamically generated statement of the form
 * 构造一个插入语句，返回一个如下形式的动态产生的语句
 *   insert into %_content (rowid, ...) values (?, ...)
 */
static const char *contentInsertStatement(fulltext_vtab *v){
  StringBuffer sb;      //创建一个StringBuffer变量
  int i;

  initStringBuffer(&sb);//初始化
  //在sb中添加sqlite插入字符串
  append(&sb, "insert into %_content (rowid, ");
  appendList(&sb, v->nColumn, v->azContentColumn);
  append(&sb, ") values (?");
  for(i=0; i<v->nColumn; ++i)
    append(&sb, ", ?");
  append(&sb, ")");
  return sb.s;
}

/* Return a dynamically generated statement of the form
 * 构造一个update语句，返回一个如下形式的动态产生的语句
 *   update %_content set [col_0] = ?, [col_1] = ?, ...
 *                    where rowid = ?
 */
static const char *contentUpdateStatement(fulltext_vtab *v){
  StringBuffer sb;
  int i;

  initStringBuffer(&sb);
  append(&sb, "update %_content set ");
  for(i=0; i<v->nColumn; ++i) {
    if( i>0 ){
      append(&sb, ", ");
    }
    append(&sb, v->azContentColumn[i]);
    append(&sb, " = ?");
  }
  append(&sb, " where rowid = ?");
  return sb.s;
}

/* Puts a freshly-prepared statement determined by iStmt in *ppStmt.
** If the indicated statement has never been prepared, it is prepared
** and cached, otherwise the cached version is reset.
** 构造一个新的操作语句，如果语句被创建，则将其储存，否则存储默认值。
*/
static int sql_get_statement(fulltext_vtab *v, fulltext_statement iStmt,
                             sqlite3_stmt **ppStmt){
  assert( iStmt<MAX_STMT );
  if( v->pFulltextStatements[iStmt]==NULL ){
    const char *zStmt;
    int rc;
    switch( iStmt ){
      case CONTENT_INSERT_STMT:
        zStmt = contentInsertStatement(v); break;
      case CONTENT_UPDATE_STMT:
        zStmt = contentUpdateStatement(v); break;
      default:
        zStmt = fulltext_zStatement[iStmt];
    }
    rc = sql_prepare(v->db, v->zDb, v->zName, &v->pFulltextStatements[iStmt],
                         zStmt);
    if( zStmt != fulltext_zStatement[iStmt]) free((void *) zStmt);
    if( rc!=SQLITE_OK ) return rc;
  } else {
    int rc = sqlite3_reset(v->pFulltextStatements[iStmt]);
    if( rc!=SQLITE_OK ) return rc;
  }

  *ppStmt = v->pFulltextStatements[iStmt];
  return SQLITE_OK;
}

/* Step the indicated statement, handling errors SQLITE_BUSY (by
** retrying) and SQLITE_SCHEMA (by re-preparing and transferring
** bindings to the new statement).
** 执行语句，错误代码处理
** SQLITE_BUSY  重试
** SQLITE_SCHEMA  重新配置并且绑定新的语句
** TODO(adam): We should extend this function so that it can work with
** statements declared locally, not only globally cached statements.
*/
static int sql_step_statement(fulltext_vtab *v, fulltext_statement iStmt,
                              sqlite3_stmt **ppStmt){
  int rc;
  sqlite3_stmt *s = *ppStmt;
  assert( iStmt<MAX_STMT );
  assert( s==v->pFulltextStatements[iStmt] );

  while( (rc=sqlite3_step(s))!=SQLITE_DONE && rc!=SQLITE_ROW ){
    if( rc==SQLITE_BUSY ) continue;
    if( rc!=SQLITE_ERROR ) return rc;

    /* If an SQLITE_SCHEMA error has occurred, then finalizing this
     * statement is going to delete the fulltext_vtab structure. If
     * the statement just executed is in the pFulltextStatements[]
     * array, it will be finalized twice. So remove it before
     * calling sqlite3_finalize().
     * 如果发生SQLITE_SCHEMA错误，最终这个语句将会删除fulltext_vtab结构。
     * 如果语句恰好在pFulltextStatements[]数组中执行，它将会执行两次。
     * 因此删除它之前调用sqlite3_finalize()。
     */
    v->pFulltextStatements[iStmt] = NULL;
    rc = sqlite3_finalize(s);
    break;
  }
  return rc;

 err:
  sqlite3_finalize(s);
  return rc;
}

/* Like sql_step_statement(), but convert SQLITE_DONE to SQLITE_OK.
** Useful for statements like UPDATE, where we expect no results.
** 和sql_step_statement()类似，但是将SQLITE_DONE转换为SQLITE_OK
** 可用于不会将结果输出的语句，比如update等。
*/
static int sql_single_step_statement(fulltext_vtab *v,
                                     fulltext_statement iStmt,
                                     sqlite3_stmt **ppStmt){
  int rc = sql_step_statement(v, iStmt, ppStmt);
  return (rc==SQLITE_DONE) ? SQLITE_OK : rc;
}

/* insert into %_content (rowid, ...) values ([rowid], [pValues]) */
/* 这个函数用于插入内容 */
static int content_insert(fulltext_vtab *v, sqlite3_value *rowid,
                          sqlite3_value **pValues){
  sqlite3_stmt *s;
  int i;
  int rc = sql_get_statement(v, CONTENT_INSERT_STMT, &s);//得到插入语句             
  if( rc!=SQLITE_OK ) return rc;
  //绑定对应的值
  rc = sqlite3_bind_value(s, 1, rowid);
  if( rc!=SQLITE_OK ) return rc;

  for(i=0; i<v->nColumn; ++i){
    rc = sqlite3_bind_value(s, 2+i, pValues[i]);
    if( rc!=SQLITE_OK ) return rc;
  }

  return sql_single_step_statement(v, CONTENT_INSERT_STMT, &s);	//执行语句
}

/* update %_content set col0 = pValues[0], col1 = pValues[1], ...
 *                  where rowid = [iRowid] */
/*这个函数用于更新内容*/
static int content_update(fulltext_vtab *v, sqlite3_value **pValues,
                          sqlite_int64 iRowid){
  sqlite3_stmt *s;
  int i;
  int rc = sql_get_statement(v, CONTENT_UPDATE_STMT, &s);//得到更新语句
  if( rc!=SQLITE_OK ) return rc;

  for(i=0; i<v->nColumn; ++i){		//依次绑定每一列的值
    rc = sqlite3_bind_value(s, 1+i, pValues[i]);
    if( rc!=SQLITE_OK ) return rc;
  }

  rc = sqlite3_bind_int64(s, 1+v->nColumn, iRowid);	//绑定伪列
  if( rc!=SQLITE_OK ) return rc;

  return sql_single_step_statement(v, CONTENT_UPDATE_STMT, &s);//执行语句
}
/*释放pString占用的资源*/
static void freeStringArray(int nString, const char **pString){
  int i;

  for (i=0 ; i < nString ; ++i) {
    if( pString[i]!=NULL ) free((void *) pString[i]);
  }
  free((void *) pString);
}

/* select * from %_content where rowid = [iRow]
 * The caller must delete the returned array and all strings in it.
 * null fields will be NULL in the returned array.
 * 执行选择语句的函数，在返回数组中包含所有选择后得到的字符串。
 * 最后，必须删除函数运行期间临时用来存储的数组。
 *
 * TODO: Perhaps we should return pointer/length strings here for consistency
 * with other code which uses pointer/length. */
static int content_select(fulltext_vtab *v, sqlite_int64 iRow,
                          const char ***pValues){
  sqlite3_stmt *s;
  const char **values;//用来临时存储返回值
  int i;
  int rc;

  *pValues = NULL;

  rc = sql_get_statement(v, CONTENT_SELECT_STMT, &s); //得到选择语句
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_int64(s, 1, iRow);
  if( rc!=SQLITE_OK ) return rc;

  rc = sql_step_statement(v, CONTENT_SELECT_STMT, &s);
  if( rc!=SQLITE_ROW ) return rc;
  /*返回值用char型数组存储*/
  values = (const char **) malloc(v->nColumn * sizeof(const char *));
  for(i=0; i<v->nColumn; ++i){
    if( sqlite3_column_type(s, i)==SQLITE_NULL ){
      values[i] = NULL;
    }else{
      values[i] = string_dup((char*)sqlite3_column_text(s, i));
    }
  }

  /* We expect only one row.  We must execute another sqlite3_step()
   * to complete the iteration; otherwise the table will remain locked. 
   * 预计只有结果为一行，我们必须执行另一个sqlite3_step()函数，否则这个表将会保持锁定。*/
  rc = sqlite3_step(s);
  if( rc==SQLITE_DONE ){
    *pValues = values;//用pValues来存储结果
    return SQLITE_OK;
  }

  freeStringArray(v->nColumn, values);//删除values，释放资源
  return rc;
}

/* delete from %_content where rowid = [iRow ] */
/* 这个函数用来删除表中的内容 */
static int content_delete(fulltext_vtab *v, sqlite_int64 iRow){
  sqlite3_stmt *s;
  int rc = sql_get_statement(v, CONTENT_DELETE_STMT, &s);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_int64(s, 1, iRow);
  if( rc!=SQLITE_OK ) return rc;

  return sql_single_step_statement(v, CONTENT_DELETE_STMT, &s);
}

/* select rowid, doclist from %_term
 *  where term = [pTerm] and segment = [iSegment]
 * If found, returns SQLITE_ROW; the caller must free the
 * returned doclist.  If no rows found, returns SQLITE_DONE. 
 * 这个函数实现在项中查询
 * 如果找到所要的数据，返回SQLITE_ROW;函数执行结束后必须删除返回doclist。
 * 如果没找到，返回SQLITE_DONE */
static int term_select(fulltext_vtab *v, const char *pTerm, int nTerm,
                       int iSegment,
                       sqlite_int64 *rowid, DocList *out){
  sqlite3_stmt *s;
  int rc = sql_get_statement(v, TERM_SELECT_STMT, &s);//得到查询语句
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_text(s, 1, pTerm, nTerm, SQLITE_STATIC);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_int(s, 2, iSegment);
  if( rc!=SQLITE_OK ) return rc;

  rc = sql_step_statement(v, TERM_SELECT_STMT, &s);
  if( rc!=SQLITE_ROW ) return rc;

  *rowid = sqlite3_column_int64(s, 0);
  /*用一个DocList类型指针来保存得到的数据*/
  docListInit(out, DL_DEFAULT,
              sqlite3_column_blob(s, 1), sqlite3_column_bytes(s, 1));

  /* We expect only one row.  We must execute another sqlite3_step()
   * to complete the iteration; otherwise the table will remain locked. 
   * 预计只有结果为一行，我们必须执行另一个sqlite3_step()函数，否则这个表将会保持锁定。*/
  rc = sqlite3_step(s);
  return rc==SQLITE_DONE ? SQLITE_ROW : rc;
}

/* Load the segment doclists for term pTerm and merge them in
** appropriate order into out.  Returns SQLITE_OK if successful.  If
** there are no segments for pTerm, successfully returns an empty
** doclist in out.
** 载入分段doclists，并且将它们按适当的顺序合并。如果成功返回SQLITE_OK。
** 如果没有和pTerm合并的段，返回一个空的doclist。
**
** Each document consists of 1 or more "columns".  The number of
** columns is v->nColumn.  If iColumn==v->nColumn, then return
** position information about all columns.  If iColumn<v->nColumn,
** then only return position information about the iColumn-th column
** (where the first column is 0).
** 每个文件都包含有一个或多个列。列的数目等于v->nColumn。
** 如果iColumn==v->nColumn，则返回所有列的位置信息。
** 如果iColumn<v->nColumn，则只返回第iColumn列的位置信息。
*/
static int term_select_all(
  fulltext_vtab *v,     /* The fulltext index we are querying against 正在查询的全文索引*/
  int iColumn,          /* If <nColumn, only look at the iColumn-th column 如果小于总列数，则仅视为第iColumn列*/
  const char *pTerm,    /* The term whose posting lists we want 这个项用于记录我们希望得到的内容列表*/
  int nTerm,            /* Number of bytes in pTerm 用以记录pTerm的字节数*/
  DocList *out          /* Write the resulting doclist here 用来代表作为结果*/
){
  DocList doclist;
  sqlite3_stmt *s;
  int rc = sql_get_statement(v, TERM_SELECT_ALL_STMT, &s);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_text(s, 1, pTerm, nTerm, SQLITE_STATIC);
  if( rc!=SQLITE_OK ) return rc;

  docListInit(&doclist, DL_DEFAULT, 0, 0);

  /* TODO(shess) Handle schema and busy errors. */
  while( (rc=sql_step_statement(v, TERM_SELECT_ALL_STMT, &s))==SQLITE_ROW ){
    DocList old;

    /* TODO(shess) If we processed doclists from oldest to newest, we
    ** could skip the malloc() involved with the following call.  For
    ** now, I'd rather keep this logic similar to index_insert_term().
    ** We could additionally drop elements when we see deletes, but
    ** that would require a distinct version of docListAccumulate().
    */
    docListInit(&old, DL_DEFAULT,
                sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0));

    if( iColumn<v->nColumn ){   /* querying a single column 只查询一列 */
      docListRestrictColumn(&old, iColumn);
    }

    /* doclist contains the newer data, so write it over old.  Then
    ** steal accumulated result for doclist.
    ** doclist里面包含新的数据，所以将它的元素覆盖写入到old中
    */
    docListAccumulate(&old, &doclist);
    docListDestroy(&doclist);
    doclist = old;
  }
  if( rc!=SQLITE_DONE ){
    docListDestroy(&doclist);
    return rc;
  }

  docListDiscardEmpty(&doclist);
  *out = doclist;
  return SQLITE_OK;
}

/* insert into %_term (rowid, term, segment, doclist)
               values ([piRowid], [pTerm], [iSegment], [doclist])
** Lets sqlite select rowid if piRowid is NULL, else uses *piRowid.
**
** NOTE(shess) piRowid is IN, with values of "space of int64" plus
** null, it is not used to pass data back to the caller.
** 这个函数用来在%_term中插入值。
** 如果piRowid为NULL，则让sqlite选择伪列，否则使用*piRowid所指向的伪列。
*/
static int term_insert(fulltext_vtab *v, sqlite_int64 *piRowid,
                       const char *pTerm, int nTerm,
                       int iSegment, DocList *doclist){
  sqlite3_stmt *s;
  int rc = sql_get_statement(v, TERM_INSERT_STMT, &s);
  if( rc!=SQLITE_OK ) return rc;
  //绑定对应的要插入的值
  if( piRowid==NULL ){
    rc = sqlite3_bind_null(s, 1);
  }else{
    rc = sqlite3_bind_int64(s, 1, *piRowid);
  }
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_text(s, 2, pTerm, nTerm, SQLITE_STATIC);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_int(s, 3, iSegment);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_blob(s, 4, doclist->pData, doclist->nData, SQLITE_STATIC);
  if( rc!=SQLITE_OK ) return rc;

  return sql_single_step_statement(v, TERM_INSERT_STMT, &s);
}

/* update %_term set doclist = [doclist] where rowid = [rowid] */
/* 这个函数用来更新%_term中的值 */
static int term_update(fulltext_vtab *v, sqlite_int64 rowid,
                       DocList *doclist){
  sqlite3_stmt *s;
  int rc = sql_get_statement(v, TERM_UPDATE_STMT, &s);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_blob(s, 1, doclist->pData, doclist->nData, SQLITE_STATIC);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_int64(s, 2, rowid);
  if( rc!=SQLITE_OK ) return rc;

  return sql_single_step_statement(v, TERM_UPDATE_STMT, &s);
}
/* 这个函数用来对%_term中的一行进行删除操作 */
static int term_delete(fulltext_vtab *v, sqlite_int64 rowid){
  sqlite3_stmt *s;
  int rc = sql_get_statement(v, TERM_DELETE_STMT, &s);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_bind_int64(s, 1, rowid);
  if( rc!=SQLITE_OK ) return rc;

  return sql_single_step_statement(v, TERM_DELETE_STMT, &s);
}

/*
** Free the memory used to contain a fulltext_vtab structure.
** 销毁一个fulltext_vtab结构，依次置空它的数据成员，释放占用的内存
*/
static void fulltext_vtab_destroy(fulltext_vtab *v){
  int iStmt, i;

  TRACE(("FTS1 Destroy %p\n", v));
  for( iStmt=0; iStmt<MAX_STMT; iStmt++ ){
    if( v->pFulltextStatements[iStmt]!=NULL ){
      sqlite3_finalize(v->pFulltextStatements[iStmt]);
      v->pFulltextStatements[iStmt] = NULL;
    }
  }

  if( v->pTokenizer!=NULL ){
    v->pTokenizer->pModule->xDestroy(v->pTokenizer);
    v->pTokenizer = NULL;
  }
  
  free(v->azColumn);
  for(i = 0; i < v->nColumn; ++i) {
    sqlite3_free(v->azContentColumn[i]);
  }
  free(v->azContentColumn);
  free(v);
}

/*
** Token types for parsing the arguments to xConnect or xCreate.
** 定义令牌类型和对应的值
*/
#define TOKEN_EOF         0    /* End of file 文件终点*/
#define TOKEN_SPACE       1    /* Any kind of whitespace 空格*/
#define TOKEN_ID          2    /* An identifier 标识符*/
#define TOKEN_STRING      3    /* A string literal 字符串*/
#define TOKEN_PUNCT       4    /* A single punctuation character 控制字符*/

/*
** If X is a character that can be used in an identifier then
** IdChar(X) will be true.  Otherwise it is false.
** 如果X是一个在标识符中的普通字符，则IdChar(X)的值为真，如果是控制字符，则IdChar(X)值为假。
** For ASCII, any character with the high-order bit set is
** allowed in an identifier.  For 7-bit characters, 
** sqlite3IsIdChar[X] must be 1.
**
** Ticket #1066.  the SQL standard does not allow '$' in the
** middle of identfiers.  But many SQL implementations do. 
** SQLite will allow '$' in identifiers for compatibility.
** But the feature is undocumented.
** SQL标准中不允许标识符中间出现'$'，但很多SQL实现中都存在这种用法。
** SQLite允许在标识符中出现'$'，但这个特征是非正式的。
*/
static const char isIdChar[] = {
/* x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF */
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 2x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 3x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 4x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,  /* 5x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 6x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,  /* 7x */
};
#define IdChar(C)  (((c=C)&0x80)!=0 || (c>0x1f && isIdChar[c-0x20]))
/* 如果X是一个在标识符中的普通字符，则IdChar(X)的值为真，如果是控制字符，则IdChar(X)值为假。*/

/*
** Return the length of the token that begins at z[0]. 
** Store the token type in *tokenType before returning.
** 返回令牌的长度，从z[0]开始计算。
** 在函数返回前，用*tokenType记录令牌类型。
*/
static int getToken(const char *z, int *tokenType){
  int i, c;     //i是一个计数器
  switch( *z ){
    //z[0]的字符代表文件终点，返回令牌的长度是0
    case 0: {
      *tokenType = TOKEN_EOF;
      return 0;
    }
    //z[0]的字符是空格
    case ' ': case '\t': case '\n': case '\f': case '\r': {
      for(i=1; safe_isspace(z[i]); i++){} //对于连续的空格，返回空格数i
      *tokenType = TOKEN_SPACE;
      return i;
    }
    //z[0]的字符是'`'、'\''、'"'，代表后面是一串字符，返回这串字符的长度。
    case '`':
    case '\'':
    case '"': {
      int delim = z[0];  		//delim用来存储z[0]，即代表'`'、'\''、'"'
      for(i=1; (c=z[i])!=0; i++){ 	//在ASCII编码中0代表空字符，如果z[i]不是空字符，计数器加1
        if( c==delim ){			//如果z[i]的字符与delim相同
          if( z[i+1]==delim ){		//如果z[i+1]也与delim相同
            i++;			//说明这一串字符没有结束，计数器加1
          }else{			//如果z[i+1]与delim不同
            break;			//直接跳出for循环
          }
        }
      }
      *tokenType = TOKEN_STRING;
      return i + (c!=0);                //如果for循环中，z[i]以空字符结束，返回i，否则返回i+1
    }
    //z[0]的字符是'['，说明是一个标识符，长度计算包括左右方括号
    case '[': {
      for(i=1, c=z[0]; c!=']' && (c=z[i])!=0; i++){}//如果c所代表的字符不是']'，且c的下一位字符不是空字符，计数加1
      *tokenType = TOKEN_ID;
      return i;
    }
    //z[0]是其他字符
    default: {
      if( !IdChar(*z) ){        //如果z[0]是控制字符，则跳出switch语句，如果不是则进行下面的for循环
        break;
      }
      for(i=1; IdChar(z[i]); i++){}	//如果z[i]是普通字符，计数器加1，否则结束循环
      *tokenType = TOKEN_ID;
      return i;
    }
  }
  *tokenType = TOKEN_PUNCT;         //直接跳出switch语句，代表这个字符为控制字符
  return 1;
}

/*
** A token extracted from a string is an instance of the following
** structure.
** 下面的结构体是一个从字符串中提取的令牌的例子
*/
typedef struct Token {
  const char *z;       /* Pointer to token text.  Not '\000' terminated 指向令牌文本的指针，不能使用'\000'作为终结 */
  short int n;         /* Length of the token text in bytes. 令牌文本的长度 */
} Token;

/*
** Given a input string (which is really one of the argv[] parameters
** passed into xConnect or xCreate) split the string up into tokens.
** Return an array of pointers to '\000' terminated strings, one string
** for each non-whitespace token.
**
** The returned array is terminated by a single NULL pointer.
**
** Space to hold the returned array is obtained from a single
** malloc and should be freed by passing the return value to free().
** The individual strings within the token list are all a part of
** the single memory allocation and will all be freed at once.
**
** 这个函数的作用是，给定一个字符串，将它分割为用几个令牌表示。
** 返回一个指针数组指向一个以'\000'结尾的字符串。
**
** 返回数组的终结符是一个空指针
**
** 保存返回数组的空间有一个单独的分配内存提供，并且使用free函数释放。
*/
static char **tokenizeString(const char *z, int *pnToken){
  int nToken = 0;
  Token *aToken = malloc( strlen(z) * sizeof(aToken[0]) );	//定义一个记录令牌的数组
  int n = 1;
  int e, i;
  int totalSize = 0;
  char **azToken;
  char *zCopy;
  while( n>0 ){
    n = getToken(z, &e);	//得到z所指向的字符串长度
    if( e!=TOKEN_SPACE ){	//如果z所指向的是空格，就跳过空格
      aToken[nToken].z = z;	//如果不是空格，使用aToken分别记录z和z的长度
      aToken[nToken].n = n;
      nToken++;
      totalSize += n+1;
    }
    z += n;
  }
  azToken = (char**)malloc( nToken*sizeof(char*) + totalSize );
  zCopy = (char*)&azToken[nToken];
  nToken--;
  for(i=0; i<nToken; i++){
    azToken[i] = zCopy;
    n = aToken[i].n;
    memcpy(zCopy, aToken[i].z, n);
    zCopy[n] = 0;
    zCopy += n+1;
  }
  azToken[nToken] = 0;
  free(aToken);
  *pnToken = nToken;
  return azToken;
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** 将一个SQL风格的引用字符串通过去掉引用字符，转换为一个普通字符串。
** 这个转换是在原地完成的。如果输入的字符串不是以一个引用字符开头，则无操作。
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
static void dequoteString(char *z){
  int quote;
  int i, j;
  if( z==0 ) return;		//如果z是空字符则无操作。
  quote = z[0];			//quote记录z[0]的ASCII码	
  switch( quote ){		//如果quote所表示的字符是引用字符，则跳出switch执行下面的for循环，否则无操作。
    case '\'':  break;
    case '"':   break;
    case '`':   break;                /* For MySQL compatibility 适用于MySQL */
    case '[':   quote = ']';  break;  /* For MS SqlServer compatibility 适用于MS SqlServer如果以'['开头，则结尾应是']' */
    default:    return;
  }
  for(i=1, j=0; z[i]; i++){	//z[0]是引用字符，故循环从z[1]开始
    if( z[i]==quote ){		//如果z[i]是和quote相同的引用字符，说明此时是字符串结尾
      if( z[i+1]==quote ){	//如果z[i+1]也是和quote相同的引用字符
        z[j++] = quote;		//将z[i+1]前移
        i++;			//这里i++，是为了跳过已知与quote相同的字符。
      }else{
        z[j++] = 0;		//如果z[i+1]和quote不同，则将z的结尾置空，结束循环。
        break;
      }
    }else{			//如果z[i]的字符与quote不相同，说明还未到字符串结尾
      z[j++] = z[i];		//字符前移一位
    }
  }
}

/*
** The input azIn is a NULL-terminated list of tokens.  Remove the first
** token and all punctuation tokens.  Remove the quotes from
** around string literal tokens.
**
** 输入的azIn是一个以空结尾的令牌列表。
** 删除第一个令牌表示的内容以及所有标点符号令牌。删除文字令牌的引用字符。
** Example:
**
**     input:      tokenize chinese ( 'simplifed' , 'mixed' )
**     output:     chinese simplifed mixed
**
** Another example:
**
**     input:      delimiters ( '[' , ']' , '...' )
**     output:     [ ] ...
*/
static void tokenListToIdList(char **azIn){
  int i, j;
  if( azIn ){
    for(i=0, j=-1; azIn[i]; i++){
      if( safe_isalnum(azIn[i][0]) || azIn[i][1] ){ //如果第i个令牌是数字或字母，并且长度不为0。
        dequoteString(azIn[i]); //删去引用字符
        if( j>=0 ){
          azIn[j] = azIn[i];
        }
        j++;
      }
    }
    azIn[j] = 0;    //使令牌列表以0结尾
  }
}


/*
** Find the first alphanumeric token in the string zIn.  Null-terminate
** this token.  Remove any quotation marks.  And return a pointer to
** the result.
** 在字符串zIn中找到第一个字母数字令牌，令牌以空结尾。删除所有引用符号，返回一个指向结果的指针。
*/
static char *firstToken(char *zIn, char **pzTail){
  int n, ttype;
  while(1){
    n = getToken(zIn, &ttype);		//获得zIn第一个令牌的类型
    if( ttype==TOKEN_SPACE ){		//如果是空格符，则跳过空格
      zIn += n;
    }else if( ttype==TOKEN_EOF ){	//如果是文件结尾符，则返回0
      *pzTail = zIn;
      return 0;
    }else{				
      zIn[n] = 0;		//对于其他类型，令zIn最后一个元素为0
      *pzTail = &zIn[1];	//pzTail指向zIn[1]
      dequoteString(zIn);	//删除zIn中的引用符号
      return zIn;
    }
  }
  /*NOTREACHED*/
}

/* Return true if...
**
**   *  s begins with the string t, ignoring case
**   *  s is longer than t
**   *  The first character of s beyond t is not a alphanumeric
** 
** Ignore leading space in *s.
**
** To put it another way, return true if the first token of
** s[] is t[].
**
** 以下情况返回true
**   *  s以字符串t开头，忽略大小写
**   *  s的长度比t长
**   *  s的第一个字符除了t没有其他的字母数字符号
** 忽略s开头的空格
** 话句话说，如果s中的第一个符号是t，则返回true
*/
static int startsWith(const char *s, const char *t){
  while( safe_isspace(*s) ){ s++; }
  while( *t ){
    if( safe_tolower(*s++)!=safe_tolower(*t++) ) return 0;
  }
  return *s!='_' && !safe_isalnum(*s);
}

/*
** An instance of this structure defines the "spec" of a
** full text index.  This structure is populated by parseSpec
** and use by fulltextConnect and fulltextCreate.
** 这个结构体的例子定义了一个全文索引的"说明"
** 这个结构体由parseSpec构造，并且在函数fulltextConnect和函数fulltextCreate中使用。
*/
typedef struct TableSpec {
  const char *zDb;         /* Logical database name 逻辑数据库名 */
  const char *zName;       /* Name of the full-text index 全文索引名 */
  int nColumn;             /* Number of columns to be indexed 组成索引的列数 */
  char **azColumn;         /* Original names of columns to be indexed 组成索引的列的原始名 */
  char **azContentColumn;  /* Column names for %_content 与内容相关的列名 */
  char **azTokenizer;      /* Name of tokenizer and its arguments 分词器的名称与他的参数 */
} TableSpec;

/*
** Reclaim all of the memory used by a TableSpec
** 使用一个TableSpec回收内存
*/
static void clearTableSpec(TableSpec *p) {
  free(p->azColumn);
  free(p->azContentColumn);
  free(p->azTokenizer);
}

/* Parse a CREATE VIRTUAL TABLE statement, which looks like this:
 * 解析一个类似如下形式的创建虚拟表的语句
 * CREATE VIRTUAL TABLE email
 *        USING fts1(subject, body, tokenize mytokenizer(myarg))
 *
 * We return parsed information in a TableSpec structure.
 * 在结构体TableSpec中返回解析后的信息
 */
static int parseSpec(TableSpec *pSpec, int argc, const char *const*argv,
                     char**pzErr){
  int i, n;
  char *z, *zDummy;
  char **azArg;
  const char *zTokenizer = 0;    /* argv[] entry describing the tokenizer argv[]接口用来描述分词器 */

  assert( argc>=3 );
  /* Current interface:
  ** argv[0] - module name
  ** argv[1] - database name
  ** argv[2] - table name
  ** argv[3..] - columns, optionally followed by tokenizer specification
  **             and snippet delimiters specification.
  **
  ** 当前接口:
  ** argv[0] - 模块名
  ** argv[1] - 数据库名
  ** argv[2] - 表名
  ** argv[3..] - 列，以及分词器规则和段分隔符规则
  */

  /* Make a copy of the complete argv[][] array in a single allocation.
  ** The argv[][] array is read-only and transient.  We can write to the
  ** copy in order to modify things and the copy is persistent.
  ** 配置一个独立完整的argv[][]数组的备份。
  ** argv[][]数组是只读的并且只保存短暂的时间，
  ** 我们可以再备份中修改内容，并且备份是可以长时间保存的。
  */
  memset(pSpec, 0, sizeof(*pSpec)); //将pSpec所在的内存空间清零
  for(i=n=0; i<argc; i++){          //计算argv[]所有元素的长度总和，并用n保存
    n += strlen(argv[i]) + 1;
  }
  azArg = malloc( sizeof(char*)*argc + n );//为azArg分配一块可以保存argv[]的内存
  if( azArg==0 ){
    return SQLITE_NOMEM;
  }
  z = (char*)&azArg[argc];
  for(i=0; i<argc; i++){    //将argv[]中的数据循环复制进z所指向的内存空间
    azArg[i] = z;           //azArg[]数组依次保存z中的数据
    strcpy(z, argv[i]);
    z += strlen(z)+1;
  }

  /* Identify the column names and the tokenizer and delimiter arguments
  ** in the argv[][] array.
  ** 在argv[][]数组中确定列名、分词器和分隔符参数
  */
  pSpec->zDb = azArg[1];	//将azArg[1]中保存的数据库名赋予pSpec->zDb		
  pSpec->zName = azArg[2];	//将azArg[2]中保存的全文索引名赋予pSpec->aName
  pSpec->nColumn = 0;		//列数赋值为0
  pSpec->azColumn = azArg;
  zTokenizer = "tokenize simple";
  for(i=3; i<argc; ++i){
    if( startsWith(azArg[i],"tokenize") ){  //azArg[i]如果保存的是分词器
      zTokenizer = azArg[i];                //zTokenizer赋值为azArg[i]
    }else{
      z = azArg[pSpec->nColumn] = firstToken(azArg[i], &zDummy);
      pSpec->nColumn++;
    }
  }
  if( pSpec->nColumn==0 ){
    azArg[0] = "content";
    pSpec->nColumn = 1;
  }

  /*
  ** Construct the list of content column names.
  **
  ** Each content column name will be of the form cNNAAAA
  ** where NN is the column number and AAAA is the sanitized
  ** column name.  "sanitized" means that special characters are
  ** converted to "_".  The cNN prefix guarantees that all column
  ** names are unique.
  **
  ** The AAAA suffix is not strictly necessary.  It is included
  ** for the convenience of people who might examine the generated
  ** %_content table and wonder what the columns are used for.
  **  
  ** 构造内容列名称列表
  **
  ** 每一个内容列名称都是cNNAAAA形式，NN是列号，AAAA是经过清理的列名。
  ** "清理"的意思是指特殊字符转化为了"_"。cNN前缀保证所有的列名是唯一的。
  **
  ** AAAA后缀并不是严格必须的
  */
  pSpec->azContentColumn = malloc( pSpec->nColumn * sizeof(char *) );   //分配一块内存空间给pSpec->azContentColumn
  if( pSpec->azContentColumn==0 ){  //如果pSpec->azContentColumn的大小为0
    clearTableSpec(pSpec);          //销毁pSpec
    return SQLITE_NOMEM;
  }
  for(i=0; i<pSpec->nColumn; i++){  //如果pSpec->azContentColumn的大小不为0，依次赋予列名
    char *p;
    pSpec->azContentColumn[i] = sqlite3_mprintf("c%d%s", i, azArg[i]);
    for (p = pSpec->azContentColumn[i]; *p ; ++p) {
      if( !safe_isalnum(*p) ) *p = '_';
    }
  }

  /*
  ** Parse the tokenizer specification string.
  ** 解析分词器规则字符串
  */
  pSpec->azTokenizer = tokenizeString(zTokenizer, &n);
  tokenListToIdList(pSpec->azTokenizer);

  return SQLITE_OK;
}

/*
** Generate a CREATE TABLE statement that describes the schema of
** the virtual table.  Return a pointer to this schema string.
**
** Space is obtained from sqlite3_mprintf() and should be freed
** using sqlite3_free().
** 通过描述虚拟表的结构，生成一个CREATE TABLE语句，返回一个指向结构字符串的指针。
** 由函数sqlite3_mprintf()分配空间，使用sqlite3_free()函数释放空间
*/
static char *fulltextSchema(
  int nColumn,                  /* Number of columns 列数 */
  const char *const* azColumn,  /* List of columns 用来存储需要添加到表中的列 */
  const char *zTableName        /* Name of the table 表名 */
){
  int i;
  char *zSchema, *zNext;
  const char *zSep = "(";
  zSchema = sqlite3_mprintf("CREATE TABLE x");
  for(i=0; i<nColumn; i++){	//循环构造创建表语句
    zNext = sqlite3_mprintf("%s%s%Q", zSchema, zSep, azColumn[i]);	
    sqlite3_free(zSchema);	//销毁当前的zSchema
    zSchema = zNext;		//赋予zSchema新构造的语句
    zSep = ",";			//列与列之间用逗号隔开
  }
  zNext = sqlite3_mprintf("%s,%Q)", zSchema, zTableName);	//在语句中加入表名
  sqlite3_free(zSchema);	//销毁当前的zSchema
  return zNext;			//返回指向保存语句的内存空间的指针
}

/*
** Build a new sqlite3_vtab structure that will describe the
** fulltext index defined by spec.
** 通过说明描述全文索引的定义，创建一个新的sqlite3_vtab结构体
*/
static int constructVtab(
  sqlite3 *db,              /* The SQLite database connection 用连接的SQLite数据库 */
  TableSpec *spec,          /* Parsed spec information from parseSpec() 通过parseSpec函数解析说明信息 */
  sqlite3_vtab **ppVTab,    /* Write the resulting vtab structure here 作为结果的vtab结构体 */
  char **pzErr              /* Write any error message here 错误信息 */
){
  int rc;
  int n;
  fulltext_vtab *v = 0;
  const sqlite3_tokenizer_module *m = NULL;
  char *schema;

  v = (fulltext_vtab *) malloc(sizeof(fulltext_vtab));  //为新的fulltext_vtab结构分配内存空间
  if( v==0 ) return SQLITE_NOMEM;
  memset(v, 0, sizeof(*v));     //将v的内存空间清零
  /* sqlite will initialize v->base 初始化v的数据成员 */
  v->db = db;
  v->zDb = spec->zDb;       /* Freed when azColumn is freed 当释放azColumn时同时释放 */
  v->zName = spec->zName;   /* Freed when azColumn is freed 当释放azColumn时同时释放 */
  v->nColumn = spec->nColumn;
  v->azContentColumn = spec->azContentColumn;
  spec->azContentColumn = 0;
  v->azColumn = spec->azColumn;
  spec->azColumn = 0;

  if( spec->azTokenizer==0 ){
    return SQLITE_NOMEM;
  }
  /* TODO(shess) For now, add new tokenizers as else if clauses. */
  if( spec->azTokenizer[0]==0 || startsWith(spec->azTokenizer[0], "simple") ){	//如果分词器以0开头，或者以"simple"开头
    sqlite3Fts1SimpleTokenizerModule(&m);	//则设置分词器模式为"simple"
  }else if( startsWith(spec->azTokenizer[0], "porter") ){	//如果分词器以"porter"开头
    sqlite3Fts1PorterTokenizerModule(&m);	//设置分词器模式为"porter"
  }else{					//否则输出错误信息
    *pzErr = sqlite3_mprintf("unknown tokenizer: %s", spec->azTokenizer[0]);
    rc = SQLITE_ERROR;
    goto err;   //跳至错误处理
  }
  //创建一个分词器
  for(n=0; spec->azTokenizer[n]; n++){}
  if( n ){  //n不为0，说明分词器的名称、参数已设定，按设定的参数创建
    rc = m->xCreate(n-1, (const char*const*)&spec->azTokenizer[1],
                    &v->pTokenizer);
  }else{
    rc = m->xCreate(0, 0, &v->pTokenizer);
  }
  if( rc!=SQLITE_OK ) goto err; //如果没有正常运行，跳转到错误处理 
  v->pTokenizer->pModule = m;

  /* TODO: verify the existence of backing tables foo_content, foo_term */

  schema = fulltextSchema(v->nColumn, (const char*const*)v->azColumn,
                          spec->zName); //生成一个创建表的语句
  rc = sqlite3_declare_vtab(db, schema);    //根据创建表的语句，设定虚拟表的结构
  sqlite3_free(schema); //释放schema
  if( rc!=SQLITE_OK ) goto err; 

  memset(v->pFulltextStatements, 0, sizeof(v->pFulltextStatements));    //将v->pFulltextStatements清零

  *ppVTab = &v->base;
  TRACE(("FTS1 Connect %p\n", v));

  return rc;

err:    //错误处理，删除v
  fulltext_vtab_destroy(v);
  return rc;
}

/* 这个函数的作用是将一个全文检索与一个表连接 */
static int fulltextConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVTab,
  char **pzErr
){
  TableSpec spec;       //将argv中的内容解析后记录到一个TableSpec结构中
  int rc = parseSpec(&spec, argc, argv, pzErr);
  if( rc!=SQLITE_OK ) return rc;

  rc = constructVtab(db, &spec, ppVTab, pzErr); //构造一个sqlite3_vtab结构保存spec中的全文索引的定义
  clearTableSpec(&spec);
  return rc;
}

  /* The %_content table holds the text of each document, with
  ** the rowid used as the docid.
  ** %_content表保存每一个文件的文本，并使用伪列作为文档编号。
  **
  ** The %_term table maps each term to a document list blob
  ** containing elements sorted by ascending docid, each element
  ** encoded as:
  **
  **   docid varint-encoded
  **   token elements:
  **     position+1 varint-encoded as delta from previous position
  **     start offset varint-encoded as delta from previous start offset
  **     end offset varint-encoded as delta from start offset
  **
  ** %_term表映射每个项到一个文档列表对象，并且其中包括的元素按文档编号升序排列
  ** 每一个元素按如下方式编码:
  **
  **   文档编号变形编码
  **   令牌元素:
  **     前一个位置的增量作为后一个位置的变形编码
  **	 之前的起始偏移量的增量作为起始偏移量的变形编码
  **	 起始偏移量的增量作为结尾偏移量的变形编码  
  **
  ** The sentinel position of 0 indicates the end of the token list.
  ** 0的位置标志着令牌列表的结束
  **
  ** Additionally, doclist blobs are chunked into multiple segments,
  ** using segment to order the segments.  New elements are added to
  ** the segment at segment 0, until it exceeds CHUNK_MAX.  Then
  ** segment 0 is deleted, and the doclist is inserted at segment 1.
  ** If there is already a doclist at segment 1, the segment 0 doclist
  ** is merged with it, the segment 1 doclist is deleted, and the
  ** merged doclist is inserted at segment 2, repeating those
  ** operations until an insert succeeds.
  ** 此外，文档列表对象被分成多个段，并且按顺序排列。新的元素会被添加到段0，
  ** 直到它超过段的最大值CHUNK_MAX。此时，段0会被删除，文档列表会被插入到段1.
  ** 如果段1中已有一个文档列表，就将段0中的文档列表与段1中的合并，然后删除段1，
  ** 将合并后的文档列表插入到段2，重复以上操作直到插入成功。
  **
  ** Since this structure doesn't allow us to update elements in place
  ** in case of deletion or update, these are simply written to
  ** segment 0 (with an empty token list in case of deletion), with
  ** docListAccumulate() taking care to retain lower-segment
  ** information in preference to higher-segment information.
  ** 由于这种结构不允许我们在原始的位置更新元素。
  ** 要删除或更新，只能写入到段0中(使用空的令牌列表进行删除操作)，
  ** 使用docListAccumulate()函数要注意，低段的信息优先于高端信息?
  */
  /* TODO(shess) Provide a VACUUM type operation which both removes
  ** deleted elements which are no longer necessary, and duplicated
  ** elements.  I suspect this will probably not be necessary in
  ** practice, though.
  */
  //这个函数用来执行全文检索中的创建表的语句
static int fulltextCreate(sqlite3 *db, void *pAux,
                          int argc, const char * const *argv,
                          sqlite3_vtab **ppVTab, char **pzErr){
  int rc;
  TableSpec spec;
  StringBuffer schema;
  TRACE(("FTS1 Create\n"));

  rc = parseSpec(&spec, argc, argv, pzErr);	//解析argv中的操作信息
  if( rc!=SQLITE_OK ) return rc;

  initStringBuffer(&schema);	//初始化字符串缓冲区
  append(&schema, "CREATE TABLE %_content(");	//向字符串缓冲区schema中添加创建%_content表的语句
  appendList(&schema, spec.nColumn, spec.azContentColumn);	//向schema中的语句中添加要创建的列
  append(&schema, ")");
  rc = sql_exec(db, spec.zDb, spec.zName, schema.s);	//执行schema中存储的语句
  free(schema.s);	//释放schema中语句
  if( rc!=SQLITE_OK ) goto out;	//如果没有成功运行，跳至处理语句
  //执行创建%_term表的语句，并指定其中的列
  rc = sql_exec(db, spec.zDb, spec.zName,
    "create table %_term(term text, segment integer, doclist blob, "
                        "primary key(term, segment));");
  if( rc!=SQLITE_OK ) goto out;

  rc = constructVtab(db, &spec, ppVTab, pzErr);

out:
  clearTableSpec(&spec);
  return rc;
}

/* Decide how to handle an SQL query. 决定如何处理SQL查询 */
static int fulltextBestIndex(sqlite3_vtab *pVTab, sqlite3_index_info *pInfo){
  int i;
  TRACE(("FTS1 BestIndex\n"));

  for(i=0; i<pInfo->nConstraint; ++i){
    const struct sqlite3_index_constraint *pConstraint; //新建一个sqlite3_index_constraint指针指向查询约束
    pConstraint = &pInfo->aConstraint[i];
    if( pConstraint->usable ) {	//如果约束为可用状态
      if( pConstraint->iColumn==-1 &&	//满足条件则按行查找
          pConstraint->op==SQLITE_INDEX_CONSTRAINT_EQ ){
        pInfo->idxNum = QUERY_ROWID;      /* lookup by rowid */
        TRACE(("FTS1 QUERY_ROWID\n"));
      } else if( pConstraint->iColumn>=0 &&	//满足条件则全文本查找
                 pConstraint->op==SQLITE_INDEX_CONSTRAINT_MATCH ){
        /* full-text search */
        pInfo->idxNum = QUERY_FULLTEXT + pConstraint->iColumn;
        TRACE(("FTS1 QUERY_FULLTEXT %d\n", pConstraint->iColumn));
      } else continue;	//约束为不可用，函数继续

      pInfo->aConstraintUsage[i].argvIndex = 1;
      pInfo->aConstraintUsage[i].omit = 1;

      /* An arbitrary value for now.
       * TODO: Perhaps rowid matches should be considered cheaper than
       * full-text searches. */
      pInfo->estimatedCost = 1.0;   

      return SQLITE_OK;
    }
  }
  pInfo->idxNum = QUERY_GENERIC;
  return SQLITE_OK;
}
//这个函数用来使全文检索与虚拟表断开连接
static int fulltextDisconnect(sqlite3_vtab *pVTab){
  TRACE(("FTS1 Disconnect %p\n", pVTab));
  fulltext_vtab_destroy((fulltext_vtab *)pVTab);//断开后销毁pVTab
  return SQLITE_OK;
}
//这个函数用来执行全文检索中的删除语句
static int fulltextDestroy(sqlite3_vtab *pVTab){
  fulltext_vtab *v = (fulltext_vtab *)pVTab;
  int rc;

  TRACE(("FTS1 Destroy %p\n", pVTab));
  rc = sql_exec(v->db, v->zDb, v->zName,
                "drop table if exists %_content;"
                "drop table if exists %_term;"
                );	//执行删除语句
  if( rc!=SQLITE_OK ) return rc;

  fulltext_vtab_destroy((fulltext_vtab *)pVTab);//函数结束后销毁pVTab
  return SQLITE_OK;
}
//当开始全文检索式，使用这个函数创建一个游标
static int fulltextOpen(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCursor){
  fulltext_cursor *c;

  c = (fulltext_cursor *) calloc(sizeof(fulltext_cursor), 1);
  /* sqlite will initialize c->base sqlite将会初始化c->base */
  *ppCursor = &c->base;
  TRACE(("FTS1 Open %p: %p\n", pVTab, c));

  return SQLITE_OK;
}


/* Free all of the dynamically allocated memory held by *q
** 释放所有q所指向的动态分配内存
*/
static void queryClear(Query *q){
  int i;
  for(i = 0; i < q->nTerms; ++i){//依次释放q中的项
    free(q->pTerms[i].pTerm);
  }
  free(q->pTerms);
  memset(q, 0, sizeof(*q));     //将q所指向的内存清零
}

/* Free all of the dynamically allocated memory held by the
** Snippet
** 释放所有Snippet的动态分配内存
*/
static void snippetClear(Snippet *p){
  //依次释放p所指向的Snippet的数据成员
  free(p->aMatch);
  free(p->zOffset);
  free(p->zSnippet);
  memset(p, 0, sizeof(*p));//将保存Snippet的内存空间清零
}
/*
** Append a single entry to the p->aMatch[] log.
** 这个函数的功能是给一个Snippet添加一个match条目
*/
static void snippetAppendMatch(
  Snippet *p,               /* Append the entry to this snippet 为这个段附加接口 */
  int iCol, int iTerm,      /* The column and query term 表示列与查询项 */
  int iStart, int nByte     /* Offset and size of the match  match的大小和偏移量 */
){
  int i;
  struct snippetMatch *pMatch;
  if( p->nMatch+1>=p->nAlloc ){	        //如果Snippet中的aMatch[]已没有空闲的空间
    p->nAlloc = p->nAlloc*2 + 10;       //增加分配给aMatch[]的空间
    p->aMatch = realloc(p->aMatch, p->nAlloc*sizeof(p->aMatch[0]) );	//扩大aMatch[]在内存中所占的空间
    if( p->aMatch==0 ){		        //如果aMatch[]本来就是空的，则恢复原状，不做操作
      p->nMatch = 0;
      p->nAlloc = 0;
      return;
    }
  }
  i = p->nMatch++;		//match总数加1
  pMatch = &p->aMatch[i];	//pMatch指向新添加的条目
  pMatch->iCol = iCol;		//记录新添加条目的信息
  pMatch->iTerm = iTerm;
  pMatch->iStart = iStart;
  pMatch->nByte = nByte;
}

/*
** Sizing information for the circular buffer used in snippetOffsetsOfColumn()
** 用于snippetOffsetsOfColumn()函数的循环缓冲区的范围信息
*/
#define FTS1_ROTOR_SZ   (32)
#define FTS1_ROTOR_MASK (FTS1_ROTOR_SZ-1)

/*
** Add entries to pSnippet->aMatch[] for every match that occurs against
** document zDoc[0..nDoc-1] which is stored in column iColumn.
** 为每一个match添加条目到pSnippet->aMatch[]，
** 依照储存在列iColumn中的文档zDoc[0..nDoc-1]
*/
static void snippetOffsetsOfColumn(
  Query *pQuery,
  Snippet *pSnippet,
  int iColumn,
  const char *zDoc,
  int nDoc
){
  const sqlite3_tokenizer_module *pTModule;  /* The tokenizer module 分词器模块 */
  sqlite3_tokenizer *pTokenizer;             /* The specific tokenizer 特殊分词器 */
  sqlite3_tokenizer_cursor *pTCursor;        /* Tokenizer cursor 分词器游标 */
  fulltext_vtab *pVtab;                /* The full text index 全文索引 */
  int nColumn;                         /* Number of columns in the index 索引中的列数 */
  const QueryTerm *aTerm;              /* Query string terms 查询语句项 */
  int nTerm;                           /* Number of query string terms 查询语句项的数量 */  
  int i, j;                            /* Loop counters 循环计数器 */
  int rc;                              /* Return code 返回代码 */
  unsigned int match, prevMatch;       /* Phrase search bitmasks 词组搜索位掩码 */
  const char *zToken;                  /* Next token from the tokenizer 分词器的下一个令牌 */
  int nToken;                          /* Size of zToken zToken的大小 */
  int iBegin, iEnd, iPos;              /* Offsets of beginning and end 起始与结尾的偏移量 */

  /* The following variables keep a circular buffer of the last
  ** few tokens 
  ** 下面的几个变量保存着剩下的令牌的循环缓冲区
  */
  unsigned int iRotor = 0;             /* Index of current token 当前令牌的索引 */
  int iRotorBegin[FTS1_ROTOR_SZ];      /* Beginning offset of token 令牌的起始偏移量 */
  int iRotorLen[FTS1_ROTOR_SZ];        /* Length of token 令牌长度 */

  pVtab = pQuery->pFts;
  nColumn = pVtab->nColumn;
  pTokenizer = pVtab->pTokenizer;
  pTModule = pTokenizer->pModule;
  rc = pTModule->xOpen(pTokenizer, zDoc, nDoc, &pTCursor);//准备分词器
  if( rc ) return;
  pTCursor->pTokenizer = pTokenizer;
  aTerm = pQuery->pTerms;
  nTerm = pQuery->nTerms;
  if( nTerm>=FTS1_ROTOR_SZ ){
    nTerm = FTS1_ROTOR_SZ - 1;
  }
  prevMatch = 0;
  while(1){
    rc = pTModule->xNext(pTCursor, &zToken, &nToken, &iBegin, &iEnd, &iPos); //依次载入令牌
    if( rc ) break;
    iRotorBegin[iRotor&FTS1_ROTOR_MASK] = iBegin;       //记录令牌起始偏移量
    iRotorLen[iRotor&FTS1_ROTOR_MASK] = iEnd-iBegin;    //记录令牌长度
    match = 0;
    for(i=0; i<nTerm; i++){     //依次操作每一个查询语句项
      int iCol;
      iCol = aTerm[i].iColumn;  //当前查询语句项的索引列
      if( iCol>=0 && iCol<nColumn && iCol!=iColumn ) continue;//列不同，跳出for循环
      if( aTerm[i].nTerm!=nToken ) continue;    //当前查询语句项的大小与令牌大小不同，跳出for循环
      if( memcmp(aTerm[i].pTerm, zToken, nToken) ) continue;    //比较当前查询语句项和令牌，如果不同，跳出for循环
      if( aTerm[i].iPhrase>1 && (prevMatch & (1<<i))==0 ) continue;
      match |= 1<<i;
      if( i==nTerm-1 || aTerm[i+1].iPhrase==1 ){
        for(j=aTerm[i].iPhrase-1; j>=0; j--){
          int k = (iRotor-j) & FTS1_ROTOR_MASK;
          snippetAppendMatch(pSnippet, iColumn, i-j,
                iRotorBegin[k], iRotorLen[k]);//为pSnippet添加一个match条目
        }
      }
    }
    prevMatch = match<<1;
    iRotor++;
  }
  pTModule->xClose(pTCursor);  
}


/* 王旭源码分析段开始
** Compute all offsets for the current row of the query.  为当前行计算所有查询的偏移量
** If the offsets have already been computed, this routine is a no-op. 如果偏移量已经计算,这个进程是一个空操作
*/
static void snippetAllOffsets(fulltext_cursor *p){
  int nColumn;
  int iColumn, i;
  int iFirst, iLast;
  fulltext_vtab *pFts;

  if( p->snippet.nMatch ) return;/**/
  if( p->q.nTerms==0 ) return;
  pFts = p->q.pFts;
  nColumn = pFts->nColumn;
  iColumn = p->iCursorType - QUERY_FULLTEXT;
  if( iColumn<0 || iColumn>=nColumn ){
    iFirst = 0;
    iLast = nColumn-1;
  }else{
    iFirst = iColumn;
    iLast = iColumn;
  }
  for(i=iFirst; i<=iLast; i++){
    const char *zDoc;
    int nDoc;
    zDoc = (const char*)sqlite3_column_text(p->pStmt, i+1);
    nDoc = sqlite3_column_bytes(p->pStmt, i+1);
    snippetOffsetsOfColumn(&p->q, &p->snippet, i, zDoc, nDoc);
  }
}

/*
** Convert the information in the aMatch[] array of the snippet   
** into the string zOffset[0..nOffset-1]. 转换aMatch[]数组的片段中的信息到字符串的zOffset[0 . . nOffset-1]。
*/
static void snippetOffsetText(Snippet *p){
  int i;
  int cnt = 0;
  StringBuffer sb;
  char zBuf[200];
  if( p->zOffset ) return;
  initStringBuffer(&sb);
  for(i=0; i<p->nMatch; i++){
    struct snippetMatch *pMatch = &p->aMatch[i];
    zBuf[0] = ' ';
    sqlite3_snprintf(sizeof(zBuf)-1, &zBuf[cnt>0], "%d %d %d %d",
        pMatch->iCol, pMatch->iTerm, pMatch->iStart, pMatch->nByte);
    append(&sb, zBuf);
    cnt++;
  }
  p->zOffset = sb.s;
  p->nOffset = sb.len;
}

/*
** zDoc[0..nDoc-1] is phrase of text.  aMatch[0..nMatch-1] are a set     
** of matching words some of which might be in zDoc.  zDoc is column
** number iCol.   
** zDoc[0 . .nDoc-1)是文本的短语。
** aMatch[0 . .nMatch-1)是一组匹配的单词其中一些可能在zDoc。
** zDoc是列数字iCol。
** iBreak is suggested spot in zDoc where we could begin or end an
** excerpt.  Return a value similar to iBreak but possibly adjusted
** to be a little left or right so that the break point is better.
** iBreak是在zDoc中的我们可以开始或结束摘录的建议点。
** 返回一个值类似于iBreak但可能调整有点向左或向右为了让破发点更好。
*/
static int wordBoundary(
  int iBreak,                   /* The suggested break point 建议的断点 */
  const char *zDoc,             /* Document text 文档文本 */
  int nDoc,                     /* Number of bytes in zDoc[] 在zDoc[]中的字节数 */
  struct snippetMatch *aMatch,  /* Matching words 匹配单词 */
  int nMatch,                   /* Number of entries in aMatch[] aMatch[]中的的条目数量 */
  int iCol                      /* The column number for zDoc[] zDoc[]的列数 */
){
  int i;
  if( iBreak<=10 ){
    return 0;
  }
  if( iBreak>=nDoc-10 ){
    return nDoc;
  }
  for(i=0; i<nMatch && aMatch[i].iCol<iCol; i++){}
  while( i<nMatch && aMatch[i].iStart+aMatch[i].nByte<iBreak ){ i++; }
  if( i<nMatch ){
    if( aMatch[i].iStart<iBreak+10 ){
      return aMatch[i].iStart;
    }
    if( i>0 && aMatch[i-1].iStart+aMatch[i-1].nByte>=iBreak ){
      return aMatch[i-1].iStart;
    }
  }
  for(i=1; i<=10; i++){
    if( safe_isspace(zDoc[iBreak-i]) ){
      return iBreak - i + 1;
    }
    if( safe_isspace(zDoc[iBreak+i]) ){
      return iBreak + i + 1;
    }
  }
  return iBreak;
}

/*
** If the StringBuffer does not end in white space, add a single
** space character to the end.
** 如果StringBuffer在空白不结束,添加一个空格字符。
*/
static void appendWhiteSpace(StringBuffer *p){
  if( p->len==0 ) return;
  if( safe_isspace(p->s[p->len-1]) ) return;
  append(p, " ");
}

/*
** Remove white space from teh end of the StringBuffer
** 删除空白囊括StringBuffer的结束
*/
static void trimWhiteSpace(StringBuffer *p){
  while( p->len>0 && safe_isspace(p->s[p->len-1]) ){
    p->len--;
  }
}



/*
** Allowed values for Snippet.aMatch[].snStatus
** Snippet.aMatch[].snStatus的允许的值
*/
#define SNIPPET_IGNORE  0   /* It is ok to omit this match from the snippet 可以省略的这段匹配片段 */
#define SNIPPET_DESIRED 1   /* We want to include this match in the snippet 我们希望包括这段匹配的片段 */

/*
** Generate the text of a snippet.生成的文本片段。
*/
static void snippetText(
  fulltext_cursor *pCursor,   /* The cursor we need the snippet for 我们需要的片段指针 */
  const char *zStartMark,     /* Markup to appear before each match 标记出现在每个匹配之前 */
  const char *zEndMark,       /* Markup to appear after each match 标记出现在每个匹配之后 */
  const char *zEllipsis       /* Ellipsis mark 省略标记 */
){
  int i, j;
  struct snippetMatch *aMatch;
  int nMatch;
  int nDesired;
  StringBuffer sb;
  int tailCol;
  int tailOffset;
  int iCol;
  int nDoc;
  const char *zDoc;
  int iStart, iEnd;
  int tailEllipsis = 0;
  int iMatch;
  

  free(pCursor->snippet.zSnippet);
  pCursor->snippet.zSnippet = 0;
  aMatch = pCursor->snippet.aMatch;
  nMatch = pCursor->snippet.nMatch;
  initStringBuffer(&sb);

  for(i=0; i<nMatch; i++){
    aMatch[i].snStatus = SNIPPET_IGNORE;
  }
  nDesired = 0;
  for(i=0; i<pCursor->q.nTerms; i++){
    for(j=0; j<nMatch; j++){
      if( aMatch[j].iTerm==i ){
        aMatch[j].snStatus = SNIPPET_DESIRED;
        nDesired++;
        break;
      }
    }
  }

  iMatch = 0;
  tailCol = -1;
  tailOffset = 0;
  for(i=0; i<nMatch && nDesired>0; i++){
    if( aMatch[i].snStatus!=SNIPPET_DESIRED ) continue;
    nDesired--;
    iCol = aMatch[i].iCol;
    zDoc = (const char*)sqlite3_column_text(pCursor->pStmt, iCol+1);
    nDoc = sqlite3_column_bytes(pCursor->pStmt, iCol+1);
    iStart = aMatch[i].iStart - 40;
    iStart = wordBoundary(iStart, zDoc, nDoc, aMatch, nMatch, iCol);
    if( iStart<=10 ){
      iStart = 0;
    }
    if( iCol==tailCol && iStart<=tailOffset+20 ){
      iStart = tailOffset;
    }
    if( (iCol!=tailCol && tailCol>=0) || iStart!=tailOffset ){
      trimWhiteSpace(&sb);
      appendWhiteSpace(&sb);
      append(&sb, zEllipsis);
      appendWhiteSpace(&sb);
    }
    iEnd = aMatch[i].iStart + aMatch[i].nByte + 40;
    iEnd = wordBoundary(iEnd, zDoc, nDoc, aMatch, nMatch, iCol);
    if( iEnd>=nDoc-10 ){
      iEnd = nDoc;
      tailEllipsis = 0;
    }else{
      tailEllipsis = 1;
    }
    while( iMatch<nMatch && aMatch[iMatch].iCol<iCol ){ iMatch++; }
    while( iStart<iEnd ){
      while( iMatch<nMatch && aMatch[iMatch].iStart<iStart
             && aMatch[iMatch].iCol<=iCol ){
        iMatch++;
      }
      if( iMatch<nMatch && aMatch[iMatch].iStart<iEnd
             && aMatch[iMatch].iCol==iCol ){
        nappend(&sb, &zDoc[iStart], aMatch[iMatch].iStart - iStart);
        iStart = aMatch[iMatch].iStart;
        append(&sb, zStartMark);
        nappend(&sb, &zDoc[iStart], aMatch[iMatch].nByte);
        append(&sb, zEndMark);
        iStart += aMatch[iMatch].nByte;
        for(j=iMatch+1; j<nMatch; j++){
          if( aMatch[j].iTerm==aMatch[iMatch].iTerm
              && aMatch[j].snStatus==SNIPPET_DESIRED ){
            nDesired--;
            aMatch[j].snStatus = SNIPPET_IGNORE;
          }
        }
      }else{
        nappend(&sb, &zDoc[iStart], iEnd - iStart);
        iStart = iEnd;
      }
    }
    tailCol = iCol;
    tailOffset = iEnd;
  }
  trimWhiteSpace(&sb);
  if( tailEllipsis ){
    appendWhiteSpace(&sb);
    append(&sb, zEllipsis);
  }
  pCursor->snippet.zSnippet = sb.s;
  pCursor->snippet.nSnippet = sb.len;  
}


/*
** Close the cursor.  For additional information see the documentation
** on the xClose method of the virtual table interface.
** 关闭指针。更多信息请参阅文档xClose虚拟表接口的方法。
*/
static int fulltextClose(sqlite3_vtab_cursor *pCursor){
  fulltext_cursor *c = (fulltext_cursor *) pCursor;
  TRACE(("FTS1 Close %p\n", c));
  sqlite3_finalize(c->pStmt);
  queryClear(&c->q);
  snippetClear(&c->snippet);
  if( c->result.pDoclist!=NULL ){
    docListDelete(c->result.pDoclist);
  }
  free(c);
  return SQLITE_OK;
}

static int fulltextNext(sqlite3_vtab_cursor *pCursor){
  fulltext_cursor *c = (fulltext_cursor *) pCursor;
  sqlite_int64 iDocid;
  int rc;

  TRACE(("FTS1 Next %p\n", pCursor));
  snippetClear(&c->snippet);
  if( c->iCursorType < QUERY_FULLTEXT ){
    /* TODO(shess) Handle SQLITE_SCHEMA AND SQLITE_BUSY.待办事项处理SQLITE_SCHEMA和SQLITE_BUSY。 */
    rc = sqlite3_step(c->pStmt);
    switch( rc ){
      case SQLITE_ROW:
        c->eof = 0;
        return SQLITE_OK;
      case SQLITE_DONE:
        c->eof = 1;
        return SQLITE_OK;
      default:
        c->eof = 1;
        return rc;
    }
  } else {  /* full-text query 全文查询 */
    rc = sqlite3_reset(c->pStmt);
    if( rc!=SQLITE_OK ) return rc;

    iDocid = nextDocid(&c->result);
    if( iDocid==0 ){
      c->eof = 1;
      return SQLITE_OK;
    }
    rc = sqlite3_bind_int64(c->pStmt, 1, iDocid);
    if( rc!=SQLITE_OK ) return rc;
    /* TODO(shess) Handle SQLITE_SCHEMA AND SQLITE_BUSY.待办事项处理SQLITE_SCHEMA和SQLITE_BUSY。 */
    rc = sqlite3_step(c->pStmt);
    if( rc==SQLITE_ROW ){   /* the case we expect 我们期望的情况 */
      c->eof = 0;
      return SQLITE_OK;
    }
    /* an error occurred; abort 发生错误,中止 */
    return rc==SQLITE_DONE ? SQLITE_ERROR : rc;
  }
}


/* Return a DocList corresponding to the query term *pTerm.  If *pTerm
** is the first term of a phrase query, go ahead and evaluate the phrase
** query and return the doclist for the entire phrase query.
** 返回一个对应DocList * pTerm查询术语。如果* pTerm短语查询的首项,继续和评价短语查询并返回doclist整个短语查询。
** The result is stored in pTerm->doclist.结果存储在pTerm - > doclist。
*/
static int docListOfTerm(
  fulltext_vtab *v,     /* The full text index 全文索引 */
  int iColumn,          /* column to restrict to. 列限制。  No restrition if >=nColumn 空，如果> = nColumn restrition */
  QueryTerm *pQTerm,    /* Term we are looking for, or 1st term of a phrase 我们正在寻找的项或者短语的首项 */
  DocList **ppResult    /* Write the result here 结果写在这里 */
){
  DocList *pLeft, *pRight, *pNew;
  int i, rc;

  pLeft = docListNew(DL_POSITIONS);
  rc = term_select_all(v, iColumn, pQTerm->pTerm, pQTerm->nTerm, pLeft);
  if( rc ){
    docListDelete(pLeft);
    return rc;
  }
  for(i=1; i<=pQTerm->nPhrase; i++){
    pRight = docListNew(DL_POSITIONS);
    rc = term_select_all(v, iColumn, pQTerm[i].pTerm, pQTerm[i].nTerm, pRight);
    if( rc ){
      docListDelete(pLeft);
      return rc;
    }
    pNew = docListNew(i<pQTerm->nPhrase ? DL_POSITIONS : DL_DOCIDS);
    docListPhraseMerge(pLeft, pRight, pNew);
    docListDelete(pLeft);
    docListDelete(pRight);
    pLeft = pNew;
  }
  *ppResult = pLeft;
  return SQLITE_OK;
}

/* Add a new term pTerm[0..nTerm-1] to the query *q.添加一个新项pTerm[0 . .nTerm-1)来查询*q。
*/
static void queryAdd(Query *q, const char *pTerm, int nTerm){
  QueryTerm *t;
  ++q->nTerms;
  q->pTerms = realloc(q->pTerms, q->nTerms * sizeof(q->pTerms[0]));
  if( q->pTerms==0 ){
    q->nTerms = 0;
    return;
  }
  t = &q->pTerms[q->nTerms - 1];
  memset(t, 0, sizeof(*t));
  t->pTerm = malloc(nTerm+1);
  memcpy(t->pTerm, pTerm, nTerm);
  t->pTerm[nTerm] = 0;
  t->nTerm = nTerm;
  t->isOr = q->nextIsOr;
  q->nextIsOr = 0;
  t->iColumn = q->nextColumn;
  q->nextColumn = q->dfltColumn;
}

/*
** Check to see if the string zToken[0...nToken-1] matches any
** column name in the virtual table.   If it does,
** return the zero-indexed column number.  If not, return -1.
** 检查字符串是否zToken[0…nToken-1)匹配任何虚拟表的列名。
** 如果不匹配,返回zero-indexed列号。如果没有,返回- 1。
*/
static int checkColumnSpecifier(
  fulltext_vtab *pVtab,    /* The virtual table 虚拟表 */
  const char *zToken,      /* Text of the token token的文本 */
  int nToken               /* Number of characters in the token token的字符数 */
){
  int i;
  for(i=0; i<pVtab->nColumn; i++){
    if( memcmp(pVtab->azColumn[i], zToken, nToken)==0
        && pVtab->azColumn[i][nToken]==0 ){
      return i;
    }
  }
  return -1;
}

/*
** Parse the text at pSegment[0..nSegment-1].  Add additional terms
** to the query being assemblied in pQuery.
** 解析文本pSegment[0 . . nSegment-1]。附加的条款添加到查询在pQuery。
** inPhrase is true if pSegment[0..nSegement-1] is contained within
** double-quotes.  If inPhrase is true, then the first term
** is marked with the number of terms in the phrase less one and
** OR and "-" syntax is ignored.  If inPhrase is false, then every
** term found is marked with nPhrase=0 and OR and "-" syntax is significant.
** 如果pSegment[0 . .nSegement-1]是包含在双引号中inPhrase是真的。
** 如果inPhrase是真的,那么被标记的首项的条款的数量短语少于一,“-”语法被忽略。
** 如果inPhrase是假的,那么每项发现标有nPhrase = 0,或者和“-”语法意义重大。
*/
static int tokenizeSegment(
  sqlite3_tokenizer *pTokenizer,          /* The tokenizer to use 记号赋予器使用 */
  const char *pSegment, int nSegment,     /* Query expression being parsed 查询表达式被解析 */
  int inPhrase,                           /* True if within "..." 真如果含有“……” */
  Query *pQuery                           /* Append results here 在这里添加结果 */
){
  const sqlite3_tokenizer_module *pModule = pTokenizer->pModule;
  sqlite3_tokenizer_cursor *pCursor;
  int firstIndex = pQuery->nTerms;
  int iCol;
  int nTerm = 1;
  
  int rc = pModule->xOpen(pTokenizer, pSegment, nSegment, &pCursor);
  if( rc!=SQLITE_OK ) return rc;
  pCursor->pTokenizer = pTokenizer;

  while( 1 ){
    const char *pToken;
    int nToken, iBegin, iEnd, iPos;

    rc = pModule->xNext(pCursor,
                        &pToken, &nToken,
                        &iBegin, &iEnd, &iPos);
    if( rc!=SQLITE_OK ) break;
    if( !inPhrase &&
        pSegment[iEnd]==':' &&
         (iCol = checkColumnSpecifier(pQuery->pFts, pToken, nToken))>=0 ){
      pQuery->nextColumn = iCol;
      continue;
    }
    if( !inPhrase && pQuery->nTerms>0 && nToken==2
         && pSegment[iBegin]=='O' && pSegment[iBegin+1]=='R' ){
      pQuery->nextIsOr = 1;
      continue;
    }
    queryAdd(pQuery, pToken, nToken);
    if( !inPhrase && iBegin>0 && pSegment[iBegin-1]=='-' ){
      pQuery->pTerms[pQuery->nTerms-1].isNot = 1;
    }
    pQuery->pTerms[pQuery->nTerms-1].iPhrase = nTerm;
    if( inPhrase ){
      nTerm++;
    }
  }

  if( inPhrase && pQuery->nTerms>firstIndex ){
    pQuery->pTerms[firstIndex].nPhrase = pQuery->nTerms - firstIndex - 1;
  }

  return pModule->xClose(pCursor);
}

/* Parse a query string, yielding a Query object pQuery.
** 解析查询字符串,出产查询对象pQuery。
** The calling function will need to queryClear() to clean up
** the dynamically allocated memory held by pQuery.
** 调用函数需要queryClear()来清理pQuery持有的动态分配的内存。
*/
static int parseQuery(
  fulltext_vtab *v,        /* The fulltext index 全文索引 */
  const char *zInput,      /* Input text of the query string 查询字符串的输入文本 */
  int nInput,              /* Size of the input text 输入文本的大小 */
  int dfltColumn,          /* Default column of the index to match against 默认索引匹配的列 */
  Query *pQuery            /* Write the parse results here.在这里写解析结果 */
){
  int iInput, inPhrase = 0;

  if( zInput==0 ) nInput = 0;
  if( nInput<0 ) nInput = strlen(zInput);
  pQuery->nTerms = 0;
  pQuery->pTerms = NULL;
  pQuery->nextIsOr = 0;
  pQuery->nextColumn = dfltColumn;
  pQuery->dfltColumn = dfltColumn;
  pQuery->pFts = v;

  for(iInput=0; iInput<nInput; ++iInput){
    int i;
    for(i=iInput; i<nInput && zInput[i]!='"'; ++i){}
    if( i>iInput ){
      tokenizeSegment(v->pTokenizer, zInput+iInput, i-iInput, inPhrase,
                       pQuery);
    }
    iInput = i;
    if( i<nInput ){
      assert( zInput[i]=='"' );
      inPhrase = !inPhrase;
    }
  }

  if( inPhrase ){
    /* unmatched quote */
    queryClear(pQuery);
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

/* Perform a full-text query using the search expression in
** zInput[0..nInput-1].  Return a list of matching documents
** in pResult.
** 执行一个全文查询使用的搜索表达式zInput[0 . . nInput-1]。
** 在pResult返回一个匹配的文档列表。
** Queries must match column iColumn.  Or if iColumn>=nColumn
** they are allowed to match against any column.
** 查询必须匹配列iColumn。或者如果iColumn > = nColumn他们可以匹配任何列。
*/
static int fulltextQuery(
  fulltext_vtab *v,      /* The full text index 全文索引 */
  int iColumn,           /* Match against this column by default 默认匹配这列 */
  const char *zInput,    /* The query string 查询字符串 */
  int nInput,            /* Number of bytes in zInput[] 在zInput[]中的字节数 */
  DocList **pResult,     /* Write the result doclist here 在这里写doclist结果 */
  Query *pQuery          /* Put parsed query string here 在这里放解析查询字符串 */
){
  int i, iNext, rc;
  DocList *pLeft = NULL;
  DocList *pRight, *pNew, *pOr;
  int nNot = 0;
  QueryTerm *aTerm;

  rc = parseQuery(v, zInput, nInput, iColumn, pQuery);
  if( rc!=SQLITE_OK ) return rc;

  /* Merge AND terms.合并和术语。 */
  aTerm = pQuery->pTerms;
  for(i = 0; i<pQuery->nTerms; i=iNext){
    if( aTerm[i].isNot ){
      /* Handle all NOT terms in a separate pass 处理所有条款在一个单独的传递 */
      nNot++;
      iNext = i + aTerm[i].nPhrase+1;
      continue;
    }
    iNext = i + aTerm[i].nPhrase + 1;
    rc = docListOfTerm(v, aTerm[i].iColumn, &aTerm[i], &pRight);
    if( rc ){
      queryClear(pQuery);
      return rc;
    }
    while( iNext<pQuery->nTerms && aTerm[iNext].isOr ){
      rc = docListOfTerm(v, aTerm[iNext].iColumn, &aTerm[iNext], &pOr);
      iNext += aTerm[iNext].nPhrase + 1;
      if( rc ){
        queryClear(pQuery);
        return rc;
      }
      pNew = docListNew(DL_DOCIDS);
      docListOrMerge(pRight, pOr, pNew);
      docListDelete(pRight);
      docListDelete(pOr);
      pRight = pNew;
    }
    if( pLeft==0 ){
      pLeft = pRight;
    }else{
      pNew = docListNew(DL_DOCIDS);
      docListAndMerge(pLeft, pRight, pNew);
      docListDelete(pRight);
      docListDelete(pLeft);
      pLeft = pNew;
    }
  }

  if( nNot && pLeft==0 ){
    /* We do not yet know how to handle a query of only NOT terms 我们还不知道如何处理一个查询的条款 */
    return SQLITE_ERROR;
  }

  /* Do the EXCEPT terms 做除外条款 */
  for(i=0; i<pQuery->nTerms;  i += aTerm[i].nPhrase + 1){
    if( !aTerm[i].isNot ) continue;
    rc = docListOfTerm(v, aTerm[i].iColumn, &aTerm[i], &pRight);
    if( rc ){
      queryClear(pQuery);
      docListDelete(pLeft);
      return rc;
    }
    pNew = docListNew(DL_DOCIDS);
    docListExceptMerge(pLeft, pRight, pNew);
    docListDelete(pRight);
    docListDelete(pLeft);
    pLeft = pNew;
  }

  *pResult = pLeft;
  return rc;
}

/*
** This is the xFilter interface for the virtual table.  See
** the virtual table xFilter method documentation for additional
** information.
** 这是xFilter虚拟表接口。看到虚拟表xFilter方法文档的附加信息。
** If idxNum==QUERY_GENERIC then do a full table scan against
** the %_content table.
** 如果idxNum = = QUERY_GENERIC那么对% _content表做一个全表扫描。
** If idxNum==QUERY_ROWID then do a rowid lookup for a single entry
** in the %_content table.
** 如果idxNum = = QUERY_ROWID那么做rowid % _content表中查找一个条目。
** If idxNum>=QUERY_FULLTEXT then use the full text index.  The
** column on the left-hand side of the MATCH operator is column
** number idxNum-QUERY_FULLTEXT, 0 indexed.  argv[0] is the right-hand
** side of the MATCH operator.
** 如果idxNum > = QUERY_FULLTEXT然后使用全文索引。
** 列匹配操作符的左边列号idxNum-QUERY_FULLTEXT,0索引。
** argv[0]是匹配操作符的右边。
*/
/* TODO(shess) Upgrade the cursor initialization and destruction to
** account for fulltextFilter() being called multiple times on the
** same cursor.  The current solution is very fragile.  Apply fix to
** fts2 as appropriate.
** 待办事项升级指针的初始化和销毁fulltextFilter占()被调用多次在同一指针。
** 当前的解决方案是非常脆弱的。修复适用于fts2。
*/
static int fulltextFilter(
  sqlite3_vtab_cursor *pCursor,     /* The cursor used for this query 指针用于该查询 */
  int idxNum, const char *idxStr,   /* Which indexing scheme to use 使用的索引方案 */
  int argc, sqlite3_value **argv    /* Arguments for the indexing scheme 参数的索引方案 */
){
  fulltext_cursor *c = (fulltext_cursor *) pCursor;
  fulltext_vtab *v = cursor_vtab(c);
  int rc;
  char *zSql;

  TRACE(("FTS1 Filter %p\n",pCursor));

  zSql = sqlite3_mprintf("select rowid, * from %%_content %s",
                          idxNum==QUERY_GENERIC ? "" : "where rowid=?");
  sqlite3_finalize(c->pStmt);
  rc = sql_prepare(v->db, v->zDb, v->zName, &c->pStmt, zSql);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ) return rc;

  c->iCursorType = idxNum;
  switch( idxNum ){
    case QUERY_GENERIC:
      break;

    case QUERY_ROWID:
      rc = sqlite3_bind_int64(c->pStmt, 1, sqlite3_value_int64(argv[0]));
      if( rc!=SQLITE_OK ) return rc;
      break;

    default:   /* full-text search 全文搜索 */
    {
      const char *zQuery = (const char *)sqlite3_value_text(argv[0]);
      DocList *pResult;
      assert( idxNum<=QUERY_FULLTEXT+v->nColumn);
      assert( argc==1 );
      queryClear(&c->q);
      rc = fulltextQuery(v, idxNum-QUERY_FULLTEXT, zQuery, -1, &pResult, &c->q);
      if( rc!=SQLITE_OK ) return rc;
      if( c->result.pDoclist!=NULL ) docListDelete(c->result.pDoclist);
      readerInit(&c->result, pResult);
      break;
    }
  }

  return fulltextNext(pCursor);
}

/* This is the xEof method of the virtual table.  The SQLite core
** calls this routine to find out if it has reached the end of
** a query's results set.
** 这是xEof虚拟表的方法。SQLite核心调用这个例程来找出是否已经达成一个查询的结果集。
*/
static int fulltextEof(sqlite3_vtab_cursor *pCursor){
  fulltext_cursor *c = (fulltext_cursor *) pCursor;
  return c->eof;
}

/* This is the xColumn method of the virtual table.  The SQLite
** core calls this method during a query when it needs the value
** of a column from the virtual table.  This method needs to use
** one of the sqlite3_result_*() routines to store the requested
** value back in the pContext.
** 这是xColumn虚拟表的方法。SQLite核心调用这个方法在查询时需要从虚拟表列的值。
** 这种方法需要使用一个sqlite3_result_ *()例程回到pContext存储请求的值。
*/
static int fulltextColumn(sqlite3_vtab_cursor *pCursor,
                          sqlite3_context *pContext, int idxCol){
  fulltext_cursor *c = (fulltext_cursor *) pCursor;
  fulltext_vtab *v = cursor_vtab(c);

  if( idxCol<v->nColumn ){
    sqlite3_value *pVal = sqlite3_column_value(c->pStmt, idxCol+1);
    sqlite3_result_value(pContext, pVal);
  }else if( idxCol==v->nColumn ){
    /* The extra column whose name is the same as the table.额外的列的名称是一样的。
    ** Return a blob which is a pointer to the cursor 返回一个blob指针
    */
    sqlite3_result_blob(pContext, &c, sizeof(c), SQLITE_TRANSIENT);
  }
  return SQLITE_OK;
}

/* This is the xRowid method.  The SQLite core calls this routine to
** retrive the rowid for the current row of the result set.  The
** rowid should be written to *pRowid.
** 这是xRowid方法。SQLite核心调用这个例程能够利用rowid的当前行结果集。rowid应被写入* pRowid 。
*/
static int fulltextRowid(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid){
  fulltext_cursor *c = (fulltext_cursor *) pCursor;

  *pRowid = sqlite3_column_int64(c->pStmt, 0);
  return SQLITE_OK;
}

/* Add all terms in [zText] to the given hash table.  If [iColumn] > 0,
 * we also store positions and offsets in the hash table using the given
 * column number.添加所有条款(zText)给定的哈希表。如果(iColumn)> 0,我们还在哈希表中存储位置和补偿使用给定的列号。 */
static int buildTerms(fulltext_vtab *v, fts1Hash *terms, sqlite_int64 iDocid,
                      const char *zText, int iColumn){
  sqlite3_tokenizer *pTokenizer = v->pTokenizer;
  sqlite3_tokenizer_cursor *pCursor;
  const char *pToken;
  int nTokenBytes;
  int iStartOffset, iEndOffset, iPosition;
  int rc;

  rc = pTokenizer->pModule->xOpen(pTokenizer, zText, -1, &pCursor);
  if( rc!=SQLITE_OK ) return rc;

  pCursor->pTokenizer = pTokenizer;
  while( SQLITE_OK==pTokenizer->pModule->xNext(pCursor,
                                               &pToken, &nTokenBytes,
                                               &iStartOffset, &iEndOffset,
                                               &iPosition) ){
    DocList *p;

    /* Positions can't be negative; we use -1 as a terminator internally.定位不能是负数,我们在内部使用-1作为一个终结者。 */
    if( iPosition<0 ){
      pTokenizer->pModule->xClose(pCursor);
      return SQLITE_ERROR;
    }

    p = fts1HashFind(terms, pToken, nTokenBytes);
    if( p==NULL ){
      p = docListNew(DL_DEFAULT);
      docListAddDocid(p, iDocid);
      fts1HashInsert(terms, pToken, nTokenBytes, p);
    }
    if( iColumn>=0 ){
      docListAddPosOffset(p, iColumn, iPosition, iStartOffset, iEndOffset);
    }
  }

  /* TODO(shess) Check return?待办事项检查返回?  Should this be able to cause errors at
  ** this point? 这应该能够导致错误在这一点上吗? Actually, same question about sqlite3_finalize(),
  ** though one could argue that failure there means that the data is
  ** not durable.事实上,同样的问题关于sqlite3_finalize(),虽然有人会说,失败意味着数据不会持久  *ponder*
  */
  pTokenizer->pModule->xClose(pCursor);
  return rc;
}

/* Update the %_terms table to map the term [pTerm] to the given rowid.更新% _terms表(pTerm)这一术语映射到给定的rowid。 */
static int index_insert_term(fulltext_vtab *v, const char *pTerm, int nTerm,
                             DocList *d){
  sqlite_int64 iIndexRow;
  DocList doclist;
  int iSegment = 0, rc;

  rc = term_select(v, pTerm, nTerm, iSegment, &iIndexRow, &doclist);
  if( rc==SQLITE_DONE ){
    docListInit(&doclist, DL_DEFAULT, 0, 0);
    docListUpdate(&doclist, d);
    /* TODO(shess) Consider length(doclist)>CHUNK_MAX?待办事项考虑长度(doclist)> CHUNK_MAX吗? */
    rc = term_insert(v, NULL, pTerm, nTerm, iSegment, &doclist);
    goto err;
  }
  if( rc!=SQLITE_ROW ) return SQLITE_ERROR;

  docListUpdate(&doclist, d);
  if( doclist.nData<=CHUNK_MAX ){
    rc = term_update(v, iIndexRow, &doclist);
    goto err;
  }

  /* Doclist doesn't fit, delete what's there, and accumulate
  ** forward. Doclist不适合,删除。
  */
  rc = term_delete(v, iIndexRow);
  if( rc!=SQLITE_OK ) goto err;

  /* Try to insert the doclist into a higher segment bucket.  On
  ** failure, accumulate existing doclist with the doclist from that
  ** bucket, and put results in the next bucket.
  ** 试图插入doclist更高部分桶排序。在失败中失败,积累现有doclist doclist从那部分,并把结果在下一桶排序。
  */
  iSegment++;
  while( (rc=term_insert(v, &iIndexRow, pTerm, nTerm, iSegment,
                         &doclist))!=SQLITE_OK ){
    sqlite_int64 iSegmentRow;
    DocList old;
    int rc2;

    /* Retain old error in case the term_insert() error was really an
    ** error rather than a bounced insert.保留旧的错误,以防term_insert()错误真的是一个错误,而不是一个被插入。
    */
    rc2 = term_select(v, pTerm, nTerm, iSegment, &iSegmentRow, &old);
    if( rc2!=SQLITE_ROW ) goto err;

    rc = term_delete(v, iSegmentRow);
    if( rc!=SQLITE_OK ) goto err;

    /* Reusing lowest-number deleted row keeps the index smaller.重用最低数删除行保持指数较小。 */
    if( iSegmentRow<iIndexRow ) iIndexRow = iSegmentRow;

    /* doclist contains the newer data, so accumulate it over old.doclist包含更新的数据,所以积累了历史。
    ** Then steal accumulated data for doclist.然后窃取doclist积累数据。
    */
    docListAccumulate(&old, &doclist);
    docListDestroy(&doclist);
    doclist = old;

    iSegment++;
  }

 err:
  docListDestroy(&doclist);
  return rc;
}

/* Add doclists for all terms in [pValues] to the hash table [terms].为所有条款添加doclists[pValues]的哈希表。 */
static int insertTerms(fulltext_vtab *v, fts1Hash *terms, sqlite_int64 iRowid,
                sqlite3_value **pValues){
  int i;
  for(i = 0; i < v->nColumn ; ++i){
    char *zText = (char*)sqlite3_value_text(pValues[i]);
    int rc = buildTerms(v, terms, iRowid, zText, i);
    if( rc!=SQLITE_OK ) return rc;
  }
  return SQLITE_OK;
}

/* Add empty doclists for all terms in the given row's content to the hash
 * table [pTerms].添加空doclists给定行中的所有条款的内容哈希表。 */
static int deleteTerms(fulltext_vtab *v, fts1Hash *pTerms, sqlite_int64 iRowid){
  const char **pValues;
  int i;

  int rc = content_select(v, iRowid, &pValues);
  if( rc!=SQLITE_OK ) return rc;

  for(i = 0 ; i < v->nColumn; ++i) {
    rc = buildTerms(v, pTerms, iRowid, pValues[i], -1);
    if( rc!=SQLITE_OK ) break;
  }

  freeStringArray(v->nColumn, pValues);
  return SQLITE_OK;
}

/* Insert a row into the %_content table; set *piRowid to be the ID of the
 * new row.  Fill [pTerms] with new doclists for the %_term table.
 * 将一行插入到% _content表;集* piRowid新行的ID。(pTerms)填入新doclists % _term表。
 */
static int index_insert(fulltext_vtab *v, sqlite3_value *pRequestRowid,
                        sqlite3_value **pValues,
                        sqlite_int64 *piRowid, fts1Hash *pTerms){
  int rc;

  rc = content_insert(v, pRequestRowid, pValues);  /* execute an SQL INSERT 执行一条SQL插入 */
  if( rc!=SQLITE_OK ) return rc;
  *piRowid = sqlite3_last_insert_rowid(v->db);
  return insertTerms(v, pTerms, *piRowid, pValues);
}

/* Delete a row from the %_content table; fill [pTerms] with empty doclists
 * to be written to the %_term table.删除一行从% _content表;(pTerms)填入空doclists写入% _term表。 */
static int index_delete(fulltext_vtab *v, sqlite_int64 iRow, fts1Hash *pTerms){
  int rc = deleteTerms(v, pTerms, iRow);
  if( rc!=SQLITE_OK ) return rc;
  return content_delete(v, iRow);  /* execute an SQL DELETE 执行一条SQL删除 */
}

/* Update a row in the %_content table; fill [pTerms] with new doclists for the
 * %_term table.更新表中的一行% _content;[pTerms]充满新doclists % _term表。 */
static int index_update(fulltext_vtab *v, sqlite_int64 iRow,
                        sqlite3_value **pValues, fts1Hash *pTerms){
  /* Generate an empty doclist for each term that previously appeared in this
   * row.生成一个空doclist为每个术语,之前出现在这一行。 */
  int rc = deleteTerms(v, pTerms, iRow);
  if( rc!=SQLITE_OK ) return rc;

  rc = content_update(v, pValues, iRow);  /* execute an SQL UPDATE 执行一条SQL更新 */
  if( rc!=SQLITE_OK ) return rc;

  /* Now add positions for terms which appear in the updated row.现在添加位置的术语出现在更新的行。 */
  return insertTerms(v, pTerms, iRow, pValues);
}

/* This function implements the xUpdate callback; it is the top-level entry
 * point for inserting, deleting or updating a row in a full-text table.
 * 这个函数实现了xUpdate回调;它是顶级入口点插入、删除或更新全文表中的一行。
 */
static int fulltextUpdate(sqlite3_vtab *pVtab, int nArg, sqlite3_value **ppArg,
                   sqlite_int64 *pRowid){
  fulltext_vtab *v = (fulltext_vtab *) pVtab;
  fts1Hash terms;   /* maps term string -> PosList 映射 string -> PstList */
  int rc;
  fts1HashElem *e;

  TRACE(("FTS1 Update %p\n", pVtab));
  
  fts1HashInit(&terms, FTS1_HASH_STRING, 1);

  if( nArg<2 ){
    rc = index_delete(v, sqlite3_value_int64(ppArg[0]), &terms);
  } else if( sqlite3_value_type(ppArg[0]) != SQLITE_NULL ){
    /* An update: 更新:
     * ppArg[0] = old rowid
     * ppArg[1] = new rowid
     * ppArg[2..2+v->nColumn-1] = values
     * ppArg[2+v->nColumn] = value for magic column (we ignore this)
     */
    sqlite_int64 rowid = sqlite3_value_int64(ppArg[0]);
    if( sqlite3_value_type(ppArg[1]) != SQLITE_INTEGER ||
      sqlite3_value_int64(ppArg[1]) != rowid ){
      rc = SQLITE_ERROR;  /* we don't allow changing the rowid */
    } else {
      assert( nArg==2+v->nColumn+1);
      rc = index_update(v, rowid, &ppArg[2], &terms);
    }
  } else {
    /* An insert:
     * ppArg[1] = requested rowid
     * ppArg[2..2+v->nColumn-1] = values
     * ppArg[2+v->nColumn] = value for magic column (we ignore this)
     */
    assert( nArg==2+v->nColumn+1);
    rc = index_insert(v, ppArg[1], &ppArg[2], pRowid, &terms);
  }

  if( rc==SQLITE_OK ){
    /* Write updated doclists to disk.更新doclists写入磁盘。 */
    for(e=fts1HashFirst(&terms); e; e=fts1HashNext(e)){
      DocList *p = fts1HashData(e);
      rc = index_insert_term(v, fts1HashKey(e), fts1HashKeysize(e), p);
      if( rc!=SQLITE_OK ) break;
    }
  }

  /* clean up 清理 */
  for(e=fts1HashFirst(&terms); e; e=fts1HashNext(e)){
    DocList *p = fts1HashData(e);
    docListDelete(p);
  }
  fts1HashClear(&terms);

  return rc;
}

/*
** Implementation of the snippet() function for FTS1 FTS1片段()函数的实现
*/
static void snippetFunc(
  sqlite3_context *pContext,
  int argc,
  sqlite3_value **argv
){
  fulltext_cursor *pCursor;
  if( argc<1 ) return;
  if( sqlite3_value_type(argv[0])!=SQLITE_BLOB ||
      sqlite3_value_bytes(argv[0])!=sizeof(pCursor) ){
    sqlite3_result_error(pContext, "illegal first argument to html_snippet",-1);
  }else{
    const char *zStart = "<b>";
    const char *zEnd = "</b>";
    const char *zEllipsis = "<b>...</b>";
    memcpy(&pCursor, sqlite3_value_blob(argv[0]), sizeof(pCursor));
    if( argc>=2 ){
      zStart = (const char*)sqlite3_value_text(argv[1]);
      if( argc>=3 ){
        zEnd = (const char*)sqlite3_value_text(argv[2]);
        if( argc>=4 ){
          zEllipsis = (const char*)sqlite3_value_text(argv[3]);
        }
      }
    }
    snippetAllOffsets(pCursor);
    snippetText(pCursor, zStart, zEnd, zEllipsis);
    sqlite3_result_text(pContext, pCursor->snippet.zSnippet,
                        pCursor->snippet.nSnippet, SQLITE_STATIC);
  }
}

/*
** Implementation of the offsets() function for FTS1 FTS1偏移量()函数的实现
*/
static void snippetOffsetsFunc(
  sqlite3_context *pContext,
  int argc,
  sqlite3_value **argv
){
  fulltext_cursor *pCursor;
  if( argc<1 ) return;
  if( sqlite3_value_type(argv[0])!=SQLITE_BLOB ||
      sqlite3_value_bytes(argv[0])!=sizeof(pCursor) ){
    sqlite3_result_error(pContext, "illegal first argument to offsets",-1);
  }else{
    memcpy(&pCursor, sqlite3_value_blob(argv[0]), sizeof(pCursor));
    snippetAllOffsets(pCursor);
    snippetOffsetText(&pCursor->snippet);
    sqlite3_result_text(pContext,
                        pCursor->snippet.zOffset, pCursor->snippet.nOffset,
                        SQLITE_STATIC);
  }
}

/*
** This routine implements the xFindFunction method for the FTS1
** virtual table.这个例程实现xFindFunction FTS1虚拟方法表。
*/
static int fulltextFindFunction(
  sqlite3_vtab *pVtab,
  int nArg,
  const char *zName,
  void (**pxFunc)(sqlite3_context*,int,sqlite3_value**),
  void **ppArg
){
  if( strcmp(zName,"snippet")==0 ){
    *pxFunc = snippetFunc;
    return 1;
  }else if( strcmp(zName,"offsets")==0 ){
    *pxFunc = snippetOffsetsFunc;
    return 1;
  }
  return 0;
}

/*
** Rename an fts1 table.重命名一个fts1表。
*/
static int fulltextRename(
  sqlite3_vtab *pVtab,
  const char *zName
){
  fulltext_vtab *p = (fulltext_vtab *)pVtab;
  int rc = SQLITE_NOMEM;
  char *zSql = sqlite3_mprintf(
    "ALTER TABLE %Q.'%q_content'  RENAME TO '%q_content';"
    "ALTER TABLE %Q.'%q_term' RENAME TO '%q_term';"
    , p->zDb, p->zName, zName
    , p->zDb, p->zName, zName
  );
  if( zSql ){
    rc = sqlite3_exec(p->db, zSql, 0, 0, 0);
    sqlite3_free(zSql);
  }
  return rc;
}

static const sqlite3_module fulltextModule = {
  /* iVersion      */ 0,
  /* xCreate       */ fulltextCreate,
  /* xConnect      */ fulltextConnect,
  /* xBestIndex    */ fulltextBestIndex,
  /* xDisconnect   */ fulltextDisconnect,
  /* xDestroy      */ fulltextDestroy,
  /* xOpen         */ fulltextOpen,
  /* xClose        */ fulltextClose,
  /* xFilter       */ fulltextFilter,
  /* xNext         */ fulltextNext,
  /* xEof          */ fulltextEof,
  /* xColumn       */ fulltextColumn,
  /* xRowid        */ fulltextRowid,
  /* xUpdate       */ fulltextUpdate,
  /* xBegin        */ 0, 
  /* xSync         */ 0,
  /* xCommit       */ 0,
  /* xRollback     */ 0,
  /* xFindFunction */ fulltextFindFunction,
  /* xRename       */ fulltextRename,
};

int sqlite3Fts1Init(sqlite3 *db){
  sqlite3_overload_function(db, "snippet", -1);
  sqlite3_overload_function(db, "offsets", -1);
  return sqlite3_create_module(db, "fts1", &fulltextModule, 0);
}

#if !SQLITE_CORE
int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg,
                           const sqlite3_api_routines *pApi){
  SQLITE_EXTENSION_INIT2(pApi)
  return sqlite3Fts1Init(db);
}
#endif

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS1) */

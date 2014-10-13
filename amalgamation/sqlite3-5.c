/************** Begin file notify.c ******************************************/
/*
** 2009 March 3
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains the implementation of the sqlite3_unlock_notify()
** API method and its associated functionality.
*/

/* Omit this entire file if SQLITE_ENABLE_UNLOCK_NOTIFY is not defined. */
#ifdef SQLITE_ENABLE_UNLOCK_NOTIFY

/*
** Public interfaces:
**
**   sqlite3ConnectionBlocked()
**   sqlite3ConnectionUnlocked()
**   sqlite3ConnectionClosed()
**   sqlite3_unlock_notify()
*/

#define assertMutexHeld() \
  assert( sqlite3_mutex_held(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER)) )

/*
** Head of a linked list of all sqlite3 objects created by this process
** for which either sqlite3.pBlockingConnection or sqlite3.pUnlockConnection
** is not NULL. This variable may only accessed while the STATIC_MASTER
** mutex is held.
*/
static sqlite3 *SQLITE_WSD sqlite3BlockedList = 0;

#ifndef NDEBUG
/*
** This function is a complex assert() that verifies the following 
** properties of the blocked connections list:
**
**   1) Each entry in the list has a non-NULL value for either 
**      pUnlockConnection or pBlockingConnection, or both.
**
**   2) All entries in the list that share a common value for 
**      xUnlockNotify are grouped together.
**
**   3) If the argument db is not NULL, then none of the entries in the
**      blocked connections list have pUnlockConnection or pBlockingConnection
**      set to db. This is used when closing connection db.
*/
static void checkListProperties(sqlite3 *db){
  sqlite3 *p;
  for(p=sqlite3BlockedList; p; p=p->pNextBlocked){
    int seen = 0;
    sqlite3 *p2;

    /* Verify property (1) */
    assert( p->pUnlockConnection || p->pBlockingConnection );

    /* Verify property (2) */
    for(p2=sqlite3BlockedList; p2!=p; p2=p2->pNextBlocked){
      if( p2->xUnlockNotify==p->xUnlockNotify ) seen = 1;
      assert( p2->xUnlockNotify==p->xUnlockNotify || !seen );
      assert( db==0 || p->pUnlockConnection!=db );
      assert( db==0 || p->pBlockingConnection!=db );
    }
  }
}
#else
# define checkListProperties(x)
#endif

/*
** Remove connection db from the blocked connections list. If connection
** db is not currently a part of the list, this function is a no-op.
*/
static void removeFromBlockedList(sqlite3 *db){
  sqlite3 **pp;
  assertMutexHeld();
  for(pp=&sqlite3BlockedList; *pp; pp = &(*pp)->pNextBlocked){
    if( *pp==db ){
      *pp = (*pp)->pNextBlocked;
      break;
    }
  }
}

/*
** Add connection db to the blocked connections list. It is assumed
** that it is not already a part of the list.
*/
static void addToBlockedList(sqlite3 *db){
  sqlite3 **pp;
  assertMutexHeld();
  for(
    pp=&sqlite3BlockedList; 
    *pp && (*pp)->xUnlockNotify!=db->xUnlockNotify; 
    pp=&(*pp)->pNextBlocked
  );
  db->pNextBlocked = *pp;
  *pp = db;
}

/*
** Obtain the STATIC_MASTER mutex.
*/
static void enterMutex(void){
  sqlite3_mutex_enter(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER));
  checkListProperties(0);
}

/*
** Release the STATIC_MASTER mutex.
*/
static void leaveMutex(void){
  assertMutexHeld();
  checkListProperties(0);
  sqlite3_mutex_leave(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER));
}

/*
** Register an unlock-notify callback.
**
** This is called after connection "db" has attempted some operation
** but has received an SQLITE_LOCKED error because another connection
** (call it pOther) in the same process was busy using the same shared
** cache.  pOther is found by looking at db->pBlockingConnection.
**
** If there is no blocking connection, the callback is invoked immediately,
** before this routine returns.
**
** If pOther is already blocked on db, then report SQLITE_LOCKED, to indicate
** a deadlock.
**
** Otherwise, make arrangements to invoke xNotify when pOther drops
** its locks.
**
** Each call to this routine overrides any prior callbacks registered
** on the same "db".  If xNotify==0 then any prior callbacks are immediately
** cancelled.
*/
SQLITE_API int sqlite3_unlock_notify(
  sqlite3 *db,
  void (*xNotify)(void **, int),
  void *pArg
){
  int rc = SQLITE_OK;

  sqlite3_mutex_enter(db->mutex);
  enterMutex();

  if( xNotify==0 ){
    removeFromBlockedList(db);
    db->pBlockingConnection = 0;
    db->pUnlockConnection = 0;
    db->xUnlockNotify = 0;
    db->pUnlockArg = 0;
  }else if( 0==db->pBlockingConnection ){
    /* The blocking transaction has been concluded. Or there never was a 
    ** blocking transaction. In either case, invoke the notify callback
    ** immediately. 
    */
    xNotify(&pArg, 1);
  }else{
    sqlite3 *p;

    for(p=db->pBlockingConnection; p && p!=db; p=p->pUnlockConnection){}
    if( p ){
      rc = SQLITE_LOCKED;              /* Deadlock detected. */
    }else{
      db->pUnlockConnection = db->pBlockingConnection;
      db->xUnlockNotify = xNotify;
      db->pUnlockArg = pArg;
      removeFromBlockedList(db);
      addToBlockedList(db);
    }
  }

  leaveMutex();
  assert( !db->mallocFailed );
  sqlite3ErrorWithMsg(db, rc, (rc?"database is deadlocked":0));
  sqlite3_mutex_leave(db->mutex);
  return rc;
}

/*
** This function is called while stepping or preparing a statement 
** associated with connection db. The operation will return SQLITE_LOCKED
** to the user because it requires a lock that will not be available
** until connection pBlocker concludes its current transaction.
*/
SQLITE_PRIVATE void sqlite3ConnectionBlocked(sqlite3 *db, sqlite3 *pBlocker){
  enterMutex();
  if( db->pBlockingConnection==0 && db->pUnlockConnection==0 ){
    addToBlockedList(db);
  }
  db->pBlockingConnection = pBlocker;
  leaveMutex();
}

/*
** This function is called when
** the transaction opened by database db has just finished. Locks held 
** by database connection db have been released.
**
** This function loops through each entry in the blocked connections
** list and does the following:
**
**   1) If the sqlite3.pBlockingConnection member of a list entry is
**      set to db, then set pBlockingConnection=0.
**
**   2) If the sqlite3.pUnlockConnection member of a list entry is
**      set to db, then invoke the configured unlock-notify callback and
**      set pUnlockConnection=0.
**
**   3) If the two steps above mean that pBlockingConnection==0 and
**      pUnlockConnection==0, remove the entry from the blocked connections
**      list.
*/
SQLITE_PRIVATE void sqlite3ConnectionUnlocked(sqlite3 *db){
  void (*xUnlockNotify)(void **, int) = 0; /* Unlock-notify cb to invoke */
  int nArg = 0;                            /* Number of entries in aArg[] */
  sqlite3 **pp;                            /* Iterator variable */
  void **aArg;               /* Arguments to the unlock callback */
  void **aDyn = 0;           /* Dynamically allocated space for aArg[] */
  void *aStatic[16];         /* Starter space for aArg[].  No malloc required */

  aArg = aStatic;
  enterMutex();         /* Enter STATIC_MASTER mutex */

  /* This loop runs once for each entry in the blocked-connections list. */
  for(pp=&sqlite3BlockedList; *pp; /* no-op */ ){
    sqlite3 *p = *pp;

    /* Step 1. */
    if( p->pBlockingConnection==db ){
      p->pBlockingConnection = 0;
    }

    /* Step 2. */
    if( p->pUnlockConnection==db ){
      assert( p->xUnlockNotify );
      if( p->xUnlockNotify!=xUnlockNotify && nArg!=0 ){
        xUnlockNotify(aArg, nArg);
        nArg = 0;
      }

      sqlite3BeginBenignMalloc();
      assert( aArg==aDyn || (aDyn==0 && aArg==aStatic) );
      assert( nArg<=(int)ArraySize(aStatic) || aArg==aDyn );
      if( (!aDyn && nArg==(int)ArraySize(aStatic))
       || (aDyn && nArg==(int)(sqlite3MallocSize(aDyn)/sizeof(void*)))
      ){
        /* The aArg[] array needs to grow. */
        void **pNew = (void **)sqlite3Malloc(nArg*sizeof(void *)*2);
        if( pNew ){
          memcpy(pNew, aArg, nArg*sizeof(void *));
          sqlite3_free(aDyn);
          aDyn = aArg = pNew;
        }else{
          /* This occurs when the array of context pointers that need to
          ** be passed to the unlock-notify callback is larger than the
          ** aStatic[] array allocated on the stack and the attempt to 
          ** allocate a larger array from the heap has failed.
          **
          ** This is a difficult situation to handle. Returning an error
          ** code to the caller is insufficient, as even if an error code
          ** is returned the transaction on connection db will still be
          ** closed and the unlock-notify callbacks on blocked connections
          ** will go unissued. This might cause the application to wait
          ** indefinitely for an unlock-notify callback that will never 
          ** arrive.
          **
          ** Instead, invoke the unlock-notify callback with the context
          ** array already accumulated. We can then clear the array and
          ** begin accumulating any further context pointers without 
          ** requiring any dynamic allocation. This is sub-optimal because
          ** it means that instead of one callback with a large array of
          ** context pointers the application will receive two or more
          ** callbacks with smaller arrays of context pointers, which will
          ** reduce the applications ability to prioritize multiple 
          ** connections. But it is the best that can be done under the
          ** circumstances.
          */
          xUnlockNotify(aArg, nArg);
          nArg = 0;
        }
      }
      sqlite3EndBenignMalloc();

      aArg[nArg++] = p->pUnlockArg;
      xUnlockNotify = p->xUnlockNotify;
      p->pUnlockConnection = 0;
      p->xUnlockNotify = 0;
      p->pUnlockArg = 0;
    }

    /* Step 3. */
    if( p->pBlockingConnection==0 && p->pUnlockConnection==0 ){
      /* Remove connection p from the blocked connections list. */
      *pp = p->pNextBlocked;
      p->pNextBlocked = 0;
    }else{
      pp = &p->pNextBlocked;
    }
  }

  if( nArg!=0 ){
    xUnlockNotify(aArg, nArg);
  }
  sqlite3_free(aDyn);
  leaveMutex();         /* Leave STATIC_MASTER mutex */
}

/*
** This is called when the database connection passed as an argument is 
** being closed. The connection is removed from the blocked list.
*/
SQLITE_PRIVATE void sqlite3ConnectionClosed(sqlite3 *db){
  sqlite3ConnectionUnlocked(db);
  enterMutex();
  removeFromBlockedList(db);
  checkListProperties(db);
  leaveMutex();
}
#endif

/************** End of notify.c **********************************************/
/************** Begin file fts3.c ********************************************/
/*
** 2006 Oct 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This is an SQLite module implementing full-text search.
*/

/*
** The code in this file is only compiled if:
**
**     * The FTS3 module is being built as an extension
**       (in which case SQLITE_CORE is not defined), or
**
**     * The FTS3 module is being built into the core of
**       SQLite (in which case SQLITE_ENABLE_FTS3 is defined).
*/

/* The full-text index is stored in a series of b+tree (-like)
** structures called segments which map terms to doclists.  The
** structures are like b+trees in layout, but are constructed from the
** bottom up in optimal fashion and are not updatable.  Since trees
** are built from the bottom up, things will be described from the
** bottom up.
**
**
**** Varints ****
** The basic unit of encoding is a variable-length integer called a
** varint.  We encode variable-length integers in little-endian order
** using seven bits * per byte as follows:
**
** KEY:
**         A = 0xxxxxxx    7 bits of data and one flag bit
**         B = 1xxxxxxx    7 bits of data and one flag bit
**
**  7 bits - A
** 14 bits - BA
** 21 bits - BBA
** and so on.
**
** This is similar in concept to how sqlite encodes "varints" but
** the encoding is not the same.  SQLite varints are big-endian
** are are limited to 9 bytes in length whereas FTS3 varints are
** little-endian and can be up to 10 bytes in length (in theory).
**
** Example encodings:
**
**     1:    0x01
**   127:    0x7f
**   128:    0x81 0x00
**
**
**** Document lists ****
** A doclist (document list) holds a docid-sorted list of hits for a
** given term.  Doclists hold docids and associated token positions.
** A docid is the unique integer identifier for a single document.
** A position is the index of a word within the document.  The first 
** word of the document has a position of 0.
**
** FTS3 used to optionally store character offsets using a compile-time
** option.  But that functionality is no longer supported.
**
** A doclist is stored like this:
**
** array {
**   varint docid;          (delta from previous doclist)
**   array {                (position list for column 0)
**     varint position;     (2 more than the delta from previous position)
**   }
**   array {
**     varint POS_COLUMN;   (marks start of position list for new column)
**     varint column;       (index of new column)
**     array {
**       varint position;   (2 more than the delta from previous position)
**     }
**   }
**   varint POS_END;        (marks end of positions for this document.
** }
**
** Here, array { X } means zero or more occurrences of X, adjacent in
** memory.  A "position" is an index of a token in the token stream
** generated by the tokenizer. Note that POS_END and POS_COLUMN occur 
** in the same logical place as the position element, and act as sentinals
** ending a position list array.  POS_END is 0.  POS_COLUMN is 1.
** The positions numbers are not stored literally but rather as two more
** than the difference from the prior position, or the just the position plus
** 2 for the first position.  Example:
**
**   label:       A B C D E  F  G H   I  J K
**   value:     123 5 9 1 1 14 35 0 234 72 0
**
** The 123 value is the first docid.  For column zero in this document
** there are two matches at positions 3 and 10 (5-2 and 9-2+3).  The 1
** at D signals the start of a new column; the 1 at E indicates that the
** new column is column number 1.  There are two positions at 12 and 45
** (14-2 and 35-2+12).  The 0 at H indicate the end-of-document.  The
** 234 at I is the delta to next docid (357).  It has one position 70
** (72-2) and then terminates with the 0 at K.
**
** A "position-list" is the list of positions for multiple columns for
** a single docid.  A "column-list" is the set of positions for a single
** column.  Hence, a position-list consists of one or more column-lists,
** a document record consists of a docid followed by a position-list and
** a doclist consists of one or more document records.
**
** A bare doclist omits the position information, becoming an 
** array of varint-encoded docids.
**
**** Segment leaf nodes ****
** Segment leaf nodes store terms and doclists, ordered by term.  Leaf
** nodes are written using LeafWriter, and read using LeafReader (to
** iterate through a single leaf node's data) and LeavesReader (to
** iterate through a segment's entire leaf layer).  Leaf nodes have
** the format:
**
** varint iHeight;             (height from leaf level, always 0)
** varint nTerm;               (length of first term)
** char pTerm[nTerm];          (content of first term)
** varint nDoclist;            (length of term's associated doclist)
** char pDoclist[nDoclist];    (content of doclist)
** array {
**                             (further terms are delta-encoded)
**   varint nPrefix;           (length of prefix shared with previous term)
**   varint nSuffix;           (length of unshared suffix)
**   char pTermSuffix[nSuffix];(unshared suffix of next term)
**   varint nDoclist;          (length of term's associated doclist)
**   char pDoclist[nDoclist];  (content of doclist)
** }
**
** Here, array { X } means zero or more occurrences of X, adjacent in
** memory.
**
** Leaf nodes are broken into blocks which are stored contiguously in
** the %_segments table in sorted order.  This means that when the end
** of a node is reached, the next term is in the node with the next
** greater node id.
**
** New data is spilled to a new leaf node when the current node
** exceeds LEAF_MAX bytes (default 2048).  New data which itself is
** larger than STANDALONE_MIN (default 1024) is placed in a standalone
** node (a leaf node with a single term and doclist).  The goal of
** these settings is to pack together groups of small doclists while
** making it efficient to directly access large doclists.  The
** assumption is that large doclists represent terms which are more
** likely to be query targets.
**
** TODO(shess) It may be useful for blocking decisions to be more
** dynamic.  For instance, it may make more sense to have a 2.5k leaf
** node rather than splitting into 2k and .5k nodes.  My intuition is
** that this might extend through 2x or 4x the pagesize.
**
**
**** Segment interior nodes ****
** Segment interior nodes store blockids for subtree nodes and terms
** to describe what data is stored by the each subtree.  Interior
** nodes are written using InteriorWriter, and read using
** InteriorReader.  InteriorWriters are created as needed when
** SegmentWriter creates new leaf nodes, or when an interior node
** itself grows too big and must be split.  The format of interior
** nodes:
**
** varint iHeight;           (height from leaf level, always >0)
** varint iBlockid;          (block id of node's leftmost subtree)
** optional {
**   varint nTerm;           (length of first term)
**   char pTerm[nTerm];      (content of first term)
**   array {
**                                (further terms are delta-encoded)
**     varint nPrefix;            (length of shared prefix with previous term)
**     varint nSuffix;            (length of unshared suffix)
**     char pTermSuffix[nSuffix]; (unshared suffix of next term)
**   }
** }
**
** Here, optional { X } means an optional element, while array { X }
** means zero or more occurrences of X, adjacent in memory.
**
** An interior node encodes n terms separating n+1 subtrees.  The
** subtree blocks are contiguous, so only the first subtree's blockid
** is encoded.  The subtree at iBlockid will contain all terms less
** than the first term encoded (or all terms if no term is encoded).
** Otherwise, for terms greater than or equal to pTerm[i] but less
** than pTerm[i+1], the subtree for that term will be rooted at
** iBlockid+i.  Interior nodes only store enough term data to
** distinguish adjacent children (if the rightmost term of the left
** child is "something", and the leftmost term of the right child is
** "wicked", only "w" is stored).
**
** New data is spilled to a new interior node at the same height when
** the current node exceeds INTERIOR_MAX bytes (default 2048).
** INTERIOR_MIN_TERMS (default 7) keeps large terms from monopolizing
** interior nodes and making the tree too skinny.  The interior nodes
** at a given height are naturally tracked by interior nodes at
** height+1, and so on.
**
**
**** Segment directory ****
** The segment directory in table %_segdir stores meta-information for
** merging and deleting segments, and also the root node of the
** segment's tree.
**
** The root node is the top node of the segment's tree after encoding
** the entire segment, restricted to ROOT_MAX bytes (default 1024).
** This could be either a leaf node or an interior node.  If the top
** node requires more than ROOT_MAX bytes, it is flushed to %_segments
** and a new root interior node is generated (which should always fit
** within ROOT_MAX because it only needs space for 2 varints, the
** height and the blockid of the previous root).
**
** The meta-information in the segment directory is:
**   level               - segment level (see below)
**   idx                 - index within level
**                       - (level,idx uniquely identify a segment)
**   start_block         - first leaf node
**   leaves_end_block    - last leaf node
**   end_block           - last block (including interior nodes)
**   root                - contents of root node
**
** If the root node is a leaf node, then start_block,
** leaves_end_block, and end_block are all 0.
**
**
**** Segment merging ****
** To amortize update costs, segments are grouped into levels and
** merged in batches.  Each increase in level represents exponentially
** more documents.
**
** New documents (actually, document updates) are tokenized and
** written individually (using LeafWriter) to a level 0 segment, with
** incrementing idx.  When idx reaches MERGE_COUNT (default 16), all
** level 0 segments are merged into a single level 1 segment.  Level 1
** is populated like level 0, and eventually MERGE_COUNT level 1
** segments are merged to a single level 2 segment (representing
** MERGE_COUNT^2 updates), and so on.
**
** A segment merge traverses all segments at a given level in
** parallel, performing a straightforward sorted merge.  Since segment
** leaf nodes are written in to the %_segments table in order, this
** merge traverses the underlying sqlite disk structures efficiently.
** After the merge, all segment blocks from the merged level are
** deleted.
**
** MERGE_COUNT controls how often we merge segments.  16 seems to be
** somewhat of a sweet spot for insertion performance.  32 and 64 show
** very similar performance numbers to 16 on insertion, though they're
** a tiny bit slower (perhaps due to more overhead in merge-time
** sorting).  8 is about 20% slower than 16, 4 about 50% slower than
** 16, 2 about 66% slower than 16.
**
** At query time, high MERGE_COUNT increases the number of segments
** which need to be scanned and merged.  For instance, with 100k docs
** inserted:
**
**    MERGE_COUNT   segments
**       16           25
**        8           12
**        4           10
**        2            6
**
** This appears to have only a moderate impact on queries for very
** frequent terms (which are somewhat dominated by segment merge
** costs), and infrequent and non-existent terms still seem to be fast
** even with many segments.
**
** TODO(shess) That said, it would be nice to have a better query-side
** argument for MERGE_COUNT of 16.  Also, it is possible/likely that
** optimizations to things like doclist merging will swing the sweet
** spot around.
**
**
**
**** Handling of deletions and updates ****
** Since we're using a segmented structure, with no docid-oriented
** index into the term index, we clearly cannot simply update the term
** index when a document is deleted or updated.  For deletions, we
** write an empty doclist (varint(docid) varint(POS_END)), for updates
** we simply write the new doclist.  Segment merges overwrite older
** data for a particular docid with newer data, so deletes or updates
** will eventually overtake the earlier data and knock it out.  The
** query logic likewise merges doclists so that newer data knocks out
** older data.
*/

/************** Include fts3Int.h in the middle of fts3.c ********************/
/************** Begin file fts3Int.h *****************************************/
/*
** 2009 Nov 12
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
*/
#ifndef _FTSINT_H
#define _FTSINT_H

#if !defined(NDEBUG) && !defined(SQLITE_DEBUG) 
# define NDEBUG 1
#endif

/*
** FTS4 is really an extension for FTS3.  It is enabled using the
** SQLITE_ENABLE_FTS3 macro.  But to avoid confusion we also all
** the SQLITE_ENABLE_FTS4 macro to serve as an alisse for SQLITE_ENABLE_FTS3.
*/
#if defined(SQLITE_ENABLE_FTS4) && !defined(SQLITE_ENABLE_FTS3)
# define SQLITE_ENABLE_FTS3
#endif

#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* If not building as part of the core, include sqlite3ext.h. */
#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

/************** Include fts3_tokenizer.h in the middle of fts3Int.h **********/
/************** Begin file fts3_tokenizer.h **********************************/
/*
** 2006 July 10
**
** The author disclaims copyright to this source code.
**
*************************************************************************
** Defines the interface to tokenizers used by fulltext-search.  There
** are three basic components:
**
** sqlite3_tokenizer_module is a singleton defining the tokenizer
** interface functions.  This is essentially the class structure for
** tokenizers.
**
** sqlite3_tokenizer is used to define a particular tokenizer, perhaps
** including customization information defined at creation time.
**
** sqlite3_tokenizer_cursor is generated by a tokenizer to generate
** tokens from a particular input.
*/
#ifndef _FTS3_TOKENIZER_H_
#define _FTS3_TOKENIZER_H_

/* TODO(shess) Only used for SQLITE_OK and SQLITE_DONE at this time.
** If tokenizers are to be allowed to call sqlite3_*() functions, then
** we will need a way to register the API consistently.
*/

/*
** Structures used by the tokenizer interface. When a new tokenizer
** implementation is registered, the caller provides a pointer to
** an sqlite3_tokenizer_module containing pointers to the callback
** functions that make up an implementation.
**
** When an fts3 table is created, it passes any arguments passed to
** the tokenizer clause of the CREATE VIRTUAL TABLE statement to the
** sqlite3_tokenizer_module.xCreate() function of the requested tokenizer
** implementation. The xCreate() function in turn returns an 
** sqlite3_tokenizer structure representing the specific tokenizer to
** be used for the fts3 table (customized by the tokenizer clause arguments).
**
** To tokenize an input buffer, the sqlite3_tokenizer_module.xOpen()
** method is called. It returns an sqlite3_tokenizer_cursor object
** that may be used to tokenize a specific input buffer based on
** the tokenization rules supplied by a specific sqlite3_tokenizer
** object.
*/
typedef struct sqlite3_tokenizer_module sqlite3_tokenizer_module;
typedef struct sqlite3_tokenizer sqlite3_tokenizer;
typedef struct sqlite3_tokenizer_cursor sqlite3_tokenizer_cursor;

struct sqlite3_tokenizer_module {

  /*
  ** Structure version. Should always be set to 0 or 1.
  */
  int iVersion;

  /*
  ** Create a new tokenizer. The values in the argv[] array are the
  ** arguments passed to the "tokenizer" clause of the CREATE VIRTUAL
  ** TABLE statement that created the fts3 table. For example, if
  ** the following SQL is executed:
  **
  **   CREATE .. USING fts3( ... , tokenizer <tokenizer-name> arg1 arg2)
  **
  ** then argc is set to 2, and the argv[] array contains pointers
  ** to the strings "arg1" and "arg2".
  **
  ** This method should return either SQLITE_OK (0), or an SQLite error 
  ** code. If SQLITE_OK is returned, then *ppTokenizer should be set
  ** to point at the newly created tokenizer structure. The generic
  ** sqlite3_tokenizer.pModule variable should not be initialized by
  ** this callback. The caller will do so.
  */
  int (*xCreate)(
    int argc,                           /* Size of argv array */
    const char *const*argv,             /* Tokenizer argument strings */
    sqlite3_tokenizer **ppTokenizer     /* OUT: Created tokenizer */
  );

  /*
  ** Destroy an existing tokenizer. The fts3 module calls this method
  ** exactly once for each successful call to xCreate().
  */
  int (*xDestroy)(sqlite3_tokenizer *pTokenizer);

  /*
  ** Create a tokenizer cursor to tokenize an input buffer. The caller
  ** is responsible for ensuring that the input buffer remains valid
  ** until the cursor is closed (using the xClose() method). 
  */
  int (*xOpen)(
    sqlite3_tokenizer *pTokenizer,       /* Tokenizer object */
    const char *pInput, int nBytes,      /* Input buffer */
    sqlite3_tokenizer_cursor **ppCursor  /* OUT: Created tokenizer cursor */
  );

  /*
  ** Destroy an existing tokenizer cursor. The fts3 module calls this 
  ** method exactly once for each successful call to xOpen().
  */
  int (*xClose)(sqlite3_tokenizer_cursor *pCursor);

  /*
  ** Retrieve the next token from the tokenizer cursor pCursor. This
  ** method should either return SQLITE_OK and set the values of the
  ** "OUT" variables identified below, or SQLITE_DONE to indicate that
  ** the end of the buffer has been reached, or an SQLite error code.
  **
  ** *ppToken should be set to point at a buffer containing the 
  ** normalized version of the token (i.e. after any case-folding and/or
  ** stemming has been performed). *pnBytes should be set to the length
  ** of this buffer in bytes. The input text that generated the token is
  ** identified by the byte offsets returned in *piStartOffset and
  ** *piEndOffset. *piStartOffset should be set to the index of the first
  ** byte of the token in the input buffer. *piEndOffset should be set
  ** to the index of the first byte just past the end of the token in
  ** the input buffer.
  **
  ** The buffer *ppToken is set to point at is managed by the tokenizer
  ** implementation. It is only required to be valid until the next call
  ** to xNext() or xClose(). 
  */
  /* TODO(shess) current implementation requires pInput to be
  ** nul-terminated.  This should either be fixed, or pInput/nBytes
  ** should be converted to zInput.
  */
  int (*xNext)(
    sqlite3_tokenizer_cursor *pCursor,   /* Tokenizer cursor */
    const char **ppToken, int *pnBytes,  /* OUT: Normalized text for token */
    int *piStartOffset,  /* OUT: Byte offset of token in input buffer */
    int *piEndOffset,    /* OUT: Byte offset of end of token in input buffer */
    int *piPosition      /* OUT: Number of tokens returned before this one */
  );

  /***********************************************************************
  ** Methods below this point are only available if iVersion>=1.
  */

  /* 
  ** Configure the language id of a tokenizer cursor.
  */
  int (*xLanguageid)(sqlite3_tokenizer_cursor *pCsr, int iLangid);
};

struct sqlite3_tokenizer {
  const sqlite3_tokenizer_module *pModule;  /* The module for this tokenizer */
  /* Tokenizer implementations will typically add additional fields */
};

struct sqlite3_tokenizer_cursor {
  sqlite3_tokenizer *pTokenizer;       /* Tokenizer for this cursor. */
  /* Tokenizer implementations will typically add additional fields */
};

int fts3_global_term_cnt(int iTerm, int iCol);
int fts3_term_cnt(int iTerm, int iCol);


#endif /* _FTS3_TOKENIZER_H_ */

/************** End of fts3_tokenizer.h **************************************/
/************** Continuing where we left off in fts3Int.h ********************/
/************** Include fts3_hash.h in the middle of fts3Int.h ***************/
/************** Begin file fts3_hash.h ***************************************/
/*
** 2001 September 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This is the header file for the generic hash-table implementation
** used in SQLite.  We've modified it slightly to serve as a standalone
** hash table implementation for the full-text indexing module.
**
*/
#ifndef _FTS3_HASH_H_
#define _FTS3_HASH_H_

/* Forward declarations of structures. */
typedef struct Fts3Hash Fts3Hash;
typedef struct Fts3HashElem Fts3HashElem;

/* A complete hash table is an instance of the following structure.
** The internals of this structure are intended to be opaque -- client
** code should not attempt to access or modify the fields of this structure
** directly.  Change this structure only by using the routines below.
** However, many of the "procedures" and "functions" for modifying and
** accessing this structure are really macros, so we can't really make
** this structure opaque.
*/
struct Fts3Hash {
  char keyClass;          /* HASH_INT, _POINTER, _STRING, _BINARY */
  char copyKey;           /* True if copy of key made on insert */
  int count;              /* Number of entries in this table */
  Fts3HashElem *first;    /* The first element of the array */
  int htsize;             /* Number of buckets in the hash table */
  struct _fts3ht {        /* the hash table */
    int count;               /* Number of entries with this hash */
    Fts3HashElem *chain;     /* Pointer to first entry with this hash */
  } *ht;
};

/* Each element in the hash table is an instance of the following 
** structure.  All elements are stored on a single doubly-linked list.
**
** Again, this structure is intended to be opaque, but it can't really
** be opaque because it is used by macros.
*/
struct Fts3HashElem {
  Fts3HashElem *next, *prev; /* Next and previous elements in the table */
  void *data;                /* Data associated with this element */
  void *pKey; int nKey;      /* Key associated with this element */
};

/*
** There are 2 different modes of operation for a hash table:
**
**   FTS3_HASH_STRING        pKey points to a string that is nKey bytes long
**                           (including the null-terminator, if any).  Case
**                           is respected in comparisons.
**
**   FTS3_HASH_BINARY        pKey points to binary data nKey bytes long. 
**                           memcmp() is used to compare keys.
**
** A copy of the key is made if the copyKey parameter to fts3HashInit is 1.  
*/
#define FTS3_HASH_STRING    1
#define FTS3_HASH_BINARY    2

/*
** Access routines.  To delete, insert a NULL pointer.
*/
SQLITE_PRIVATE void sqlite3Fts3HashInit(Fts3Hash *pNew, char keyClass, char copyKey);
SQLITE_PRIVATE void *sqlite3Fts3HashInsert(Fts3Hash*, const void *pKey, int nKey, void *pData);
SQLITE_PRIVATE void *sqlite3Fts3HashFind(const Fts3Hash*, const void *pKey, int nKey);
SQLITE_PRIVATE void sqlite3Fts3HashClear(Fts3Hash*);
SQLITE_PRIVATE Fts3HashElem *sqlite3Fts3HashFindElem(const Fts3Hash *, const void *, int);

/*
** Shorthand for the functions above
*/
#define fts3HashInit     sqlite3Fts3HashInit
#define fts3HashInsert   sqlite3Fts3HashInsert
#define fts3HashFind     sqlite3Fts3HashFind
#define fts3HashClear    sqlite3Fts3HashClear
#define fts3HashFindElem sqlite3Fts3HashFindElem

/*
** Macros for looping over all elements of a hash table.  The idiom is
** like this:
**
**   Fts3Hash h;
**   Fts3HashElem *p;
**   ...
**   for(p=fts3HashFirst(&h); p; p=fts3HashNext(p)){
**     SomeStructure *pData = fts3HashData(p);
**     // do something with pData
**   }
*/
#define fts3HashFirst(H)  ((H)->first)
#define fts3HashNext(E)   ((E)->next)
#define fts3HashData(E)   ((E)->data)
#define fts3HashKey(E)    ((E)->pKey)
#define fts3HashKeysize(E) ((E)->nKey)

/*
** Number of entries in a hash table
*/
#define fts3HashCount(H)  ((H)->count)

#endif /* _FTS3_HASH_H_ */

/************** End of fts3_hash.h *******************************************/
/************** Continuing where we left off in fts3Int.h ********************/

/*
** This constant determines the maximum depth of an FTS expression tree
** that the library will create and use. FTS uses recursion to perform 
** various operations on the query tree, so the disadvantage of a large
** limit is that it may allow very large queries to use large amounts
** of stack space (perhaps causing a stack overflow).
*/
#ifndef SQLITE_FTS3_MAX_EXPR_DEPTH
# define SQLITE_FTS3_MAX_EXPR_DEPTH 12
#endif


/*
** This constant controls how often segments are merged. Once there are
** FTS3_MERGE_COUNT segments of level N, they are merged into a single
** segment of level N+1.
*/
#define FTS3_MERGE_COUNT 16

/*
** This is the maximum amount of data (in bytes) to store in the 
** Fts3Table.pendingTerms hash table. Normally, the hash table is
** populated as documents are inserted/updated/deleted in a transaction
** and used to create a new segment when the transaction is committed.
** However if this limit is reached midway through a transaction, a new 
** segment is created and the hash table cleared immediately.
*/
#define FTS3_MAX_PENDING_DATA (1*1024*1024)

/*
** Macro to return the number of elements in an array. SQLite has a
** similar macro called ArraySize(). Use a different name to avoid
** a collision when building an amalgamation with built-in FTS3.
*/
#define SizeofArray(X) ((int)(sizeof(X)/sizeof(X[0])))


#ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
#endif
#ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
#endif

/*
** Maximum length of a varint encoded integer. The varint format is different
** from that used by SQLite, so the maximum length is 10, not 9.
*/
#define FTS3_VARINT_MAX 10

/*
** FTS4 virtual tables may maintain multiple indexes - one index of all terms
** in the document set and zero or more prefix indexes. All indexes are stored
** as one or more b+-trees in the %_segments and %_segdir tables. 
**
** It is possible to determine which index a b+-tree belongs to based on the
** value stored in the "%_segdir.level" column. Given this value L, the index
** that the b+-tree belongs to is (L<<10). In other words, all b+-trees with
** level values between 0 and 1023 (inclusive) belong to index 0, all levels
** between 1024 and 2047 to index 1, and so on.
**
** It is considered impossible for an index to use more than 1024 levels. In 
** theory though this may happen, but only after at least 
** (FTS3_MERGE_COUNT^1024) separate flushes of the pending-terms tables.
*/
#define FTS3_SEGDIR_MAXLEVEL      1024
#define FTS3_SEGDIR_MAXLEVEL_STR "1024"

/*
** The testcase() macro is only used by the amalgamation.  If undefined,
** make it a no-op.
*/
#ifndef testcase
# define testcase(X)
#endif

/*
** Terminator values for position-lists and column-lists.
*/
#define POS_COLUMN  (1)     /* Column-list terminator */
#define POS_END     (0)     /* Position-list terminator */ 

/*
** This section provides definitions to allow the
** FTS3 extension to be compiled outside of the 
** amalgamation.
*/
#ifndef SQLITE_AMALGAMATION
/*
** Macros indicating that conditional expressions are always true or
** false.
*/
#ifdef SQLITE_COVERAGE_TEST
# define ALWAYS(x) (1)
# define NEVER(X)  (0)
#else
# define ALWAYS(x) (x)
# define NEVER(x)  (x)
#endif

/*
** Internal types used by SQLite.
*/
typedef unsigned char u8;         /* 1-byte (or larger) unsigned integer */
typedef short int i16;            /* 2-byte (or larger) signed integer */
typedef unsigned int u32;         /* 4-byte unsigned integer */
typedef sqlite3_uint64 u64;       /* 8-byte unsigned integer */
typedef sqlite3_int64 i64;        /* 8-byte signed integer */

/*
** Macro used to suppress compiler warnings for unused parameters.
*/
#define UNUSED_PARAMETER(x) (void)(x)

/*
** Activate assert() only if SQLITE_TEST is enabled.
*/
#if !defined(NDEBUG) && !defined(SQLITE_DEBUG) 
# define NDEBUG 1
#endif

/*
** The TESTONLY macro is used to enclose variable declarations or
** other bits of code that are needed to support the arguments
** within testcase() and assert() macros.
*/
#if defined(SQLITE_DEBUG) || defined(SQLITE_COVERAGE_TEST)
# define TESTONLY(X)  X
#else
# define TESTONLY(X)
#endif

#endif /* SQLITE_AMALGAMATION */

#ifdef SQLITE_DEBUG
SQLITE_PRIVATE int sqlite3Fts3Corrupt(void);
# define FTS_CORRUPT_VTAB sqlite3Fts3Corrupt()
#else
# define FTS_CORRUPT_VTAB SQLITE_CORRUPT_VTAB
#endif

typedef struct Fts3Table Fts3Table;
typedef struct Fts3Cursor Fts3Cursor;
typedef struct Fts3Expr Fts3Expr;
typedef struct Fts3Phrase Fts3Phrase;
typedef struct Fts3PhraseToken Fts3PhraseToken;

typedef struct Fts3Doclist Fts3Doclist;
typedef struct Fts3SegFilter Fts3SegFilter;
typedef struct Fts3DeferredToken Fts3DeferredToken;
typedef struct Fts3SegReader Fts3SegReader;
typedef struct Fts3MultiSegReader Fts3MultiSegReader;

/*
** A connection to a fulltext index is an instance of the following
** structure. The xCreate and xConnect methods create an instance
** of this structure and xDestroy and xDisconnect free that instance.
** All other methods receive a pointer to the structure as one of their
** arguments.
*/
struct Fts3Table {
  sqlite3_vtab base;              /* Base class used by SQLite core */
  sqlite3 *db;                    /* The database connection */
  const char *zDb;                /* logical database name */
  const char *zName;              /* virtual table name */
  int nColumn;                    /* number of named columns in virtual table */
  char **azColumn;                /* column names.  malloced */
  u8 *abNotindexed;               /* True for 'notindexed' columns */
  sqlite3_tokenizer *pTokenizer;  /* tokenizer for inserts and queries */
  char *zContentTbl;              /* content=xxx option, or NULL */
  char *zLanguageid;              /* languageid=xxx option, or NULL */
  int nAutoincrmerge;             /* Value configured by 'automerge' */
  u32 nLeafAdd;                   /* Number of leaf blocks added this trans */

  /* Precompiled statements used by the implementation. Each of these 
  ** statements is run and reset within a single virtual table API call. 
  */
  sqlite3_stmt *aStmt[40];

  char *zReadExprlist;
  char *zWriteExprlist;

  int nNodeSize;                  /* Soft limit for node size */
  u8 bFts4;                       /* True for FTS4, false for FTS3 */
  u8 bHasStat;                    /* True if %_stat table exists (2==unknown) */
  u8 bHasDocsize;                 /* True if %_docsize table exists */
  u8 bDescIdx;                    /* True if doclists are in reverse order */
  u8 bIgnoreSavepoint;            /* True to ignore xSavepoint invocations */
  int nPgsz;                      /* Page size for host database */
  char *zSegmentsTbl;             /* Name of %_segments table */
  sqlite3_blob *pSegments;        /* Blob handle open on %_segments table */

  /* 
  ** The following array of hash tables is used to buffer pending index 
  ** updates during transactions. All pending updates buffered at any one
  ** time must share a common language-id (see the FTS4 langid= feature).
  ** The current language id is stored in variable iPrevLangid.
  **
  ** A single FTS4 table may have multiple full-text indexes. For each index
  ** there is an entry in the aIndex[] array. Index 0 is an index of all the
  ** terms that appear in the document set. Each subsequent index in aIndex[]
  ** is an index of prefixes of a specific length.
  **
  ** Variable nPendingData contains an estimate the memory consumed by the 
  ** pending data structures, including hash table overhead, but not including
  ** malloc overhead.  When nPendingData exceeds nMaxPendingData, all hash
  ** tables are flushed to disk. Variable iPrevDocid is the docid of the most 
  ** recently inserted record.
  */
  int nIndex;                     /* Size of aIndex[] */
  struct Fts3Index {
    int nPrefix;                  /* Prefix length (0 for main terms index) */
    Fts3Hash hPending;            /* Pending terms table for this index */
  } *aIndex;
  int nMaxPendingData;            /* Max pending data before flush to disk */
  int nPendingData;               /* Current bytes of pending data */
  sqlite_int64 iPrevDocid;        /* Docid of most recently inserted document */
  int iPrevLangid;                /* Langid of recently inserted document */

#if defined(SQLITE_DEBUG) || defined(SQLITE_COVERAGE_TEST)
  /* State variables used for validating that the transaction control
  ** methods of the virtual table are called at appropriate times.  These
  ** values do not contribute to FTS functionality; they are used for
  ** verifying the operation of the SQLite core.
  */
  int inTransaction;     /* True after xBegin but before xCommit/xRollback */
  int mxSavepoint;       /* Largest valid xSavepoint integer */
#endif

#ifdef SQLITE_TEST
  /* True to disable the incremental doclist optimization. This is controled
  ** by special insert command 'test-no-incr-doclist'.  */
  int bNoIncrDoclist;
#endif
};

/*
** When the core wants to read from the virtual table, it creates a
** virtual table cursor (an instance of the following structure) using
** the xOpen method. Cursors are destroyed using the xClose method.
*/
struct Fts3Cursor {
  sqlite3_vtab_cursor base;       /* Base class used by SQLite core */
  i16 eSearch;                    /* Search strategy (see below) */
  u8 isEof;                       /* True if at End Of Results */
  u8 isRequireSeek;               /* True if must seek pStmt to %_content row */
  sqlite3_stmt *pStmt;            /* Prepared statement in use by the cursor */
  Fts3Expr *pExpr;                /* Parsed MATCH query string */
  int iLangid;                    /* Language being queried for */
  int nPhrase;                    /* Number of matchable phrases in query */
  Fts3DeferredToken *pDeferred;   /* Deferred search tokens, if any */
  sqlite3_int64 iPrevId;          /* Previous id read from aDoclist */
  char *pNextId;                  /* Pointer into the body of aDoclist */
  char *aDoclist;                 /* List of docids for full-text queries */
  int nDoclist;                   /* Size of buffer at aDoclist */
  u8 bDesc;                       /* True to sort in descending order */
  int eEvalmode;                  /* An FTS3_EVAL_XX constant */
  int nRowAvg;                    /* Average size of database rows, in pages */
  sqlite3_int64 nDoc;             /* Documents in table */
  i64 iMinDocid;                  /* Minimum docid to return */
  i64 iMaxDocid;                  /* Maximum docid to return */
  int isMatchinfoNeeded;          /* True when aMatchinfo[] needs filling in */
  u32 *aMatchinfo;                /* Information about most recent match */
  int nMatchinfo;                 /* Number of elements in aMatchinfo[] */
  char *zMatchinfo;               /* Matchinfo specification */
};

#define FTS3_EVAL_FILTER    0
#define FTS3_EVAL_NEXT      1
#define FTS3_EVAL_MATCHINFO 2

/*
** The Fts3Cursor.eSearch member is always set to one of the following.
** Actualy, Fts3Cursor.eSearch can be greater than or equal to
** FTS3_FULLTEXT_SEARCH.  If so, then Fts3Cursor.eSearch - 2 is the index
** of the column to be searched.  For example, in
**
**     CREATE VIRTUAL TABLE ex1 USING fts3(a,b,c,d);
**     SELECT docid FROM ex1 WHERE b MATCH 'one two three';
** 
** Because the LHS of the MATCH operator is 2nd column "b",
** Fts3Cursor.eSearch will be set to FTS3_FULLTEXT_SEARCH+1.  (+0 for a,
** +1 for b, +2 for c, +3 for d.)  If the LHS of MATCH were "ex1" 
** indicating that all columns should be searched,
** then eSearch would be set to FTS3_FULLTEXT_SEARCH+4.
*/
#define FTS3_FULLSCAN_SEARCH 0    /* Linear scan of %_content table */
#define FTS3_DOCID_SEARCH    1    /* Lookup by rowid on %_content table */
#define FTS3_FULLTEXT_SEARCH 2    /* Full-text index search */

/*
** The lower 16-bits of the sqlite3_index_info.idxNum value set by
** the xBestIndex() method contains the Fts3Cursor.eSearch value described
** above. The upper 16-bits contain a combination of the following
** bits, used to describe extra constraints on full-text searches.
*/
#define FTS3_HAVE_LANGID    0x00010000      /* languageid=? */
#define FTS3_HAVE_DOCID_GE  0x00020000      /* docid>=? */
#define FTS3_HAVE_DOCID_LE  0x00040000      /* docid<=? */

struct Fts3Doclist {
  char *aAll;                    /* Array containing doclist (or NULL) */
  int nAll;                      /* Size of a[] in bytes */
  char *pNextDocid;              /* Pointer to next docid */

  sqlite3_int64 iDocid;          /* Current docid (if pList!=0) */
  int bFreeList;                 /* True if pList should be sqlite3_free()d */
  char *pList;                   /* Pointer to position list following iDocid */
  int nList;                     /* Length of position list */
};

/*
** A "phrase" is a sequence of one or more tokens that must match in
** sequence.  A single token is the base case and the most common case.
** For a sequence of tokens contained in double-quotes (i.e. "one two three")
** nToken will be the number of tokens in the string.
*/
struct Fts3PhraseToken {
  char *z;                        /* Text of the token */
  int n;                          /* Number of bytes in buffer z */
  int isPrefix;                   /* True if token ends with a "*" character */
  int bFirst;                     /* True if token must appear at position 0 */

  /* Variables above this point are populated when the expression is
  ** parsed (by code in fts3_expr.c). Below this point the variables are
  ** used when evaluating the expression. */
  Fts3DeferredToken *pDeferred;   /* Deferred token object for this token */
  Fts3MultiSegReader *pSegcsr;    /* Segment-reader for this token */
};

struct Fts3Phrase {
  /* Cache of doclist for this phrase. */
  Fts3Doclist doclist;
  int bIncr;                 /* True if doclist is loaded incrementally */
  int iDoclistToken;

  /* Variables below this point are populated by fts3_expr.c when parsing 
  ** a MATCH expression. Everything above is part of the evaluation phase. 
  */
  int nToken;                /* Number of tokens in the phrase */
  int iColumn;               /* Index of column this phrase must match */
  Fts3PhraseToken aToken[1]; /* One entry for each token in the phrase */
};

/*
** A tree of these objects forms the RHS of a MATCH operator.
**
** If Fts3Expr.eType is FTSQUERY_PHRASE and isLoaded is true, then aDoclist 
** points to a malloced buffer, size nDoclist bytes, containing the results 
** of this phrase query in FTS3 doclist format. As usual, the initial 
** "Length" field found in doclists stored on disk is omitted from this 
** buffer.
**
** Variable aMI is used only for FTSQUERY_NEAR nodes to store the global
** matchinfo data. If it is not NULL, it points to an array of size nCol*3,
** where nCol is the number of columns in the queried FTS table. The array
** is populated as follows:
**
**   aMI[iCol*3 + 0] = Undefined
**   aMI[iCol*3 + 1] = Number of occurrences
**   aMI[iCol*3 + 2] = Number of rows containing at least one instance
**
** The aMI array is allocated using sqlite3_malloc(). It should be freed 
** when the expression node is.
*/
struct Fts3Expr {
  int eType;                 /* One of the FTSQUERY_XXX values defined below */
  int nNear;                 /* Valid if eType==FTSQUERY_NEAR */
  Fts3Expr *pParent;         /* pParent->pLeft==this or pParent->pRight==this */
  Fts3Expr *pLeft;           /* Left operand */
  Fts3Expr *pRight;          /* Right operand */
  Fts3Phrase *pPhrase;       /* Valid if eType==FTSQUERY_PHRASE */

  /* The following are used by the fts3_eval.c module. */
  sqlite3_int64 iDocid;      /* Current docid */
  u8 bEof;                   /* True this expression is at EOF already */
  u8 bStart;                 /* True if iDocid is valid */
  u8 bDeferred;              /* True if this expression is entirely deferred */

  u32 *aMI;
};

/*
** Candidate values for Fts3Query.eType. Note that the order of the first
** four values is in order of precedence when parsing expressions. For 
** example, the following:
**
**   "a OR b AND c NOT d NEAR e"
**
** is equivalent to:
**
**   "a OR (b AND (c NOT (d NEAR e)))"
*/
#define FTSQUERY_NEAR   1
#define FTSQUERY_NOT    2
#define FTSQUERY_AND    3
#define FTSQUERY_OR     4
#define FTSQUERY_PHRASE 5


/* fts3_write.c */
SQLITE_PRIVATE int sqlite3Fts3UpdateMethod(sqlite3_vtab*,int,sqlite3_value**,sqlite3_int64*);
SQLITE_PRIVATE int sqlite3Fts3PendingTermsFlush(Fts3Table *);
SQLITE_PRIVATE void sqlite3Fts3PendingTermsClear(Fts3Table *);
SQLITE_PRIVATE int sqlite3Fts3Optimize(Fts3Table *);
SQLITE_PRIVATE int sqlite3Fts3SegReaderNew(int, int, sqlite3_int64,
  sqlite3_int64, sqlite3_int64, const char *, int, Fts3SegReader**);
SQLITE_PRIVATE int sqlite3Fts3SegReaderPending(
  Fts3Table*,int,const char*,int,int,Fts3SegReader**);
SQLITE_PRIVATE void sqlite3Fts3SegReaderFree(Fts3SegReader *);
SQLITE_PRIVATE int sqlite3Fts3AllSegdirs(Fts3Table*, int, int, int, sqlite3_stmt **);
SQLITE_PRIVATE int sqlite3Fts3ReadBlock(Fts3Table*, sqlite3_int64, char **, int*, int*);

SQLITE_PRIVATE int sqlite3Fts3SelectDoctotal(Fts3Table *, sqlite3_stmt **);
SQLITE_PRIVATE int sqlite3Fts3SelectDocsize(Fts3Table *, sqlite3_int64, sqlite3_stmt **);

#ifndef SQLITE_DISABLE_FTS4_DEFERRED
SQLITE_PRIVATE void sqlite3Fts3FreeDeferredTokens(Fts3Cursor *);
SQLITE_PRIVATE int sqlite3Fts3DeferToken(Fts3Cursor *, Fts3PhraseToken *, int);
SQLITE_PRIVATE int sqlite3Fts3CacheDeferredDoclists(Fts3Cursor *);
SQLITE_PRIVATE void sqlite3Fts3FreeDeferredDoclists(Fts3Cursor *);
SQLITE_PRIVATE int sqlite3Fts3DeferredTokenList(Fts3DeferredToken *, char **, int *);
#else
# define sqlite3Fts3FreeDeferredTokens(x)
# define sqlite3Fts3DeferToken(x,y,z) SQLITE_OK
# define sqlite3Fts3CacheDeferredDoclists(x) SQLITE_OK
# define sqlite3Fts3FreeDeferredDoclists(x)
# define sqlite3Fts3DeferredTokenList(x,y,z) SQLITE_OK
#endif

SQLITE_PRIVATE void sqlite3Fts3SegmentsClose(Fts3Table *);
SQLITE_PRIVATE int sqlite3Fts3MaxLevel(Fts3Table *, int *);

/* Special values interpreted by sqlite3SegReaderCursor() */
#define FTS3_SEGCURSOR_PENDING        -1
#define FTS3_SEGCURSOR_ALL            -2

SQLITE_PRIVATE int sqlite3Fts3SegReaderStart(Fts3Table*, Fts3MultiSegReader*, Fts3SegFilter*);
SQLITE_PRIVATE int sqlite3Fts3SegReaderStep(Fts3Table *, Fts3MultiSegReader *);
SQLITE_PRIVATE void sqlite3Fts3SegReaderFinish(Fts3MultiSegReader *);

SQLITE_PRIVATE int sqlite3Fts3SegReaderCursor(Fts3Table *, 
    int, int, int, const char *, int, int, int, Fts3MultiSegReader *);

/* Flags allowed as part of the 4th argument to SegmentReaderIterate() */
#define FTS3_SEGMENT_REQUIRE_POS   0x00000001
#define FTS3_SEGMENT_IGNORE_EMPTY  0x00000002
#define FTS3_SEGMENT_COLUMN_FILTER 0x00000004
#define FTS3_SEGMENT_PREFIX        0x00000008
#define FTS3_SEGMENT_SCAN          0x00000010
#define FTS3_SEGMENT_FIRST         0x00000020

/* Type passed as 4th argument to SegmentReaderIterate() */
struct Fts3SegFilter {
  const char *zTerm;
  int nTerm;
  int iCol;
  int flags;
};

struct Fts3MultiSegReader {
  /* Used internally by sqlite3Fts3SegReaderXXX() calls */
  Fts3SegReader **apSegment;      /* Array of Fts3SegReader objects */
  int nSegment;                   /* Size of apSegment array */
  int nAdvance;                   /* How many seg-readers to advance */
  Fts3SegFilter *pFilter;         /* Pointer to filter object */
  char *aBuffer;                  /* Buffer to merge doclists in */
  int nBuffer;                    /* Allocated size of aBuffer[] in bytes */

  int iColFilter;                 /* If >=0, filter for this column */
  int bRestart;

  /* Used by fts3.c only. */
  int nCost;                      /* Cost of running iterator */
  int bLookup;                    /* True if a lookup of a single entry. */

  /* Output values. Valid only after Fts3SegReaderStep() returns SQLITE_ROW. */
  char *zTerm;                    /* Pointer to term buffer */
  int nTerm;                      /* Size of zTerm in bytes */
  char *aDoclist;                 /* Pointer to doclist buffer */
  int nDoclist;                   /* Size of aDoclist[] in bytes */
};

SQLITE_PRIVATE int sqlite3Fts3Incrmerge(Fts3Table*,int,int);

#define fts3GetVarint32(p, piVal) (                                           \
  (*(u8*)(p)&0x80) ? sqlite3Fts3GetVarint32(p, piVal) : (*piVal=*(u8*)(p), 1) \
)

/* fts3.c */
SQLITE_PRIVATE int sqlite3Fts3PutVarint(char *, sqlite3_int64);
SQLITE_PRIVATE int sqlite3Fts3GetVarint(const char *, sqlite_int64 *);
SQLITE_PRIVATE int sqlite3Fts3GetVarint32(const char *, int *);
SQLITE_PRIVATE int sqlite3Fts3VarintLen(sqlite3_uint64);
SQLITE_PRIVATE void sqlite3Fts3Dequote(char *);
SQLITE_PRIVATE void sqlite3Fts3DoclistPrev(int,char*,int,char**,sqlite3_int64*,int*,u8*);
SQLITE_PRIVATE int sqlite3Fts3EvalPhraseStats(Fts3Cursor *, Fts3Expr *, u32 *);
SQLITE_PRIVATE int sqlite3Fts3FirstFilter(sqlite3_int64, char *, int, char *);
SQLITE_PRIVATE void sqlite3Fts3CreateStatTable(int*, Fts3Table*);

/* fts3_tokenizer.c */
SQLITE_PRIVATE const char *sqlite3Fts3NextToken(const char *, int *);
SQLITE_PRIVATE int sqlite3Fts3InitHashTable(sqlite3 *, Fts3Hash *, const char *);
SQLITE_PRIVATE int sqlite3Fts3InitTokenizer(Fts3Hash *pHash, const char *, 
    sqlite3_tokenizer **, char **
);
SQLITE_PRIVATE int sqlite3Fts3IsIdChar(char);

/* fts3_snippet.c */
SQLITE_PRIVATE void sqlite3Fts3Offsets(sqlite3_context*, Fts3Cursor*);
SQLITE_PRIVATE void sqlite3Fts3Snippet(sqlite3_context *, Fts3Cursor *, const char *,
  const char *, const char *, int, int
);
SQLITE_PRIVATE void sqlite3Fts3Matchinfo(sqlite3_context *, Fts3Cursor *, const char *);

/* fts3_expr.c */
SQLITE_PRIVATE int sqlite3Fts3ExprParse(sqlite3_tokenizer *, int,
  char **, int, int, int, const char *, int, Fts3Expr **, char **
);
SQLITE_PRIVATE void sqlite3Fts3ExprFree(Fts3Expr *);
#ifdef SQLITE_TEST
SQLITE_PRIVATE int sqlite3Fts3ExprInitTestInterface(sqlite3 *db);
SQLITE_PRIVATE int sqlite3Fts3InitTerm(sqlite3 *db);
#endif

SQLITE_PRIVATE int sqlite3Fts3OpenTokenizer(sqlite3_tokenizer *, int, const char *, int,
  sqlite3_tokenizer_cursor **
);

/* fts3_aux.c */
SQLITE_PRIVATE int sqlite3Fts3InitAux(sqlite3 *db);

SQLITE_PRIVATE void sqlite3Fts3EvalPhraseCleanup(Fts3Phrase *);

SQLITE_PRIVATE int sqlite3Fts3MsrIncrStart(
    Fts3Table*, Fts3MultiSegReader*, int, const char*, int);
SQLITE_PRIVATE int sqlite3Fts3MsrIncrNext(
    Fts3Table *, Fts3MultiSegReader *, sqlite3_int64 *, char **, int *);
SQLITE_PRIVATE int sqlite3Fts3EvalPhrasePoslist(Fts3Cursor *, Fts3Expr *, int iCol, char **); 
SQLITE_PRIVATE int sqlite3Fts3MsrOvfl(Fts3Cursor *, Fts3MultiSegReader *, int *);
SQLITE_PRIVATE int sqlite3Fts3MsrIncrRestart(Fts3MultiSegReader *pCsr);

/* fts3_tokenize_vtab.c */
SQLITE_PRIVATE int sqlite3Fts3InitTok(sqlite3*, Fts3Hash *);

/* fts3_unicode2.c (functions generated by parsing unicode text files) */
#ifndef SQLITE_DISABLE_FTS3_UNICODE
SQLITE_PRIVATE int sqlite3FtsUnicodeFold(int, int);
SQLITE_PRIVATE int sqlite3FtsUnicodeIsalnum(int);
SQLITE_PRIVATE int sqlite3FtsUnicodeIsdiacritic(int);
#endif

#endif /* !SQLITE_CORE || SQLITE_ENABLE_FTS3 */
#endif /* _FTSINT_H */

/************** End of fts3Int.h *********************************************/
/************** Continuing where we left off in fts3.c ***********************/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

#if defined(SQLITE_ENABLE_FTS3) && !defined(SQLITE_CORE)
# define SQLITE_CORE 1
#endif

/* #include <assert.h> */
/* #include <stdlib.h> */
/* #include <stddef.h> */
/* #include <stdio.h> */
/* #include <string.h> */
/* #include <stdarg.h> */

#ifndef SQLITE_CORE 
  SQLITE_EXTENSION_INIT1
#endif

static int fts3EvalNext(Fts3Cursor *pCsr);
static int fts3EvalStart(Fts3Cursor *pCsr);
static int fts3TermSegReaderCursor(
    Fts3Cursor *, const char *, int, int, Fts3MultiSegReader **);

/* 
** Write a 64-bit variable-length integer to memory starting at p[0].
** The length of data written will be between 1 and FTS3_VARINT_MAX bytes.
** The number of bytes written is returned.
*/
SQLITE_PRIVATE int sqlite3Fts3PutVarint(char *p, sqlite_int64 v){
  unsigned char *q = (unsigned char *) p;
  sqlite_uint64 vu = v;
  do{
    *q++ = (unsigned char) ((vu & 0x7f) | 0x80);
    vu >>= 7;
  }while( vu!=0 );
  q[-1] &= 0x7f;  /* turn off high bit in final byte */
  assert( q - (unsigned char *)p <= FTS3_VARINT_MAX );
  return (int) (q - (unsigned char *)p);
}

#define GETVARINT_STEP(v, ptr, shift, mask1, mask2, var, ret) \
  v = (v & mask1) | ( (*ptr++) << shift );                    \
  if( (v & mask2)==0 ){ var = v; return ret; }
#define GETVARINT_INIT(v, ptr, shift, mask1, mask2, var, ret) \
  v = (*ptr++);                                               \
  if( (v & mask2)==0 ){ var = v; return ret; }

/* 
** Read a 64-bit variable-length integer from memory starting at p[0].
** Return the number of bytes read, or 0 on error.
** The value is stored in *v.
*/
SQLITE_PRIVATE int sqlite3Fts3GetVarint(const char *p, sqlite_int64 *v){
  const char *pStart = p;
  u32 a;
  u64 b;
  int shift;

  GETVARINT_INIT(a, p, 0,  0x00,     0x80, *v, 1);
  GETVARINT_STEP(a, p, 7,  0x7F,     0x4000, *v, 2);
  GETVARINT_STEP(a, p, 14, 0x3FFF,   0x200000, *v, 3);
  GETVARINT_STEP(a, p, 21, 0x1FFFFF, 0x10000000, *v, 4);
  b = (a & 0x0FFFFFFF );

  for(shift=28; shift<=63; shift+=7){
    u64 c = *p++;
    b += (c&0x7F) << shift;
    if( (c & 0x80)==0 ) break;
  }
  *v = b;
  return (int)(p - pStart);
}

/*
** Similar to sqlite3Fts3GetVarint(), except that the output is truncated to a
** 32-bit integer before it is returned.
*/
SQLITE_PRIVATE int sqlite3Fts3GetVarint32(const char *p, int *pi){
  u32 a;

#ifndef fts3GetVarint32
  GETVARINT_INIT(a, p, 0,  0x00,     0x80, *pi, 1);
#else
  a = (*p++);
  assert( a & 0x80 );
#endif

  GETVARINT_STEP(a, p, 7,  0x7F,     0x4000, *pi, 2);
  GETVARINT_STEP(a, p, 14, 0x3FFF,   0x200000, *pi, 3);
  GETVARINT_STEP(a, p, 21, 0x1FFFFF, 0x10000000, *pi, 4);
  a = (a & 0x0FFFFFFF );
  *pi = (int)(a | ((u32)(*p & 0x0F) << 28));
  return 5;
}

/*
** Return the number of bytes required to encode v as a varint
*/
SQLITE_PRIVATE int sqlite3Fts3VarintLen(sqlite3_uint64 v){
  int i = 0;
  do{
    i++;
    v >>= 7;
  }while( v!=0 );
  return i;
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
**
*/
SQLITE_PRIVATE void sqlite3Fts3Dequote(char *z){
  char quote;                     /* Quote character (if any ) */

  quote = z[0];
  if( quote=='[' || quote=='\'' || quote=='"' || quote=='`' ){
    int iIn = 1;                  /* Index of next byte to read from input */
    int iOut = 0;                 /* Index of next byte to write to output */

    /* If the first byte was a '[', then the close-quote character is a ']' */
    if( quote=='[' ) quote = ']';  

    while( ALWAYS(z[iIn]) ){
      if( z[iIn]==quote ){
        if( z[iIn+1]!=quote ) break;
        z[iOut++] = quote;
        iIn += 2;
      }else{
        z[iOut++] = z[iIn++];
      }
    }
    z[iOut] = '\0';
  }
}

/*
** Read a single varint from the doclist at *pp and advance *pp to point
** to the first byte past the end of the varint.  Add the value of the varint
** to *pVal.
*/
static void fts3GetDeltaVarint(char **pp, sqlite3_int64 *pVal){
  sqlite3_int64 iVal;
  *pp += sqlite3Fts3GetVarint(*pp, &iVal);
  *pVal += iVal;
}

/*
** When this function is called, *pp points to the first byte following a
** varint that is part of a doclist (or position-list, or any other list
** of varints). This function moves *pp to point to the start of that varint,
** and sets *pVal by the varint value.
**
** Argument pStart points to the first byte of the doclist that the
** varint is part of.
*/
static void fts3GetReverseVarint(
  char **pp, 
  char *pStart, 
  sqlite3_int64 *pVal
){
  sqlite3_int64 iVal;
  char *p;

  /* Pointer p now points at the first byte past the varint we are 
  ** interested in. So, unless the doclist is corrupt, the 0x80 bit is
  ** clear on character p[-1]. */
  for(p = (*pp)-2; p>=pStart && *p&0x80; p--);
  p++;
  *pp = p;

  sqlite3Fts3GetVarint(p, &iVal);
  *pVal = iVal;
}

/*
** The xDisconnect() virtual table method.
*/
static int fts3DisconnectMethod(sqlite3_vtab *pVtab){
  Fts3Table *p = (Fts3Table *)pVtab;
  int i;

  assert( p->nPendingData==0 );
  assert( p->pSegments==0 );

  /* Free any prepared statements held */
  for(i=0; i<SizeofArray(p->aStmt); i++){
    sqlite3_finalize(p->aStmt[i]);
  }
  sqlite3_free(p->zSegmentsTbl);
  sqlite3_free(p->zReadExprlist);
  sqlite3_free(p->zWriteExprlist);
  sqlite3_free(p->zContentTbl);
  sqlite3_free(p->zLanguageid);

  /* Invoke the tokenizer destructor to free the tokenizer. */
  p->pTokenizer->pModule->xDestroy(p->pTokenizer);

  sqlite3_free(p);
  return SQLITE_OK;
}

/*
** Construct one or more SQL statements from the format string given
** and then evaluate those statements. The success code is written
** into *pRc.
**
** If *pRc is initially non-zero then this routine is a no-op.
*/
static void fts3DbExec(
  int *pRc,              /* Success code */
  sqlite3 *db,           /* Database in which to run SQL */
  const char *zFormat,   /* Format string for SQL */
  ...                    /* Arguments to the format string */
){
  va_list ap;
  char *zSql;
  if( *pRc ) return;
  va_start(ap, zFormat);
  zSql = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
  if( zSql==0 ){
    *pRc = SQLITE_NOMEM;
  }else{
    *pRc = sqlite3_exec(db, zSql, 0, 0, 0);
    sqlite3_free(zSql);
  }
}

/*
** The xDestroy() virtual table method.
*/
static int fts3DestroyMethod(sqlite3_vtab *pVtab){
  Fts3Table *p = (Fts3Table *)pVtab;
  int rc = SQLITE_OK;              /* Return code */
  const char *zDb = p->zDb;        /* Name of database (e.g. "main", "temp") */
  sqlite3 *db = p->db;             /* Database handle */

  /* Drop the shadow tables */
  if( p->zContentTbl==0 ){
    fts3DbExec(&rc, db, "DROP TABLE IF EXISTS %Q.'%q_content'", zDb, p->zName);
  }
  fts3DbExec(&rc, db, "DROP TABLE IF EXISTS %Q.'%q_segments'", zDb,p->zName);
  fts3DbExec(&rc, db, "DROP TABLE IF EXISTS %Q.'%q_segdir'", zDb, p->zName);
  fts3DbExec(&rc, db, "DROP TABLE IF EXISTS %Q.'%q_docsize'", zDb, p->zName);
  fts3DbExec(&rc, db, "DROP TABLE IF EXISTS %Q.'%q_stat'", zDb, p->zName);

  /* If everything has worked, invoke fts3DisconnectMethod() to free the
  ** memory associated with the Fts3Table structure and return SQLITE_OK.
  ** Otherwise, return an SQLite error code.
  */
  return (rc==SQLITE_OK ? fts3DisconnectMethod(pVtab) : rc);
}


/*
** Invoke sqlite3_declare_vtab() to declare the schema for the FTS3 table
** passed as the first argument. This is done as part of the xConnect()
** and xCreate() methods.
**
** If *pRc is non-zero when this function is called, it is a no-op. 
** Otherwise, if an error occurs, an SQLite error code is stored in *pRc
** before returning.
*/
static void fts3DeclareVtab(int *pRc, Fts3Table *p){
  if( *pRc==SQLITE_OK ){
    int i;                        /* Iterator variable */
    int rc;                       /* Return code */
    char *zSql;                   /* SQL statement passed to declare_vtab() */
    char *zCols;                  /* List of user defined columns */
    const char *zLanguageid;

    zLanguageid = (p->zLanguageid ? p->zLanguageid : "__langid");
    sqlite3_vtab_config(p->db, SQLITE_VTAB_CONSTRAINT_SUPPORT, 1);

    /* Create a list of user columns for the virtual table */
    zCols = sqlite3_mprintf("%Q, ", p->azColumn[0]);
    for(i=1; zCols && i<p->nColumn; i++){
      zCols = sqlite3_mprintf("%z%Q, ", zCols, p->azColumn[i]);
    }

    /* Create the whole "CREATE TABLE" statement to pass to SQLite */
    zSql = sqlite3_mprintf(
        "CREATE TABLE x(%s %Q HIDDEN, docid HIDDEN, %Q HIDDEN)", 
        zCols, p->zName, zLanguageid
    );
    if( !zCols || !zSql ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlite3_declare_vtab(p->db, zSql);
    }

    sqlite3_free(zSql);
    sqlite3_free(zCols);
    *pRc = rc;
  }
}

/*
** Create the %_stat table if it does not already exist.
*/
SQLITE_PRIVATE void sqlite3Fts3CreateStatTable(int *pRc, Fts3Table *p){
  fts3DbExec(pRc, p->db, 
      "CREATE TABLE IF NOT EXISTS %Q.'%q_stat'"
          "(id INTEGER PRIMARY KEY, value BLOB);",
      p->zDb, p->zName
  );
  if( (*pRc)==SQLITE_OK ) p->bHasStat = 1;
}

/*
** Create the backing store tables (%_content, %_segments and %_segdir)
** required by the FTS3 table passed as the only argument. This is done
** as part of the vtab xCreate() method.
**
** If the p->bHasDocsize boolean is true (indicating that this is an
** FTS4 table, not an FTS3 table) then also create the %_docsize and
** %_stat tables required by FTS4.
*/
static int fts3CreateTables(Fts3Table *p){
  int rc = SQLITE_OK;             /* Return code */
  int i;                          /* Iterator variable */
  sqlite3 *db = p->db;            /* The database connection */

  if( p->zContentTbl==0 ){
    const char *zLanguageid = p->zLanguageid;
    char *zContentCols;           /* Columns of %_content table */

    /* Create a list of user columns for the content table */
    zContentCols = sqlite3_mprintf("docid INTEGER PRIMARY KEY");
    for(i=0; zContentCols && i<p->nColumn; i++){
      char *z = p->azColumn[i];
      zContentCols = sqlite3_mprintf("%z, 'c%d%q'", zContentCols, i, z);
    }
    if( zLanguageid && zContentCols ){
      zContentCols = sqlite3_mprintf("%z, langid", zContentCols, zLanguageid);
    }
    if( zContentCols==0 ) rc = SQLITE_NOMEM;
  
    /* Create the content table */
    fts3DbExec(&rc, db, 
       "CREATE TABLE %Q.'%q_content'(%s)",
       p->zDb, p->zName, zContentCols
    );
    sqlite3_free(zContentCols);
  }

  /* Create other tables */
  fts3DbExec(&rc, db, 
      "CREATE TABLE %Q.'%q_segments'(blockid INTEGER PRIMARY KEY, block BLOB);",
      p->zDb, p->zName
  );
  fts3DbExec(&rc, db, 
      "CREATE TABLE %Q.'%q_segdir'("
        "level INTEGER,"
        "idx INTEGER,"
        "start_block INTEGER,"
        "leaves_end_block INTEGER,"
        "end_block INTEGER,"
        "root BLOB,"
        "PRIMARY KEY(level, idx)"
      ");",
      p->zDb, p->zName
  );
  if( p->bHasDocsize ){
    fts3DbExec(&rc, db, 
        "CREATE TABLE %Q.'%q_docsize'(docid INTEGER PRIMARY KEY, size BLOB);",
        p->zDb, p->zName
    );
  }
  assert( p->bHasStat==p->bFts4 );
  if( p->bHasStat ){
    sqlite3Fts3CreateStatTable(&rc, p);
  }
  return rc;
}

/*
** Store the current database page-size in bytes in p->nPgsz.
**
** If *pRc is non-zero when this function is called, it is a no-op. 
** Otherwise, if an error occurs, an SQLite error code is stored in *pRc
** before returning.
*/
static void fts3DatabasePageSize(int *pRc, Fts3Table *p){
  if( *pRc==SQLITE_OK ){
    int rc;                       /* Return code */
    char *zSql;                   /* SQL text "PRAGMA %Q.page_size" */
    sqlite3_stmt *pStmt;          /* Compiled "PRAGMA %Q.page_size" statement */
  
    zSql = sqlite3_mprintf("PRAGMA %Q.page_size", p->zDb);
    if( !zSql ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlite3_prepare(p->db, zSql, -1, &pStmt, 0);
      if( rc==SQLITE_OK ){
        sqlite3_step(pStmt);
        p->nPgsz = sqlite3_column_int(pStmt, 0);
        rc = sqlite3_finalize(pStmt);
      }else if( rc==SQLITE_AUTH ){
        p->nPgsz = 1024;
        rc = SQLITE_OK;
      }
    }
    assert( p->nPgsz>0 || rc!=SQLITE_OK );
    sqlite3_free(zSql);
    *pRc = rc;
  }
}

/*
** "Special" FTS4 arguments are column specifications of the following form:
**
**   <key> = <value>
**
** There may not be whitespace surrounding the "=" character. The <value> 
** term may be quoted, but the <key> may not.
*/
static int fts3IsSpecialColumn(
  const char *z, 
  int *pnKey,
  char **pzValue
){
  char *zValue;
  const char *zCsr = z;

  while( *zCsr!='=' ){
    if( *zCsr=='\0' ) return 0;
    zCsr++;
  }

  *pnKey = (int)(zCsr-z);
  zValue = sqlite3_mprintf("%s", &zCsr[1]);
  if( zValue ){
    sqlite3Fts3Dequote(zValue);
  }
  *pzValue = zValue;
  return 1;
}

/*
** Append the output of a printf() style formatting to an existing string.
*/
static void fts3Appendf(
  int *pRc,                       /* IN/OUT: Error code */
  char **pz,                      /* IN/OUT: Pointer to string buffer */
  const char *zFormat,            /* Printf format string to append */
  ...                             /* Arguments for printf format string */
){
  if( *pRc==SQLITE_OK ){
    va_list ap;
    char *z;
    va_start(ap, zFormat);
    z = sqlite3_vmprintf(zFormat, ap);
    va_end(ap);
    if( z && *pz ){
      char *z2 = sqlite3_mprintf("%s%s", *pz, z);
      sqlite3_free(z);
      z = z2;
    }
    if( z==0 ) *pRc = SQLITE_NOMEM;
    sqlite3_free(*pz);
    *pz = z;
  }
}

/*
** Return a copy of input string zInput enclosed in double-quotes (") and
** with all double quote characters escaped. For example:
**
**     fts3QuoteId("un \"zip\"")   ->    "un \"\"zip\"\""
**
** The pointer returned points to memory obtained from sqlite3_malloc(). It
** is the callers responsibility to call sqlite3_free() to release this
** memory.
*/
static char *fts3QuoteId(char const *zInput){
  int nRet;
  char *zRet;
  nRet = 2 + (int)strlen(zInput)*2 + 1;
  zRet = sqlite3_malloc(nRet);
  if( zRet ){
    int i;
    char *z = zRet;
    *(z++) = '"';
    for(i=0; zInput[i]; i++){
      if( zInput[i]=='"' ) *(z++) = '"';
      *(z++) = zInput[i];
    }
    *(z++) = '"';
    *(z++) = '\0';
  }
  return zRet;
}

/*
** Return a list of comma separated SQL expressions and a FROM clause that 
** could be used in a SELECT statement such as the following:
**
**     SELECT <list of expressions> FROM %_content AS x ...
**
** to return the docid, followed by each column of text data in order
** from left to write. If parameter zFunc is not NULL, then instead of
** being returned directly each column of text data is passed to an SQL
** function named zFunc first. For example, if zFunc is "unzip" and the
** table has the three user-defined columns "a", "b", and "c", the following
** string is returned:
**
**     "docid, unzip(x.'a'), unzip(x.'b'), unzip(x.'c') FROM %_content AS x"
**
** The pointer returned points to a buffer allocated by sqlite3_malloc(). It
** is the responsibility of the caller to eventually free it.
**
** If *pRc is not SQLITE_OK when this function is called, it is a no-op (and
** a NULL pointer is returned). Otherwise, if an OOM error is encountered
** by this function, NULL is returned and *pRc is set to SQLITE_NOMEM. If
** no error occurs, *pRc is left unmodified.
*/
static char *fts3ReadExprList(Fts3Table *p, const char *zFunc, int *pRc){
  char *zRet = 0;
  char *zFree = 0;
  char *zFunction;
  int i;

  if( p->zContentTbl==0 ){
    if( !zFunc ){
      zFunction = "";
    }else{
      zFree = zFunction = fts3QuoteId(zFunc);
    }
    fts3Appendf(pRc, &zRet, "docid");
    for(i=0; i<p->nColumn; i++){
      fts3Appendf(pRc, &zRet, ",%s(x.'c%d%q')", zFunction, i, p->azColumn[i]);
    }
    if( p->zLanguageid ){
      fts3Appendf(pRc, &zRet, ", x.%Q", "langid");
    }
    sqlite3_free(zFree);
  }else{
    fts3Appendf(pRc, &zRet, "rowid");
    for(i=0; i<p->nColumn; i++){
      fts3Appendf(pRc, &zRet, ", x.'%q'", p->azColumn[i]);
    }
    if( p->zLanguageid ){
      fts3Appendf(pRc, &zRet, ", x.%Q", p->zLanguageid);
    }
  }
  fts3Appendf(pRc, &zRet, " FROM '%q'.'%q%s' AS x", 
      p->zDb,
      (p->zContentTbl ? p->zContentTbl : p->zName),
      (p->zContentTbl ? "" : "_content")
  );
  return zRet;
}

/*
** Return a list of N comma separated question marks, where N is the number
** of columns in the %_content table (one for the docid plus one for each
** user-defined text column).
**
** If argument zFunc is not NULL, then all but the first question mark
** is preceded by zFunc and an open bracket, and followed by a closed
** bracket. For example, if zFunc is "zip" and the FTS3 table has three 
** user-defined text columns, the following string is returned:
**
**     "?, zip(?), zip(?), zip(?)"
**
** The pointer returned points to a buffer allocated by sqlite3_malloc(). It
** is the responsibility of the caller to eventually free it.
**
** If *pRc is not SQLITE_OK when this function is called, it is a no-op (and
** a NULL pointer is returned). Otherwise, if an OOM error is encountered
** by this function, NULL is returned and *pRc is set to SQLITE_NOMEM. If
** no error occurs, *pRc is left unmodified.
*/
static char *fts3WriteExprList(Fts3Table *p, const char *zFunc, int *pRc){
  char *zRet = 0;
  char *zFree = 0;
  char *zFunction;
  int i;

  if( !zFunc ){
    zFunction = "";
  }else{
    zFree = zFunction = fts3QuoteId(zFunc);
  }
  fts3Appendf(pRc, &zRet, "?");
  for(i=0; i<p->nColumn; i++){
    fts3Appendf(pRc, &zRet, ",%s(?)", zFunction);
  }
  if( p->zLanguageid ){
    fts3Appendf(pRc, &zRet, ", ?");
  }
  sqlite3_free(zFree);
  return zRet;
}

/*
** This function interprets the string at (*pp) as a non-negative integer
** value. It reads the integer and sets *pnOut to the value read, then 
** sets *pp to point to the byte immediately following the last byte of
** the integer value.
**
** Only decimal digits ('0'..'9') may be part of an integer value. 
**
** If *pp does not being with a decimal digit SQLITE_ERROR is returned and
** the output value undefined. Otherwise SQLITE_OK is returned.
**
** This function is used when parsing the "prefix=" FTS4 parameter.
*/
static int fts3GobbleInt(const char **pp, int *pnOut){
  const char *p;                  /* Iterator pointer */
  int nInt = 0;                   /* Output value */

  for(p=*pp; p[0]>='0' && p[0]<='9'; p++){
    nInt = nInt * 10 + (p[0] - '0');
  }
  if( p==*pp ) return SQLITE_ERROR;
  *pnOut = nInt;
  *pp = p;
  return SQLITE_OK;
}

/*
** This function is called to allocate an array of Fts3Index structures
** representing the indexes maintained by the current FTS table. FTS tables
** always maintain the main "terms" index, but may also maintain one or
** more "prefix" indexes, depending on the value of the "prefix=" parameter
** (if any) specified as part of the CREATE VIRTUAL TABLE statement.
**
** Argument zParam is passed the value of the "prefix=" option if one was
** specified, or NULL otherwise.
**
** If no error occurs, SQLITE_OK is returned and *apIndex set to point to
** the allocated array. *pnIndex is set to the number of elements in the
** array. If an error does occur, an SQLite error code is returned.
**
** Regardless of whether or not an error is returned, it is the responsibility
** of the caller to call sqlite3_free() on the output array to free it.
*/
static int fts3PrefixParameter(
  const char *zParam,             /* ABC in prefix=ABC parameter to parse */
  int *pnIndex,                   /* OUT: size of *apIndex[] array */
  struct Fts3Index **apIndex      /* OUT: Array of indexes for this table */
){
  struct Fts3Index *aIndex;       /* Allocated array */
  int nIndex = 1;                 /* Number of entries in array */

  if( zParam && zParam[0] ){
    const char *p;
    nIndex++;
    for(p=zParam; *p; p++){
      if( *p==',' ) nIndex++;
    }
  }

  aIndex = sqlite3_malloc(sizeof(struct Fts3Index) * nIndex);
  *apIndex = aIndex;
  *pnIndex = nIndex;
  if( !aIndex ){
    return SQLITE_NOMEM;
  }

  memset(aIndex, 0, sizeof(struct Fts3Index) * nIndex);
  if( zParam ){
    const char *p = zParam;
    int i;
    for(i=1; i<nIndex; i++){
      int nPrefix;
      if( fts3GobbleInt(&p, &nPrefix) ) return SQLITE_ERROR;
      aIndex[i].nPrefix = nPrefix;
      p++;
    }
  }

  return SQLITE_OK;
}

/*
** This function is called when initializing an FTS4 table that uses the
** content=xxx option. It determines the number of and names of the columns
** of the new FTS4 table.
**
** The third argument passed to this function is the value passed to the
** config=xxx option (i.e. "xxx"). This function queries the database for
** a table of that name. If found, the output variables are populated
** as follows:
**
**   *pnCol:   Set to the number of columns table xxx has,
**
**   *pnStr:   Set to the total amount of space required to store a copy
**             of each columns name, including the nul-terminator.
**
**   *pazCol:  Set to point to an array of *pnCol strings. Each string is
**             the name of the corresponding column in table xxx. The array
**             and its contents are allocated using a single allocation. It
**             is the responsibility of the caller to free this allocation
**             by eventually passing the *pazCol value to sqlite3_free().
**
** If the table cannot be found, an error code is returned and the output
** variables are undefined. Or, if an OOM is encountered, SQLITE_NOMEM is
** returned (and the output variables are undefined).
*/
static int fts3ContentColumns(
  sqlite3 *db,                    /* Database handle */
  const char *zDb,                /* Name of db (i.e. "main", "temp" etc.) */
  const char *zTbl,               /* Name of content table */
  const char ***pazCol,           /* OUT: Malloc'd array of column names */
  int *pnCol,                     /* OUT: Size of array *pazCol */
  int *pnStr                      /* OUT: Bytes of string content */
){
  int rc = SQLITE_OK;             /* Return code */
  char *zSql;                     /* "SELECT *" statement on zTbl */  
  sqlite3_stmt *pStmt = 0;        /* Compiled version of zSql */

  zSql = sqlite3_mprintf("SELECT * FROM %Q.%Q", zDb, zTbl);
  if( !zSql ){
    rc = SQLITE_NOMEM;
  }else{
    rc = sqlite3_prepare(db, zSql, -1, &pStmt, 0);
  }
  sqlite3_free(zSql);

  if( rc==SQLITE_OK ){
    const char **azCol;           /* Output array */
    int nStr = 0;                 /* Size of all column names (incl. 0x00) */
    int nCol;                     /* Number of table columns */
    int i;                        /* Used to iterate through columns */

    /* Loop through the returned columns. Set nStr to the number of bytes of
    ** space required to store a copy of each column name, including the
    ** nul-terminator byte.  */
    nCol = sqlite3_column_count(pStmt);
    for(i=0; i<nCol; i++){
      const char *zCol = sqlite3_column_name(pStmt, i);
      nStr += (int)strlen(zCol) + 1;
    }

    /* Allocate and populate the array to return. */
    azCol = (const char **)sqlite3_malloc(sizeof(char *) * nCol + nStr);
    if( azCol==0 ){
      rc = SQLITE_NOMEM;
    }else{
      char *p = (char *)&azCol[nCol];
      for(i=0; i<nCol; i++){
        const char *zCol = sqlite3_column_name(pStmt, i);
        int n = (int)strlen(zCol)+1;
        memcpy(p, zCol, n);
        azCol[i] = p;
        p += n;
      }
    }
    sqlite3_finalize(pStmt);

    /* Set the output variables. */
    *pnCol = nCol;
    *pnStr = nStr;
    *pazCol = azCol;
  }

  return rc;
}

/*
** This function is the implementation of both the xConnect and xCreate
** methods of the FTS3 virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fts3" or "fts4")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> "column name" and other module argument fields.
*/
static int fts3InitVtab(
  int isCreate,                   /* True for xCreate, false for xConnect */
  sqlite3 *db,                    /* The SQLite database connection */
  void *pAux,                     /* Hash table containing tokenizers */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVTab,          /* Write the resulting vtab structure here */
  char **pzErr                    /* Write any error message here */
){
  Fts3Hash *pHash = (Fts3Hash *)pAux;
  Fts3Table *p = 0;               /* Pointer to allocated vtab */
  int rc = SQLITE_OK;             /* Return code */
  int i;                          /* Iterator variable */
  int nByte;                      /* Size of allocation used for *p */
  int iCol;                       /* Column index */
  int nString = 0;                /* Bytes required to hold all column names */
  int nCol = 0;                   /* Number of columns in the FTS table */
  char *zCsr;                     /* Space for holding column names */
  int nDb;                        /* Bytes required to hold database name */
  int nName;                      /* Bytes required to hold table name */
  int isFts4 = (argv[0][3]=='4'); /* True for FTS4, false for FTS3 */
  const char **aCol;              /* Array of column names */
  sqlite3_tokenizer *pTokenizer = 0;        /* Tokenizer for this table */

  int nIndex;                     /* Size of aIndex[] array */
  struct Fts3Index *aIndex = 0;   /* Array of indexes for this table */

  /* The results of parsing supported FTS4 key=value options: */
  int bNoDocsize = 0;             /* True to omit %_docsize table */
  int bDescIdx = 0;               /* True to store descending indexes */
  char *zPrefix = 0;              /* Prefix parameter value (or NULL) */
  char *zCompress = 0;            /* compress=? parameter (or NULL) */
  char *zUncompress = 0;          /* uncompress=? parameter (or NULL) */
  char *zContent = 0;             /* content=? parameter (or NULL) */
  char *zLanguageid = 0;          /* languageid=? parameter (or NULL) */
  char **azNotindexed = 0;        /* The set of notindexed= columns */
  int nNotindexed = 0;            /* Size of azNotindexed[] array */

  assert( strlen(argv[0])==4 );
  assert( (sqlite3_strnicmp(argv[0], "fts4", 4)==0 && isFts4)
       || (sqlite3_strnicmp(argv[0], "fts3", 4)==0 && !isFts4)
  );

  nDb = (int)strlen(argv[1]) + 1;
  nName = (int)strlen(argv[2]) + 1;

  nByte = sizeof(const char *) * (argc-2);
  aCol = (const char **)sqlite3_malloc(nByte);
  if( aCol ){
    memset((void*)aCol, 0, nByte);
    azNotindexed = (char **)sqlite3_malloc(nByte);
  }
  if( azNotindexed ){
    memset(azNotindexed, 0, nByte);
  }
  if( !aCol || !azNotindexed ){
    rc = SQLITE_NOMEM;
    goto fts3_init_out;
  }

  /* Loop through all of the arguments passed by the user to the FTS3/4
  ** module (i.e. all the column names and special arguments). This loop
  ** does the following:
  **
  **   + Figures out the number of columns the FTSX table will have, and
  **     the number of bytes of space that must be allocated to store copies
  **     of the column names.
  **
  **   + If there is a tokenizer specification included in the arguments,
  **     initializes the tokenizer pTokenizer.
  */
  for(i=3; rc==SQLITE_OK && i<argc; i++){
    char const *z = argv[i];
    int nKey;
    char *zVal;

    /* Check if this is a tokenizer specification */
    if( !pTokenizer 
     && strlen(z)>8
     && 0==sqlite3_strnicmp(z, "tokenize", 8) 
     && 0==sqlite3Fts3IsIdChar(z[8])
    ){
      rc = sqlite3Fts3InitTokenizer(pHash, &z[9], &pTokenizer, pzErr);
    }

    /* Check if it is an FTS4 special argument. */
    else if( isFts4 && fts3IsSpecialColumn(z, &nKey, &zVal) ){
      struct Fts4Option {
        const char *zOpt;
        int nOpt;
      } aFts4Opt[] = {
        { "matchinfo",   9 },     /* 0 -> MATCHINFO */
        { "prefix",      6 },     /* 1 -> PREFIX */
        { "compress",    8 },     /* 2 -> COMPRESS */
        { "uncompress", 10 },     /* 3 -> UNCOMPRESS */
        { "order",       5 },     /* 4 -> ORDER */
        { "content",     7 },     /* 5 -> CONTENT */
        { "languageid", 10 },     /* 6 -> LANGUAGEID */
        { "notindexed", 10 }      /* 7 -> NOTINDEXED */
      };

      int iOpt;
      if( !zVal ){
        rc = SQLITE_NOMEM;
      }else{
        for(iOpt=0; iOpt<SizeofArray(aFts4Opt); iOpt++){
          struct Fts4Option *pOp = &aFts4Opt[iOpt];
          if( nKey==pOp->nOpt && !sqlite3_strnicmp(z, pOp->zOpt, pOp->nOpt) ){
            break;
          }
        }
        if( iOpt==SizeofArray(aFts4Opt) ){
          *pzErr = sqlite3_mprintf("unrecognized parameter: %s", z);
          rc = SQLITE_ERROR;
        }else{
          switch( iOpt ){
            case 0:               /* MATCHINFO */
              if( strlen(zVal)!=4 || sqlite3_strnicmp(zVal, "fts3", 4) ){
                *pzErr = sqlite3_mprintf("unrecognized matchinfo: %s", zVal);
                rc = SQLITE_ERROR;
              }
              bNoDocsize = 1;
              break;

            case 1:               /* PREFIX */
              sqlite3_free(zPrefix);
              zPrefix = zVal;
              zVal = 0;
              break;

            case 2:               /* COMPRESS */
              sqlite3_free(zCompress);
              zCompress = zVal;
              zVal = 0;
              break;

            case 3:               /* UNCOMPRESS */
              sqlite3_free(zUncompress);
              zUncompress = zVal;
              zVal = 0;
              break;

            case 4:               /* ORDER */
              if( (strlen(zVal)!=3 || sqlite3_strnicmp(zVal, "asc", 3)) 
               && (strlen(zVal)!=4 || sqlite3_strnicmp(zVal, "desc", 4)) 
              ){
                *pzErr = sqlite3_mprintf("unrecognized order: %s", zVal);
                rc = SQLITE_ERROR;
              }
              bDescIdx = (zVal[0]=='d' || zVal[0]=='D');
              break;

            case 5:              /* CONTENT */
              sqlite3_free(zContent);
              zContent = zVal;
              zVal = 0;
              break;

            case 6:              /* LANGUAGEID */
              assert( iOpt==6 );
              sqlite3_free(zLanguageid);
              zLanguageid = zVal;
              zVal = 0;
              break;

            case 7:              /* NOTINDEXED */
              azNotindexed[nNotindexed++] = zVal;
              zVal = 0;
              break;
          }
        }
        sqlite3_free(zVal);
      }
    }

    /* Otherwise, the argument is a column name. */
    else {
      nString += (int)(strlen(z) + 1);
      aCol[nCol++] = z;
    }
  }

  /* If a content=xxx option was specified, the following:
  **
  **   1. Ignore any compress= and uncompress= options.
  **
  **   2. If no column names were specified as part of the CREATE VIRTUAL
  **      TABLE statement, use all columns from the content table.
  */
  if( rc==SQLITE_OK && zContent ){
    sqlite3_free(zCompress); 
    sqlite3_free(zUncompress); 
    zCompress = 0;
    zUncompress = 0;
    if( nCol==0 ){
      sqlite3_free((void*)aCol); 
      aCol = 0;
      rc = fts3ContentColumns(db, argv[1], zContent, &aCol, &nCol, &nString);

      /* If a languageid= option was specified, remove the language id
      ** column from the aCol[] array. */ 
      if( rc==SQLITE_OK && zLanguageid ){
        int j;
        for(j=0; j<nCol; j++){
          if( sqlite3_stricmp(zLanguageid, aCol[j])==0 ){
            int k;
            for(k=j; k<nCol; k++) aCol[k] = aCol[k+1];
            nCol--;
            break;
          }
        }
      }
    }
  }
  if( rc!=SQLITE_OK ) goto fts3_init_out;

  if( nCol==0 ){
    assert( nString==0 );
    aCol[0] = "content";
    nString = 8;
    nCol = 1;
  }

  if( pTokenizer==0 ){
    rc = sqlite3Fts3InitTokenizer(pHash, "simple", &pTokenizer, pzErr);
    if( rc!=SQLITE_OK ) goto fts3_init_out;
  }
  assert( pTokenizer );

  rc = fts3PrefixParameter(zPrefix, &nIndex, &aIndex);
  if( rc==SQLITE_ERROR ){
    assert( zPrefix );
    *pzErr = sqlite3_mprintf("error parsing prefix parameter: %s", zPrefix);
  }
  if( rc!=SQLITE_OK ) goto fts3_init_out;

  /* Allocate and populate the Fts3Table structure. */
  nByte = sizeof(Fts3Table) +                  /* Fts3Table */
          nCol * sizeof(char *) +              /* azColumn */
          nIndex * sizeof(struct Fts3Index) +  /* aIndex */
          nCol * sizeof(u8) +                  /* abNotindexed */
          nName +                              /* zName */
          nDb +                                /* zDb */
          nString;                             /* Space for azColumn strings */
  p = (Fts3Table*)sqlite3_malloc(nByte);
  if( p==0 ){
    rc = SQLITE_NOMEM;
    goto fts3_init_out;
  }
  memset(p, 0, nByte);
  p->db = db;
  p->nColumn = nCol;
  p->nPendingData = 0;
  p->azColumn = (char **)&p[1];
  p->pTokenizer = pTokenizer;
  p->nMaxPendingData = FTS3_MAX_PENDING_DATA;
  p->bHasDocsize = (isFts4 && bNoDocsize==0);
  p->bHasStat = isFts4;
  p->bFts4 = isFts4;
  p->bDescIdx = bDescIdx;
  p->nAutoincrmerge = 0xff;   /* 0xff means setting unknown */
  p->zContentTbl = zContent;
  p->zLanguageid = zLanguageid;
  zContent = 0;
  zLanguageid = 0;
  TESTONLY( p->inTransaction = -1 );
  TESTONLY( p->mxSavepoint = -1 );

  p->aIndex = (struct Fts3Index *)&p->azColumn[nCol];
  memcpy(p->aIndex, aIndex, sizeof(struct Fts3Index) * nIndex);
  p->nIndex = nIndex;
  for(i=0; i<nIndex; i++){
    fts3HashInit(&p->aIndex[i].hPending, FTS3_HASH_STRING, 1);
  }
  p->abNotindexed = (u8 *)&p->aIndex[nIndex];

  /* Fill in the zName and zDb fields of the vtab structure. */
  zCsr = (char *)&p->abNotindexed[nCol];
  p->zName = zCsr;
  memcpy(zCsr, argv[2], nName);
  zCsr += nName;
  p->zDb = zCsr;
  memcpy(zCsr, argv[1], nDb);
  zCsr += nDb;

  /* Fill in the azColumn array */
  for(iCol=0; iCol<nCol; iCol++){
    char *z; 
    int n = 0;
    z = (char *)sqlite3Fts3NextToken(aCol[iCol], &n);
    memcpy(zCsr, z, n);
    zCsr[n] = '\0';
    sqlite3Fts3Dequote(zCsr);
    p->azColumn[iCol] = zCsr;
    zCsr += n+1;
    assert( zCsr <= &((char *)p)[nByte] );
  }

  /* Fill in the abNotindexed array */
  for(iCol=0; iCol<nCol; iCol++){
    int n = (int)strlen(p->azColumn[iCol]);
    for(i=0; i<nNotindexed; i++){
      char *zNot = azNotindexed[i];
      if( zNot && n==(int)strlen(zNot)
       && 0==sqlite3_strnicmp(p->azColumn[iCol], zNot, n) 
      ){
        p->abNotindexed[iCol] = 1;
        sqlite3_free(zNot);
        azNotindexed[i] = 0;
      }
    }
  }
  for(i=0; i<nNotindexed; i++){
    if( azNotindexed[i] ){
      *pzErr = sqlite3_mprintf("no such column: %s", azNotindexed[i]);
      rc = SQLITE_ERROR;
    }
  }

  if( rc==SQLITE_OK && (zCompress==0)!=(zUncompress==0) ){
    char const *zMiss = (zCompress==0 ? "compress" : "uncompress");
    rc = SQLITE_ERROR;
    *pzErr = sqlite3_mprintf("missing %s parameter in fts4 constructor", zMiss);
  }
  p->zReadExprlist = fts3ReadExprList(p, zUncompress, &rc);
  p->zWriteExprlist = fts3WriteExprList(p, zCompress, &rc);
  if( rc!=SQLITE_OK ) goto fts3_init_out;

  /* If this is an xCreate call, create the underlying tables in the 
  ** database. TODO: For xConnect(), it could verify that said tables exist.
  */
  if( isCreate ){
    rc = fts3CreateTables(p);
  }

  /* Check to see if a legacy fts3 table has been "upgraded" by the
  ** addition of a %_stat table so that it can use incremental merge.
  */
  if( !isFts4 && !isCreate ){
    p->bHasStat = 2;
  }

  /* Figure out the page-size for the database. This is required in order to
  ** estimate the cost of loading large doclists from the database.  */
  fts3DatabasePageSize(&rc, p);
  p->nNodeSize = p->nPgsz-35;

  /* Declare the table schema to SQLite. */
  fts3DeclareVtab(&rc, p);

fts3_init_out:
  sqlite3_free(zPrefix);
  sqlite3_free(aIndex);
  sqlite3_free(zCompress);
  sqlite3_free(zUncompress);
  sqlite3_free(zContent);
  sqlite3_free(zLanguageid);
  for(i=0; i<nNotindexed; i++) sqlite3_free(azNotindexed[i]);
  sqlite3_free((void *)aCol);
  sqlite3_free((void *)azNotindexed);
  if( rc!=SQLITE_OK ){
    if( p ){
      fts3DisconnectMethod((sqlite3_vtab *)p);
    }else if( pTokenizer ){
      pTokenizer->pModule->xDestroy(pTokenizer);
    }
  }else{
    assert( p->pSegments==0 );
    *ppVTab = &p->base;
  }
  return rc;
}

/*
** The xConnect() and xCreate() methods for the virtual table. All the
** work is done in function fts3InitVtab().
*/
static int fts3ConnectMethod(
  sqlite3 *db,                    /* Database connection */
  void *pAux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVtab,          /* OUT: New sqlite3_vtab object */
  char **pzErr                    /* OUT: sqlite3_malloc'd error message */
){
  return fts3InitVtab(0, db, pAux, argc, argv, ppVtab, pzErr);
}
static int fts3CreateMethod(
  sqlite3 *db,                    /* Database connection */
  void *pAux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVtab,          /* OUT: New sqlite3_vtab object */
  char **pzErr                    /* OUT: sqlite3_malloc'd error message */
){
  return fts3InitVtab(1, db, pAux, argc, argv, ppVtab, pzErr);
}

/*
** Set the pIdxInfo->estimatedRows variable to nRow. Unless this
** extension is currently being used by a version of SQLite too old to
** support estimatedRows. In that case this function is a no-op.
*/
static void fts3SetEstimatedRows(sqlite3_index_info *pIdxInfo, i64 nRow){
#if SQLITE_VERSION_NUMBER>=3008002
  if( sqlite3_libversion_number()>=3008002 ){
    pIdxInfo->estimatedRows = nRow;
  }
#endif
}

/* 
** Implementation of the xBestIndex method for FTS3 tables. There
** are three possible strategies, in order of preference:
**
**   1. Direct lookup by rowid or docid. 
**   2. Full-text search using a MATCH operator on a non-docid column.
**   3. Linear scan of %_content table.
*/
static int fts3BestIndexMethod(sqlite3_vtab *pVTab, sqlite3_index_info *pInfo){
  Fts3Table *p = (Fts3Table *)pVTab;
  int i;                          /* Iterator variable */
  int iCons = -1;                 /* Index of constraint to use */

  int iLangidCons = -1;           /* Index of langid=x constraint, if present */
  int iDocidGe = -1;              /* Index of docid>=x constraint, if present */
  int iDocidLe = -1;              /* Index of docid<=x constraint, if present */
  int iIdx;

  /* By default use a full table scan. This is an expensive option,
  ** so search through the constraints to see if a more efficient 
  ** strategy is possible.
  */
  pInfo->idxNum = FTS3_FULLSCAN_SEARCH;
  pInfo->estimatedCost = 5000000;
  for(i=0; i<pInfo->nConstraint; i++){
    int bDocid;                 /* True if this constraint is on docid */
    struct sqlite3_index_constraint *pCons = &pInfo->aConstraint[i];
    if( pCons->usable==0 ){
      if( pCons->op==SQLITE_INDEX_CONSTRAINT_MATCH ){
        /* There exists an unusable MATCH constraint. This means that if
        ** the planner does elect to use the results of this call as part
        ** of the overall query plan the user will see an "unable to use
        ** function MATCH in the requested context" error. To discourage
        ** this, return a very high cost here.  */
        pInfo->idxNum = FTS3_FULLSCAN_SEARCH;
        pInfo->estimatedCost = 1e50;
        fts3SetEstimatedRows(pInfo, ((sqlite3_int64)1) << 50);
        return SQLITE_OK;
      }
      continue;
    }

    bDocid = (pCons->iColumn<0 || pCons->iColumn==p->nColumn+1);

    /* A direct lookup on the rowid or docid column. Assign a cost of 1.0. */
    if( iCons<0 && pCons->op==SQLITE_INDEX_CONSTRAINT_EQ && bDocid ){
      pInfo->idxNum = FTS3_DOCID_SEARCH;
      pInfo->estimatedCost = 1.0;
      iCons = i;
    }

    /* A MATCH constraint. Use a full-text search.
    **
    ** If there is more than one MATCH constraint available, use the first
    ** one encountered. If there is both a MATCH constraint and a direct
    ** rowid/docid lookup, prefer the MATCH strategy. This is done even 
    ** though the rowid/docid lookup is faster than a MATCH query, selecting
    ** it would lead to an "unable to use function MATCH in the requested 
    ** context" error.
    */
    if( pCons->op==SQLITE_INDEX_CONSTRAINT_MATCH 
     && pCons->iColumn>=0 && pCons->iColumn<=p->nColumn
    ){
      pInfo->idxNum = FTS3_FULLTEXT_SEARCH + pCons->iColumn;
      pInfo->estimatedCost = 2.0;
      iCons = i;
    }

    /* Equality constraint on the langid column */
    if( pCons->op==SQLITE_INDEX_CONSTRAINT_EQ 
     && pCons->iColumn==p->nColumn + 2
    ){
      iLangidCons = i;
    }

    if( bDocid ){
      switch( pCons->op ){
        case SQLITE_INDEX_CONSTRAINT_GE:
        case SQLITE_INDEX_CONSTRAINT_GT:
          iDocidGe = i;
          break;

        case SQLITE_INDEX_CONSTRAINT_LE:
        case SQLITE_INDEX_CONSTRAINT_LT:
          iDocidLe = i;
          break;
      }
    }
  }

  iIdx = 1;
  if( iCons>=0 ){
    pInfo->aConstraintUsage[iCons].argvIndex = iIdx++;
    pInfo->aConstraintUsage[iCons].omit = 1;
  } 
  if( iLangidCons>=0 ){
    pInfo->idxNum |= FTS3_HAVE_LANGID;
    pInfo->aConstraintUsage[iLangidCons].argvIndex = iIdx++;
  } 
  if( iDocidGe>=0 ){
    pInfo->idxNum |= FTS3_HAVE_DOCID_GE;
    pInfo->aConstraintUsage[iDocidGe].argvIndex = iIdx++;
  } 
  if( iDocidLe>=0 ){
    pInfo->idxNum |= FTS3_HAVE_DOCID_LE;
    pInfo->aConstraintUsage[iDocidLe].argvIndex = iIdx++;
  } 

  /* Regardless of the strategy selected, FTS can deliver rows in rowid (or
  ** docid) order. Both ascending and descending are possible. 
  */
  if( pInfo->nOrderBy==1 ){
    struct sqlite3_index_orderby *pOrder = &pInfo->aOrderBy[0];
    if( pOrder->iColumn<0 || pOrder->iColumn==p->nColumn+1 ){
      if( pOrder->desc ){
        pInfo->idxStr = "DESC";
      }else{
        pInfo->idxStr = "ASC";
      }
      pInfo->orderByConsumed = 1;
    }
  }

  assert( p->pSegments==0 );
  return SQLITE_OK;
}

/*
** Implementation of xOpen method.
*/
static int fts3OpenMethod(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCsr){
  sqlite3_vtab_cursor *pCsr;               /* Allocated cursor */

  UNUSED_PARAMETER(pVTab);

  /* Allocate a buffer large enough for an Fts3Cursor structure. If the
  ** allocation succeeds, zero it and return SQLITE_OK. Otherwise, 
  ** if the allocation fails, return SQLITE_NOMEM.
  */
  *ppCsr = pCsr = (sqlite3_vtab_cursor *)sqlite3_malloc(sizeof(Fts3Cursor));
  if( !pCsr ){
    return SQLITE_NOMEM;
  }
  memset(pCsr, 0, sizeof(Fts3Cursor));
  return SQLITE_OK;
}

/*
** Close the cursor.  For additional information see the documentation
** on the xClose method of the virtual table interface.
*/
static int fts3CloseMethod(sqlite3_vtab_cursor *pCursor){
  Fts3Cursor *pCsr = (Fts3Cursor *)pCursor;
  assert( ((Fts3Table *)pCsr->base.pVtab)->pSegments==0 );
  sqlite3_finalize(pCsr->pStmt);
  sqlite3Fts3ExprFree(pCsr->pExpr);
  sqlite3Fts3FreeDeferredTokens(pCsr);
  sqlite3_free(pCsr->aDoclist);
  sqlite3_free(pCsr->aMatchinfo);
  assert( ((Fts3Table *)pCsr->base.pVtab)->pSegments==0 );
  sqlite3_free(pCsr);
  return SQLITE_OK;
}

/*
** If pCsr->pStmt has not been prepared (i.e. if pCsr->pStmt==0), then
** compose and prepare an SQL statement of the form:
**
**    "SELECT <columns> FROM %_content WHERE rowid = ?"
**
** (or the equivalent for a content=xxx table) and set pCsr->pStmt to
** it. If an error occurs, return an SQLite error code.
**
** Otherwise, set *ppStmt to point to pCsr->pStmt and return SQLITE_OK.
*/
static int fts3CursorSeekStmt(Fts3Cursor *pCsr, sqlite3_stmt **ppStmt){
  int rc = SQLITE_OK;
  if( pCsr->pStmt==0 ){
    Fts3Table *p = (Fts3Table *)pCsr->base.pVtab;
    char *zSql;
    zSql = sqlite3_mprintf("SELECT %s WHERE rowid = ?", p->zReadExprlist);
    if( !zSql ) return SQLITE_NOMEM;
    rc = sqlite3_prepare_v2(p->db, zSql, -1, &pCsr->pStmt, 0);
    sqlite3_free(zSql);
  }
  *ppStmt = pCsr->pStmt;
  return rc;
}

/*
** Position the pCsr->pStmt statement so that it is on the row
** of the %_content table that contains the last match.  Return
** SQLITE_OK on success.  
*/
static int fts3CursorSeek(sqlite3_context *pContext, Fts3Cursor *pCsr){
  int rc = SQLITE_OK;
  if( pCsr->isRequireSeek ){
    sqlite3_stmt *pStmt = 0;

    rc = fts3CursorSeekStmt(pCsr, &pStmt);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(pCsr->pStmt, 1, pCsr->iPrevId);
      pCsr->isRequireSeek = 0;
      if( SQLITE_ROW==sqlite3_step(pCsr->pStmt) ){
        return SQLITE_OK;
      }else{
        rc = sqlite3_reset(pCsr->pStmt);
        if( rc==SQLITE_OK && ((Fts3Table *)pCsr->base.pVtab)->zContentTbl==0 ){
          /* If no row was found and no error has occurred, then the %_content
          ** table is missing a row that is present in the full-text index.
          ** The data structures are corrupt.  */
          rc = FTS_CORRUPT_VTAB;
          pCsr->isEof = 1;
        }
      }
    }
  }

  if( rc!=SQLITE_OK && pContext ){
    sqlite3_result_error_code(pContext, rc);
  }
  return rc;
}

/*
** This function is used to process a single interior node when searching
** a b-tree for a term or term prefix. The node data is passed to this 
** function via the zNode/nNode parameters. The term to search for is
** passed in zTerm/nTerm.
**
** If piFirst is not NULL, then this function sets *piFirst to the blockid
** of the child node that heads the sub-tree that may contain the term.
**
** If piLast is not NULL, then *piLast is set to the right-most child node
** that heads a sub-tree that may contain a term for which zTerm/nTerm is
** a prefix.
**
** If an OOM error occurs, SQLITE_NOMEM is returned. Otherwise, SQLITE_OK.
*/
static int fts3ScanInteriorNode(
  const char *zTerm,              /* Term to select leaves for */
  int nTerm,                      /* Size of term zTerm in bytes */
  const char *zNode,              /* Buffer containing segment interior node */
  int nNode,                      /* Size of buffer at zNode */
  sqlite3_int64 *piFirst,         /* OUT: Selected child node */
  sqlite3_int64 *piLast           /* OUT: Selected child node */
){
  int rc = SQLITE_OK;             /* Return code */
  const char *zCsr = zNode;       /* Cursor to iterate through node */
  const char *zEnd = &zCsr[nNode];/* End of interior node buffer */
  char *zBuffer = 0;              /* Buffer to load terms into */
  int nAlloc = 0;                 /* Size of allocated buffer */
  int isFirstTerm = 1;            /* True when processing first term on page */
  sqlite3_int64 iChild;           /* Block id of child node to descend to */

  /* Skip over the 'height' varint that occurs at the start of every 
  ** interior node. Then load the blockid of the left-child of the b-tree
  ** node into variable iChild.  
  **
  ** Even if the data structure on disk is corrupted, this (reading two
  ** varints from the buffer) does not risk an overread. If zNode is a
  ** root node, then the buffer comes from a SELECT statement. SQLite does
  ** not make this guarantee explicitly, but in practice there are always
  ** either more than 20 bytes of allocated space following the nNode bytes of
  ** contents, or two zero bytes. Or, if the node is read from the %_segments
  ** table, then there are always 20 bytes of zeroed padding following the
  ** nNode bytes of content (see sqlite3Fts3ReadBlock() for details).
  */
  zCsr += sqlite3Fts3GetVarint(zCsr, &iChild);
  zCsr += sqlite3Fts3GetVarint(zCsr, &iChild);
  if( zCsr>zEnd ){
    return FTS_CORRUPT_VTAB;
  }
  
  while( zCsr<zEnd && (piFirst || piLast) ){
    int cmp;                      /* memcmp() result */
    int nSuffix;                  /* Size of term suffix */
    int nPrefix = 0;              /* Size of term prefix */
    int nBuffer;                  /* Total term size */
  
    /* Load the next term on the node into zBuffer. Use realloc() to expand
    ** the size of zBuffer if required.  */
    if( !isFirstTerm ){
      zCsr += fts3GetVarint32(zCsr, &nPrefix);
    }
    isFirstTerm = 0;
    zCsr += fts3GetVarint32(zCsr, &nSuffix);
    
    if( nPrefix<0 || nSuffix<0 || &zCsr[nSuffix]>zEnd ){
      rc = FTS_CORRUPT_VTAB;
      goto finish_scan;
    }
    if( nPrefix+nSuffix>nAlloc ){
      char *zNew;
      nAlloc = (nPrefix+nSuffix) * 2;
      zNew = (char *)sqlite3_realloc(zBuffer, nAlloc);
      if( !zNew ){
        rc = SQLITE_NOMEM;
        goto finish_scan;
      }
      zBuffer = zNew;
    }
    assert( zBuffer );
    memcpy(&zBuffer[nPrefix], zCsr, nSuffix);
    nBuffer = nPrefix + nSuffix;
    zCsr += nSuffix;

    /* Compare the term we are searching for with the term just loaded from
    ** the interior node. If the specified term is greater than or equal
    ** to the term from the interior node, then all terms on the sub-tree 
    ** headed by node iChild are smaller than zTerm. No need to search 
    ** iChild.
    **
    ** If the interior node term is larger than the specified term, then
    ** the tree headed by iChild may contain the specified term.
    */
    cmp = memcmp(zTerm, zBuffer, (nBuffer>nTerm ? nTerm : nBuffer));
    if( piFirst && (cmp<0 || (cmp==0 && nBuffer>nTerm)) ){
      *piFirst = iChild;
      piFirst = 0;
    }

    if( piLast && cmp<0 ){
      *piLast = iChild;
      piLast = 0;
    }

    iChild++;
  };

  if( piFirst ) *piFirst = iChild;
  if( piLast ) *piLast = iChild;

 finish_scan:
  sqlite3_free(zBuffer);
  return rc;
}


/*
** The buffer pointed to by argument zNode (size nNode bytes) contains an
** interior node of a b-tree segment. The zTerm buffer (size nTerm bytes)
** contains a term. This function searches the sub-tree headed by the zNode
** node for the range of leaf nodes that may contain the specified term
** or terms for which the specified term is a prefix.
**
** If piLeaf is not NULL, then *piLeaf is set to the blockid of the 
** left-most leaf node in the tree that may contain the specified term.
** If piLeaf2 is not NULL, then *piLeaf2 is set to the blockid of the
** right-most leaf node that may contain a term for which the specified
** term is a prefix.
**
** It is possible that the range of returned leaf nodes does not contain 
** the specified term or any terms for which it is a prefix. However, if the 
** segment does contain any such terms, they are stored within the identified
** range. Because this function only inspects interior segment nodes (and
** never loads leaf nodes into memory), it is not possible to be sure.
**
** If an error occurs, an error code other than SQLITE_OK is returned.
*/ 
static int fts3SelectLeaf(
  Fts3Table *p,                   /* Virtual table handle */
  const char *zTerm,              /* Term to select leaves for */
  int nTerm,                      /* Size of term zTerm in bytes */
  const char *zNode,              /* Buffer containing segment interior node */
  int nNode,                      /* Size of buffer at zNode */
  sqlite3_int64 *piLeaf,          /* Selected leaf node */
  sqlite3_int64 *piLeaf2          /* Selected leaf node */
){
  int rc;                         /* Return code */
  int iHeight;                    /* Height of this node in tree */

  assert( piLeaf || piLeaf2 );

  fts3GetVarint32(zNode, &iHeight);
  rc = fts3ScanInteriorNode(zTerm, nTerm, zNode, nNode, piLeaf, piLeaf2);
  assert( !piLeaf2 || !piLeaf || rc!=SQLITE_OK || (*piLeaf<=*piLeaf2) );

  if( rc==SQLITE_OK && iHeight>1 ){
    char *zBlob = 0;              /* Blob read from %_segments table */
    int nBlob;                    /* Size of zBlob in bytes */

    if( piLeaf && piLeaf2 && (*piLeaf!=*piLeaf2) ){
      rc = sqlite3Fts3ReadBlock(p, *piLeaf, &zBlob, &nBlob, 0);
      if( rc==SQLITE_OK ){
        rc = fts3SelectLeaf(p, zTerm, nTerm, zBlob, nBlob, piLeaf, 0);
      }
      sqlite3_free(zBlob);
      piLeaf = 0;
      zBlob = 0;
    }

    if( rc==SQLITE_OK ){
      rc = sqlite3Fts3ReadBlock(p, piLeaf?*piLeaf:*piLeaf2, &zBlob, &nBlob, 0);
    }
    if( rc==SQLITE_OK ){
      rc = fts3SelectLeaf(p, zTerm, nTerm, zBlob, nBlob, piLeaf, piLeaf2);
    }
    sqlite3_free(zBlob);
  }

  return rc;
}

/*
** This function is used to create delta-encoded serialized lists of FTS3 
** varints. Each call to this function appends a single varint to a list.
*/
static void fts3PutDeltaVarint(
  char **pp,                      /* IN/OUT: Output pointer */
  sqlite3_int64 *piPrev,          /* IN/OUT: Previous value written to list */
  sqlite3_int64 iVal              /* Write this value to the list */
){
  assert( iVal-*piPrev > 0 || (*piPrev==0 && iVal==0) );
  *pp += sqlite3Fts3PutVarint(*pp, iVal-*piPrev);
  *piPrev = iVal;
}

/*
** When this function is called, *ppPoslist is assumed to point to the 
** start of a position-list. After it returns, *ppPoslist points to the
** first byte after the position-list.
**
** A position list is list of positions (delta encoded) and columns for 
** a single document record of a doclist.  So, in other words, this
** routine advances *ppPoslist so that it points to the next docid in
** the doclist, or to the first byte past the end of the doclist.
**
** If pp is not NULL, then the contents of the position list are copied
** to *pp. *pp is set to point to the first byte past the last byte copied
** before this function returns.
*/
static void fts3PoslistCopy(char **pp, char **ppPoslist){
  char *pEnd = *ppPoslist;
  char c = 0;

  /* The end of a position list is marked by a zero encoded as an FTS3 
  ** varint. A single POS_END (0) byte. Except, if the 0 byte is preceded by
  ** a byte with the 0x80 bit set, then it is not a varint 0, but the tail
  ** of some other, multi-byte, value.
  **
  ** The following while-loop moves pEnd to point to the first byte that is not 
  ** immediately preceded by a byte with the 0x80 bit set. Then increments
  ** pEnd once more so that it points to the byte immediately following the
  ** last byte in the position-list.
  */
  while( *pEnd | c ){
    c = *pEnd++ & 0x80;
    testcase( c!=0 && (*pEnd)==0 );
  }
  pEnd++;  /* Advance past the POS_END terminator byte */

  if( pp ){
    int n = (int)(pEnd - *ppPoslist);
    char *p = *pp;
    memcpy(p, *ppPoslist, n);
    p += n;
    *pp = p;
  }
  *ppPoslist = pEnd;
}

/*
** When this function is called, *ppPoslist is assumed to point to the 
** start of a column-list. After it returns, *ppPoslist points to the
** to the terminator (POS_COLUMN or POS_END) byte of the column-list.
**
** A column-list is list of delta-encoded positions for a single column
** within a single document within a doclist.
**
** The column-list is terminated either by a POS_COLUMN varint (1) or
** a POS_END varint (0).  This routine leaves *ppPoslist pointing to
** the POS_COLUMN or POS_END that terminates the column-list.
**
** If pp is not NULL, then the contents of the column-list are copied
** to *pp. *pp is set to point to the first byte past the last byte copied
** before this function returns.  The POS_COLUMN or POS_END terminator
** is not copied into *pp.
*/
static void fts3ColumnlistCopy(char **pp, char **ppPoslist){
  char *pEnd = *ppPoslist;
  char c = 0;

  /* A column-list is terminated by either a 0x01 or 0x00 byte that is
  ** not part of a multi-byte varint.
  */
  while( 0xFE & (*pEnd | c) ){
    c = *pEnd++ & 0x80;
    testcase( c!=0 && ((*pEnd)&0xfe)==0 );
  }
  if( pp ){
    int n = (int)(pEnd - *ppPoslist);
    char *p = *pp;
    memcpy(p, *ppPoslist, n);
    p += n;
    *pp = p;
  }
  *ppPoslist = pEnd;
}

/*
** Value used to signify the end of an position-list. This is safe because
** it is not possible to have a document with 2^31 terms.
*/
#define POSITION_LIST_END 0x7fffffff

/*
** This function is used to help parse position-lists. When this function is
** called, *pp may point to the start of the next varint in the position-list
** being parsed, or it may point to 1 byte past the end of the position-list
** (in which case **pp will be a terminator bytes POS_END (0) or
** (1)).
**
** If *pp points past the end of the current position-list, set *pi to 
** POSITION_LIST_END and return. Otherwise, read the next varint from *pp,
** increment the current value of *pi by the value read, and set *pp to
** point to the next value before returning.
**
** Before calling this routine *pi must be initialized to the value of
** the previous position, or zero if we are reading the first position
** in the position-list.  Because positions are delta-encoded, the value
** of the previous position is needed in order to compute the value of
** the next position.
*/
static void fts3ReadNextPos(
  char **pp,                    /* IN/OUT: Pointer into position-list buffer */
  sqlite3_int64 *pi             /* IN/OUT: Value read from position-list */
){
  if( (**pp)&0xFE ){
    fts3GetDeltaVarint(pp, pi);
    *pi -= 2;
  }else{
    *pi = POSITION_LIST_END;
  }
}

/*
** If parameter iCol is not 0, write an POS_COLUMN (1) byte followed by
** the value of iCol encoded as a varint to *pp.   This will start a new
** column list.
**
** Set *pp to point to the byte just after the last byte written before 
** returning (do not modify it if iCol==0). Return the total number of bytes
** written (0 if iCol==0).
*/
static int fts3PutColNumber(char **pp, int iCol){
  int n = 0;                      /* Number of bytes written */
  if( iCol ){
    char *p = *pp;                /* Output pointer */
    n = 1 + sqlite3Fts3PutVarint(&p[1], iCol);
    *p = 0x01;
    *pp = &p[n];
  }
  return n;
}

/*
** Compute the union of two position lists.  The output written
** into *pp contains all positions of both *pp1 and *pp2 in sorted
** order and with any duplicates removed.  All pointers are
** updated appropriately.   The caller is responsible for insuring
** that there is enough space in *pp to hold the complete output.
*/
static void fts3PoslistMerge(
  char **pp,                      /* Output buffer */
  char **pp1,                     /* Left input list */
  char **pp2                      /* Right input list */
){
  char *p = *pp;
  char *p1 = *pp1;
  char *p2 = *pp2;

  while( *p1 || *p2 ){
    int iCol1;         /* The current column index in pp1 */
    int iCol2;         /* The current column index in pp2 */

    if( *p1==POS_COLUMN ) fts3GetVarint32(&p1[1], &iCol1);
    else if( *p1==POS_END ) iCol1 = POSITION_LIST_END;
    else iCol1 = 0;

    if( *p2==POS_COLUMN ) fts3GetVarint32(&p2[1], &iCol2);
    else if( *p2==POS_END ) iCol2 = POSITION_LIST_END;
    else iCol2 = 0;

    if( iCol1==iCol2 ){
      sqlite3_int64 i1 = 0;       /* Last position from pp1 */
      sqlite3_int64 i2 = 0;       /* Last position from pp2 */
      sqlite3_int64 iPrev = 0;
      int n = fts3PutColNumber(&p, iCol1);
      p1 += n;
      p2 += n;

      /* At this point, both p1 and p2 point to the start of column-lists
      ** for the same column (the column with index iCol1 and iCol2).
      ** A column-list is a list of non-negative delta-encoded varints, each 
      ** incremented by 2 before being stored. Each list is terminated by a
      ** POS_END (0) or POS_COLUMN (1). The following block merges the two lists
      ** and writes the results to buffer p. p is left pointing to the byte
      ** after the list written. No terminator (POS_END or POS_COLUMN) is
      ** written to the output.
      */
      fts3GetDeltaVarint(&p1, &i1);
      fts3GetDeltaVarint(&p2, &i2);
      do {
        fts3PutDeltaVarint(&p, &iPrev, (i1<i2) ? i1 : i2); 
        iPrev -= 2;
        if( i1==i2 ){
          fts3ReadNextPos(&p1, &i1);
          fts3ReadNextPos(&p2, &i2);
        }else if( i1<i2 ){
          fts3ReadNextPos(&p1, &i1);
        }else{
          fts3ReadNextPos(&p2, &i2);
        }
      }while( i1!=POSITION_LIST_END || i2!=POSITION_LIST_END );
    }else if( iCol1<iCol2 ){
      p1 += fts3PutColNumber(&p, iCol1);
      fts3ColumnlistCopy(&p, &p1);
    }else{
      p2 += fts3PutColNumber(&p, iCol2);
      fts3ColumnlistCopy(&p, &p2);
    }
  }

  *p++ = POS_END;
  *pp = p;
  *pp1 = p1 + 1;
  *pp2 = p2 + 1;
}

/*
** This function is used to merge two position lists into one. When it is
** called, *pp1 and *pp2 must both point to position lists. A position-list is
** the part of a doclist that follows each document id. For example, if a row
** contains:
**
**     'a b c'|'x y z'|'a b b a'
**
** Then the position list for this row for token 'b' would consist of:
**
**     0x02 0x01 0x02 0x03 0x03 0x00
**
** When this function returns, both *pp1 and *pp2 are left pointing to the
** byte following the 0x00 terminator of their respective position lists.
**
** If isSaveLeft is 0, an entry is added to the output position list for 
** each position in *pp2 for which there exists one or more positions in
** *pp1 so that (pos(*pp2)>pos(*pp1) && pos(*pp2)-pos(*pp1)<=nToken). i.e.
** when the *pp1 token appears before the *pp2 token, but not more than nToken
** slots before it.
**
** e.g. nToken==1 searches for adjacent positions.
*/
static int fts3PoslistPhraseMerge(
  char **pp,                      /* IN/OUT: Preallocated output buffer */
  int nToken,                     /* Maximum difference in token positions */
  int isSaveLeft,                 /* Save the left position */
  int isExact,                    /* If *pp1 is exactly nTokens before *pp2 */
  char **pp1,                     /* IN/OUT: Left input list */
  char **pp2                      /* IN/OUT: Right input list */
){
  char *p = *pp;
  char *p1 = *pp1;
  char *p2 = *pp2;
  int iCol1 = 0;
  int iCol2 = 0;

  /* Never set both isSaveLeft and isExact for the same invocation. */
  assert( isSaveLeft==0 || isExact==0 );

  assert( p!=0 && *p1!=0 && *p2!=0 );
  if( *p1==POS_COLUMN ){ 
    p1++;
    p1 += fts3GetVarint32(p1, &iCol1);
  }
  if( *p2==POS_COLUMN ){ 
    p2++;
    p2 += fts3GetVarint32(p2, &iCol2);
  }

  while( 1 ){
    if( iCol1==iCol2 ){
      char *pSave = p;
      sqlite3_int64 iPrev = 0;
      sqlite3_int64 iPos1 = 0;
      sqlite3_int64 iPos2 = 0;

      if( iCol1 ){
        *p++ = POS_COLUMN;
        p += sqlite3Fts3PutVarint(p, iCol1);
      }

      assert( *p1!=POS_END && *p1!=POS_COLUMN );
      assert( *p2!=POS_END && *p2!=POS_COLUMN );
      fts3GetDeltaVarint(&p1, &iPos1); iPos1 -= 2;
      fts3GetDeltaVarint(&p2, &iPos2); iPos2 -= 2;

      while( 1 ){
        if( iPos2==iPos1+nToken 
         || (isExact==0 && iPos2>iPos1 && iPos2<=iPos1+nToken) 
        ){
          sqlite3_int64 iSave;
          iSave = isSaveLeft ? iPos1 : iPos2;
          fts3PutDeltaVarint(&p, &iPrev, iSave+2); iPrev -= 2;
          pSave = 0;
          assert( p );
        }
        if( (!isSaveLeft && iPos2<=(iPos1+nToken)) || iPos2<=iPos1 ){
          if( (*p2&0xFE)==0 ) break;
          fts3GetDeltaVarint(&p2, &iPos2); iPos2 -= 2;
        }else{
          if( (*p1&0xFE)==0 ) break;
          fts3GetDeltaVarint(&p1, &iPos1); iPos1 -= 2;
        }
      }

      if( pSave ){
        assert( pp && p );
        p = pSave;
      }

      fts3ColumnlistCopy(0, &p1);
      fts3ColumnlistCopy(0, &p2);
      assert( (*p1&0xFE)==0 && (*p2&0xFE)==0 );
      if( 0==*p1 || 0==*p2 ) break;

      p1++;
      p1 += fts3GetVarint32(p1, &iCol1);
      p2++;
      p2 += fts3GetVarint32(p2, &iCol2);
    }

    /* Advance pointer p1 or p2 (whichever corresponds to the smaller of
    ** iCol1 and iCol2) so that it points to either the 0x00 that marks the
    ** end of the position list, or the 0x01 that precedes the next 
    ** column-number in the position list. 
    */
    else if( iCol1<iCol2 ){
      fts3ColumnlistCopy(0, &p1);
      if( 0==*p1 ) break;
      p1++;
      p1 += fts3GetVarint32(p1, &iCol1);
    }else{
      fts3ColumnlistCopy(0, &p2);
      if( 0==*p2 ) break;
      p2++;
      p2 += fts3GetVarint32(p2, &iCol2);
    }
  }

  fts3PoslistCopy(0, &p2);
  fts3PoslistCopy(0, &p1);
  *pp1 = p1;
  *pp2 = p2;
  if( *pp==p ){
    return 0;
  }
  *p++ = 0x00;
  *pp = p;
  return 1;
}

/*
** Merge two position-lists as required by the NEAR operator. The argument
** position lists correspond to the left and right phrases of an expression 
** like:
**
**     "phrase 1" NEAR "phrase number 2"
**
** Position list *pp1 corresponds to the left-hand side of the NEAR 
** expression and *pp2 to the right. As usual, the indexes in the position 
** lists are the offsets of the last token in each phrase (tokens "1" and "2" 
** in the example above).
**
** The output position list - written to *pp - is a copy of *pp2 with those
** entries that are not sufficiently NEAR entries in *pp1 removed.
*/
static int fts3PoslistNearMerge(
  char **pp,                      /* Output buffer */
  char *aTmp,                     /* Temporary buffer space */
  int nRight,                     /* Maximum difference in token positions */
  int nLeft,                      /* Maximum difference in token positions */
  char **pp1,                     /* IN/OUT: Left input list */
  char **pp2                      /* IN/OUT: Right input list */
){
  char *p1 = *pp1;
  char *p2 = *pp2;

  char *pTmp1 = aTmp;
  char *pTmp2;
  char *aTmp2;
  int res = 1;

  fts3PoslistPhraseMerge(&pTmp1, nRight, 0, 0, pp1, pp2);
  aTmp2 = pTmp2 = pTmp1;
  *pp1 = p1;
  *pp2 = p2;
  fts3PoslistPhraseMerge(&pTmp2, nLeft, 1, 0, pp2, pp1);
  if( pTmp1!=aTmp && pTmp2!=aTmp2 ){
    fts3PoslistMerge(pp, &aTmp, &aTmp2);
  }else if( pTmp1!=aTmp ){
    fts3PoslistCopy(pp, &aTmp);
  }else if( pTmp2!=aTmp2 ){
    fts3PoslistCopy(pp, &aTmp2);
  }else{
    res = 0;
  }

  return res;
}

/* 
** An instance of this function is used to merge together the (potentially
** large number of) doclists for each term that matches a prefix query.
** See function fts3TermSelectMerge() for details.
*/
typedef struct TermSelect TermSelect;
struct TermSelect {
  char *aaOutput[16];             /* Malloc'd output buffers */
  int anOutput[16];               /* Size each output buffer in bytes */
};

/*
** This function is used to read a single varint from a buffer. Parameter
** pEnd points 1 byte past the end of the buffer. When this function is
** called, if *pp points to pEnd or greater, then the end of the buffer
** has been reached. In this case *pp is set to 0 and the function returns.
**
** If *pp does not point to or past pEnd, then a single varint is read
** from *pp. *pp is then set to point 1 byte past the end of the read varint.
**
** If bDescIdx is false, the value read is added to *pVal before returning.
** If it is true, the value read is subtracted from *pVal before this 
** function returns.
*/
static void fts3GetDeltaVarint3(
  char **pp,                      /* IN/OUT: Point to read varint from */
  char *pEnd,                     /* End of buffer */
  int bDescIdx,                   /* True if docids are descending */
  sqlite3_int64 *pVal             /* IN/OUT: Integer value */
){
  if( *pp>=pEnd ){
    *pp = 0;
  }else{
    sqlite3_int64 iVal;
    *pp += sqlite3Fts3GetVarint(*pp, &iVal);
    if( bDescIdx ){
      *pVal -= iVal;
    }else{
      *pVal += iVal;
    }
  }
}

/*
** This function is used to write a single varint to a buffer. The varint
** is written to *pp. Before returning, *pp is set to point 1 byte past the
** end of the value written.
**
** If *pbFirst is zero when this function is called, the value written to
** the buffer is that of parameter iVal. 
**
** If *pbFirst is non-zero when this function is called, then the value 
** written is either (iVal-*piPrev) (if bDescIdx is zero) or (*piPrev-iVal)
** (if bDescIdx is non-zero).
**
** Before returning, this function always sets *pbFirst to 1 and *piPrev
** to the value of parameter iVal.
*/
static void fts3PutDeltaVarint3(
  char **pp,                      /* IN/OUT: Output pointer */
  int bDescIdx,                   /* True for descending docids */
  sqlite3_int64 *piPrev,          /* IN/OUT: Previous value written to list */
  int *pbFirst,                   /* IN/OUT: True after first int written */
  sqlite3_int64 iVal              /* Write this value to the list */
){
  sqlite3_int64 iWrite;
  if( bDescIdx==0 || *pbFirst==0 ){
    iWrite = iVal - *piPrev;
  }else{
    iWrite = *piPrev - iVal;
  }
  assert( *pbFirst || *piPrev==0 );
  assert( *pbFirst==0 || iWrite>0 );
  *pp += sqlite3Fts3PutVarint(*pp, iWrite);
  *piPrev = iVal;
  *pbFirst = 1;
}


/*
** This macro is used by various functions that merge doclists. The two
** arguments are 64-bit docid values. If the value of the stack variable
** bDescDoclist is 0 when this macro is invoked, then it returns (i1-i2). 
** Otherwise, (i2-i1).
**
** Using this makes it easier to write code that can merge doclists that are
** sorted in either ascending or descending order.
*/
#define DOCID_CMP(i1, i2) ((bDescDoclist?-1:1) * (i1-i2))

/*
** This function does an "OR" merge of two doclists (output contains all
** positions contained in either argument doclist). If the docids in the 
** input doclists are sorted in ascending order, parameter bDescDoclist
** should be false. If they are sorted in ascending order, it should be
** passed a non-zero value.
**
** If no error occurs, *paOut is set to point at an sqlite3_malloc'd buffer
** containing the output doclist and SQLITE_OK is returned. In this case
** *pnOut is set to the number of bytes in the output doclist.
**
** If an error occurs, an SQLite error code is returned. The output values
** are undefined in this case.
*/
static int fts3DoclistOrMerge(
  int bDescDoclist,               /* True if arguments are desc */
  char *a1, int n1,               /* First doclist */
  char *a2, int n2,               /* Second doclist */
  char **paOut, int *pnOut        /* OUT: Malloc'd doclist */
){
  sqlite3_int64 i1 = 0;
  sqlite3_int64 i2 = 0;
  sqlite3_int64 iPrev = 0;
  char *pEnd1 = &a1[n1];
  char *pEnd2 = &a2[n2];
  char *p1 = a1;
  char *p2 = a2;
  char *p;
  char *aOut;
  int bFirstOut = 0;

  *paOut = 0;
  *pnOut = 0;

  /* Allocate space for the output. Both the input and output doclists
  ** are delta encoded. If they are in ascending order (bDescDoclist==0),
  ** then the first docid in each list is simply encoded as a varint. For
  ** each subsequent docid, the varint stored is the difference between the
  ** current and previous docid (a positive number - since the list is in
  ** ascending order).
  **
  ** The first docid written to the output is therefore encoded using the 
  ** same number of bytes as it is in whichever of the input lists it is
  ** read from. And each subsequent docid read from the same input list 
  ** consumes either the same or less bytes as it did in the input (since
  ** the difference between it and the previous value in the output must
  ** be a positive value less than or equal to the delta value read from 
  ** the input list). The same argument applies to all but the first docid
  ** read from the 'other' list. And to the contents of all position lists
  ** that will be copied and merged from the input to the output.
  **
  ** However, if the first docid copied to the output is a negative number,
  ** then the encoding of the first docid from the 'other' input list may
  ** be larger in the output than it was in the input (since the delta value
  ** may be a larger positive integer than the actual docid).
  **
  ** The space required to store the output is therefore the sum of the
  ** sizes of the two inputs, plus enough space for exactly one of the input
  ** docids to grow. 
  **
  ** A symetric argument may be made if the doclists are in descending 
  ** order.
  */
  aOut = sqlite3_malloc(n1+n2+FTS3_VARINT_MAX-1);
  if( !aOut ) return SQLITE_NOMEM;

  p = aOut;
  fts3GetDeltaVarint3(&p1, pEnd1, 0, &i1);
  fts3GetDeltaVarint3(&p2, pEnd2, 0, &i2);
  while( p1 || p2 ){
    sqlite3_int64 iDiff = DOCID_CMP(i1, i2);

    if( p2 && p1 && iDiff==0 ){
      fts3PutDeltaVarint3(&p, bDescDoclist, &iPrev, &bFirstOut, i1);
      fts3PoslistMerge(&p, &p1, &p2);
      fts3GetDeltaVarint3(&p1, pEnd1, bDescDoclist, &i1);
      fts3GetDeltaVarint3(&p2, pEnd2, bDescDoclist, &i2);
    }else if( !p2 || (p1 && iDiff<0) ){
      fts3PutDeltaVarint3(&p, bDescDoclist, &iPrev, &bFirstOut, i1);
      fts3PoslistCopy(&p, &p1);
      fts3GetDeltaVarint3(&p1, pEnd1, bDescDoclist, &i1);
    }else{
      fts3PutDeltaVarint3(&p, bDescDoclist, &iPrev, &bFirstOut, i2);
      fts3PoslistCopy(&p, &p2);
      fts3GetDeltaVarint3(&p2, pEnd2, bDescDoclist, &i2);
    }
  }

  *paOut = aOut;
  *pnOut = (int)(p-aOut);
  assert( *pnOut<=n1+n2+FTS3_VARINT_MAX-1 );
  return SQLITE_OK;
}

/*
** This function does a "phrase" merge of two doclists. In a phrase merge,
** the output contains a copy of each position from the right-hand input
** doclist for which there is a position in the left-hand input doclist
** exactly nDist tokens before it.
**
** If the docids in the input doclists are sorted in ascending order,
** parameter bDescDoclist should be false. If they are sorted in ascending 
** order, it should be passed a non-zero value.
**
** The right-hand input doclist is overwritten by this function.
*/
static void fts3DoclistPhraseMerge(
  int bDescDoclist,               /* True if arguments are desc */
  int nDist,                      /* Distance from left to right (1=adjacent) */
  char *aLeft, int nLeft,         /* Left doclist */
  char *aRight, int *pnRight      /* IN/OUT: Right/output doclist */
){
  sqlite3_int64 i1 = 0;
  sqlite3_int64 i2 = 0;
  sqlite3_int64 iPrev = 0;
  char *pEnd1 = &aLeft[nLeft];
  char *pEnd2 = &aRight[*pnRight];
  char *p1 = aLeft;
  char *p2 = aRight;
  char *p;
  int bFirstOut = 0;
  char *aOut = aRight;

  assert( nDist>0 );

  p = aOut;
  fts3GetDeltaVarint3(&p1, pEnd1, 0, &i1);
  fts3GetDeltaVarint3(&p2, pEnd2, 0, &i2);

  while( p1 && p2 ){
    sqlite3_int64 iDiff = DOCID_CMP(i1, i2);
    if( iDiff==0 ){
      char *pSave = p;
      sqlite3_int64 iPrevSave = iPrev;
      int bFirstOutSave = bFirstOut;

      fts3PutDeltaVarint3(&p, bDescDoclist, &iPrev, &bFirstOut, i1);
      if( 0==fts3PoslistPhraseMerge(&p, nDist, 0, 1, &p1, &p2) ){
        p = pSave;
        iPrev = iPrevSave;
        bFirstOut = bFirstOutSave;
      }
      fts3GetDeltaVarint3(&p1, pEnd1, bDescDoclist, &i1);
      fts3GetDeltaVarint3(&p2, pEnd2, bDescDoclist, &i2);
    }else if( iDiff<0 ){
      fts3PoslistCopy(0, &p1);
      fts3GetDeltaVarint3(&p1, pEnd1, bDescDoclist, &i1);
    }else{
      fts3PoslistCopy(0, &p2);
      fts3GetDeltaVarint3(&p2, pEnd2, bDescDoclist, &i2);
    }
  }

  *pnRight = (int)(p - aOut);
}

/*
** Argument pList points to a position list nList bytes in size. This
** function checks to see if the position list contains any entries for
** a token in position 0 (of any column). If so, it writes argument iDelta
** to the output buffer pOut, followed by a position list consisting only
** of the entries from pList at position 0, and terminated by an 0x00 byte.
** The value returned is the number of bytes written to pOut (if any).
*/
SQLITE_PRIVATE int sqlite3Fts3FirstFilter(
  sqlite3_int64 iDelta,           /* Varint that may be written to pOut */
  char *pList,                    /* Position list (no 0x00 term) */
  int nList,                      /* Size of pList in bytes */
  char *pOut                      /* Write output here */
){
  int nOut = 0;
  int bWritten = 0;               /* True once iDelta has been written */
  char *p = pList;
  char *pEnd = &pList[nList];

  if( *p!=0x01 ){
    if( *p==0x02 ){
      nOut += sqlite3Fts3PutVarint(&pOut[nOut], iDelta);
      pOut[nOut++] = 0x02;
      bWritten = 1;
    }
    fts3ColumnlistCopy(0, &p);
  }

  while( p<pEnd && *p==0x01 ){
    sqlite3_int64 iCol;
    p++;
    p += sqlite3Fts3GetVarint(p, &iCol);
    if( *p==0x02 ){
      if( bWritten==0 ){
        nOut += sqlite3Fts3PutVarint(&pOut[nOut], iDelta);
        bWritten = 1;
      }
      pOut[nOut++] = 0x01;
      nOut += sqlite3Fts3PutVarint(&pOut[nOut], iCol);
      pOut[nOut++] = 0x02;
    }
    fts3ColumnlistCopy(0, &p);
  }
  if( bWritten ){
    pOut[nOut++] = 0x00;
  }

  return nOut;
}


/*
** Merge all doclists in the TermSelect.aaOutput[] array into a single
** doclist stored in TermSelect.aaOutput[0]. If successful, delete all
** other doclists (except the aaOutput[0] one) and return SQLITE_OK.
**
** If an OOM error occurs, return SQLITE_NOMEM. In this case it is
** the responsibility of the caller to free any doclists left in the
** TermSelect.aaOutput[] array.
*/
static int fts3TermSelectFinishMerge(Fts3Table *p, TermSelect *pTS){
  char *aOut = 0;
  int nOut = 0;
  int i;

  /* Loop through the doclists in the aaOutput[] array. Merge them all
  ** into a single doclist.
  */
  for(i=0; i<SizeofArray(pTS->aaOutput); i++){
    if( pTS->aaOutput[i] ){
      if( !aOut ){
        aOut = pTS->aaOutput[i];
        nOut = pTS->anOutput[i];
        pTS->aaOutput[i] = 0;
      }else{
        int nNew;
        char *aNew;

        int rc = fts3DoclistOrMerge(p->bDescIdx, 
            pTS->aaOutput[i], pTS->anOutput[i], aOut, nOut, &aNew, &nNew
        );
        if( rc!=SQLITE_OK ){
          sqlite3_free(aOut);
          return rc;
        }

        sqlite3_free(pTS->aaOutput[i]);
        sqlite3_free(aOut);
        pTS->aaOutput[i] = 0;
        aOut = aNew;
        nOut = nNew;
      }
    }
  }

  pTS->aaOutput[0] = aOut;
  pTS->anOutput[0] = nOut;
  return SQLITE_OK;
}

/*
** Merge the doclist aDoclist/nDoclist into the TermSelect object passed
** as the first argument. The merge is an "OR" merge (see function
** fts3DoclistOrMerge() for details).
**
** This function is called with the doclist for each term that matches
** a queried prefix. It merges all these doclists into one, the doclist
** for the specified prefix. Since there can be a very large number of
** doclists to merge, the merging is done pair-wise using the TermSelect
** object.
**
** This function returns SQLITE_OK if the merge is successful, or an
** SQLite error code (SQLITE_NOMEM) if an error occurs.
*/
static int fts3TermSelectMerge(
  Fts3Table *p,                   /* FTS table handle */
  TermSelect *pTS,                /* TermSelect object to merge into */
  char *aDoclist,                 /* Pointer to doclist */
  int nDoclist                    /* Size of aDoclist in bytes */
){
  if( pTS->aaOutput[0]==0 ){
    /* If this is the first term selected, copy the doclist to the output
    ** buffer using memcpy(). */
    pTS->aaOutput[0] = sqlite3_malloc(nDoclist);
    pTS->anOutput[0] = nDoclist;
    if( pTS->aaOutput[0] ){
      memcpy(pTS->aaOutput[0], aDoclist, nDoclist);
    }else{
      return SQLITE_NOMEM;
    }
  }else{
    char *aMerge = aDoclist;
    int nMerge = nDoclist;
    int iOut;

    for(iOut=0; iOut<SizeofArray(pTS->aaOutput); iOut++){
      if( pTS->aaOutput[iOut]==0 ){
        assert( iOut>0 );
        pTS->aaOutput[iOut] = aMerge;
        pTS->anOutput[iOut] = nMerge;
        break;
      }else{
        char *aNew;
        int nNew;

        int rc = fts3DoclistOrMerge(p->bDescIdx, aMerge, nMerge, 
            pTS->aaOutput[iOut], pTS->anOutput[iOut], &aNew, &nNew
        );
        if( rc!=SQLITE_OK ){
          if( aMerge!=aDoclist ) sqlite3_free(aMerge);
          return rc;
        }

        if( aMerge!=aDoclist ) sqlite3_free(aMerge);
        sqlite3_free(pTS->aaOutput[iOut]);
        pTS->aaOutput[iOut] = 0;
  
        aMerge = aNew;
        nMerge = nNew;
        if( (iOut+1)==SizeofArray(pTS->aaOutput) ){
          pTS->aaOutput[iOut] = aMerge;
          pTS->anOutput[iOut] = nMerge;
        }
      }
    }
  }
  return SQLITE_OK;
}

/*
** Append SegReader object pNew to the end of the pCsr->apSegment[] array.
*/
static int fts3SegReaderCursorAppend(
  Fts3MultiSegReader *pCsr, 
  Fts3SegReader *pNew
){
  if( (pCsr->nSegment%16)==0 ){
    Fts3SegReader **apNew;
    int nByte = (pCsr->nSegment + 16)*sizeof(Fts3SegReader*);
    apNew = (Fts3SegReader **)sqlite3_realloc(pCsr->apSegment, nByte);
    if( !apNew ){
      sqlite3Fts3SegReaderFree(pNew);
      return SQLITE_NOMEM;
    }
    pCsr->apSegment = apNew;
  }
  pCsr->apSegment[pCsr->nSegment++] = pNew;
  return SQLITE_OK;
}

/*
** Add seg-reader objects to the Fts3MultiSegReader object passed as the
** 8th argument.
**
** This function returns SQLITE_OK if successful, or an SQLite error code
** otherwise.
*/
static int fts3SegReaderCursor(
  Fts3Table *p,                   /* FTS3 table handle */
  int iLangid,                    /* Language id */
  int iIndex,                     /* Index to search (from 0 to p->nIndex-1) */
  int iLevel,                     /* Level of segments to scan */
  const char *zTerm,              /* Term to query for */
  int nTerm,                      /* Size of zTerm in bytes */
  int isPrefix,                   /* True for a prefix search */
  int isScan,                     /* True to scan from zTerm to EOF */
  Fts3MultiSegReader *pCsr        /* Cursor object to populate */
){
  int rc = SQLITE_OK;             /* Error code */
  sqlite3_stmt *pStmt = 0;        /* Statement to iterate through segments */
  int rc2;                        /* Result of sqlite3_reset() */

  /* If iLevel is less than 0 and this is not a scan, include a seg-reader 
  ** for the pending-terms. If this is a scan, then this call must be being
  ** made by an fts4aux module, not an FTS table. In this case calling
  ** Fts3SegReaderPending might segfault, as the data structures used by 
  ** fts4aux are not completely populated. So it's easiest to filter these
  ** calls out here.  */
  if( iLevel<0 && p->aIndex ){
    Fts3SegReader *pSeg = 0;
    rc = sqlite3Fts3SegReaderPending(p, iIndex, zTerm, nTerm, isPrefix, &pSeg);
    if( rc==SQLITE_OK && pSeg ){
      rc = fts3SegReaderCursorAppend(pCsr, pSeg);
    }
  }

  if( iLevel!=FTS3_SEGCURSOR_PENDING ){
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts3AllSegdirs(p, iLangid, iIndex, iLevel, &pStmt);
    }

    while( rc==SQLITE_OK && SQLITE_ROW==(rc = sqlite3_step(pStmt)) ){
      Fts3SegReader *pSeg = 0;

      /* Read the values returned by the SELECT into local variables. */
      sqlite3_int64 iStartBlock = sqlite3_column_int64(pStmt, 1);
      sqlite3_int64 iLeavesEndBlock = sqlite3_column_int64(pStmt, 2);
      sqlite3_int64 iEndBlock = sqlite3_column_int64(pStmt, 3);
      int nRoot = sqlite3_column_bytes(pStmt, 4);
      char const *zRoot = sqlite3_column_blob(pStmt, 4);

      /* If zTerm is not NULL, and this segment is not stored entirely on its
      ** root node, the range of leaves scanned can be reduced. Do this. */
      if( iStartBlock && zTerm ){
        sqlite3_int64 *pi = (isPrefix ? &iLeavesEndBlock : 0);
        rc = fts3SelectLeaf(p, zTerm, nTerm, zRoot, nRoot, &iStartBlock, pi);
        if( rc!=SQLITE_OK ) goto finished;
        if( isPrefix==0 && isScan==0 ) iLeavesEndBlock = iStartBlock;
      }
 
      rc = sqlite3Fts3SegReaderNew(pCsr->nSegment+1, 
          (isPrefix==0 && isScan==0),
          iStartBlock, iLeavesEndBlock, 
          iEndBlock, zRoot, nRoot, &pSeg
      );
      if( rc!=SQLITE_OK ) goto finished;
      rc = fts3SegReaderCursorAppend(pCsr, pSeg);
    }
  }

 finished:
  rc2 = sqlite3_reset(pStmt);
  if( rc==SQLITE_DONE ) rc = rc2;

  return rc;
}

/*
** Set up a cursor object for iterating through a full-text index or a 
** single level therein.
*/
SQLITE_PRIVATE int sqlite3Fts3SegReaderCursor(
  Fts3Table *p,                   /* FTS3 table handle */
  int iLangid,                    /* Language-id to search */
  int iIndex,                     /* Index to search (from 0 to p->nIndex-1) */
  int iLevel,                     /* Level of segments to scan */
  const char *zTerm,              /* Term to query for */
  int nTerm,                      /* Size of zTerm in bytes */
  int isPrefix,                   /* True for a prefix search */
  int isScan,                     /* True to scan from zTerm to EOF */
  Fts3MultiSegReader *pCsr       /* Cursor object to populate */
){
  assert( iIndex>=0 && iIndex<p->nIndex );
  assert( iLevel==FTS3_SEGCURSOR_ALL
      ||  iLevel==FTS3_SEGCURSOR_PENDING 
      ||  iLevel>=0
  );
  assert( iLevel<FTS3_SEGDIR_MAXLEVEL );
  assert( FTS3_SEGCURSOR_ALL<0 && FTS3_SEGCURSOR_PENDING<0 );
  assert( isPrefix==0 || isScan==0 );

  memset(pCsr, 0, sizeof(Fts3MultiSegReader));
  return fts3SegReaderCursor(
      p, iLangid, iIndex, iLevel, zTerm, nTerm, isPrefix, isScan, pCsr
  );
}

/*
** In addition to its current configuration, have the Fts3MultiSegReader
** passed as the 4th argument also scan the doclist for term zTerm/nTerm.
**
** SQLITE_OK is returned if no error occurs, otherwise an SQLite error code.
*/
static int fts3SegReaderCursorAddZero(
  Fts3Table *p,                   /* FTS virtual table handle */
  int iLangid,
  const char *zTerm,              /* Term to scan doclist of */
  int nTerm,                      /* Number of bytes in zTerm */
  Fts3MultiSegReader *pCsr        /* Fts3MultiSegReader to modify */
){
  return fts3SegReaderCursor(p, 
      iLangid, 0, FTS3_SEGCURSOR_ALL, zTerm, nTerm, 0, 0,pCsr
  );
}

/*
** Open an Fts3MultiSegReader to scan the doclist for term zTerm/nTerm. Or,
** if isPrefix is true, to scan the doclist for all terms for which 
** zTerm/nTerm is a prefix. If successful, return SQLITE_OK and write
** a pointer to the new Fts3MultiSegReader to *ppSegcsr. Otherwise, return
** an SQLite error code.
**
** It is the responsibility of the caller to free this object by eventually
** passing it to fts3SegReaderCursorFree() 
**
** SQLITE_OK is returned if no error occurs, otherwise an SQLite error code.
** Output parameter *ppSegcsr is set to 0 if an error occurs.
*/
static int fts3TermSegReaderCursor(
  Fts3Cursor *pCsr,               /* Virtual table cursor handle */
  const char *zTerm,              /* Term to query for */
  int nTerm,                      /* Size of zTerm in bytes */
  int isPrefix,                   /* True for a prefix search */
  Fts3MultiSegReader **ppSegcsr   /* OUT: Allocated seg-reader cursor */
){
  Fts3MultiSegReader *pSegcsr;    /* Object to allocate and return */
  int rc = SQLITE_NOMEM;          /* Return code */

  pSegcsr = sqlite3_malloc(sizeof(Fts3MultiSegReader));
  if( pSegcsr ){
    int i;
    int bFound = 0;               /* True once an index has been found */
    Fts3Table *p = (Fts3Table *)pCsr->base.pVtab;

    if( isPrefix ){
      for(i=1; bFound==0 && i<p->nIndex; i++){
        if( p->aIndex[i].nPrefix==nTerm ){
          bFound = 1;
          rc = sqlite3Fts3SegReaderCursor(p, pCsr->iLangid, 
              i, FTS3_SEGCURSOR_ALL, zTerm, nTerm, 0, 0, pSegcsr
          );
          pSegcsr->bLookup = 1;
        }
      }

      for(i=1; bFound==0 && i<p->nIndex; i++){
        if( p->aIndex[i].nPrefix==nTerm+1 ){
          bFound = 1;
          rc = sqlite3Fts3SegReaderCursor(p, pCsr->iLangid, 
              i, FTS3_SEGCURSOR_ALL, zTerm, nTerm, 1, 0, pSegcsr
          );
          if( rc==SQLITE_OK ){
            rc = fts3SegReaderCursorAddZero(
                p, pCsr->iLangid, zTerm, nTerm, pSegcsr
            );
          }
        }
      }
    }

    if( bFound==0 ){
      rc = sqlite3Fts3SegReaderCursor(p, pCsr->iLangid, 
          0, FTS3_SEGCURSOR_ALL, zTerm, nTerm, isPrefix, 0, pSegcsr
      );
      pSegcsr->bLookup = !isPrefix;
    }
  }

  *ppSegcsr = pSegcsr;
  return rc;
}

/*
** Free an Fts3MultiSegReader allocated by fts3TermSegReaderCursor().
*/
static void fts3SegReaderCursorFree(Fts3MultiSegReader *pSegcsr){
  sqlite3Fts3SegReaderFinish(pSegcsr);
  sqlite3_free(pSegcsr);
}

/*
** This function retrieves the doclist for the specified term (or term
** prefix) from the database.
*/
static int fts3TermSelect(
  Fts3Table *p,                   /* Virtual table handle */
  Fts3PhraseToken *pTok,          /* Token to query for */
  int iColumn,                    /* Column to query (or -ve for all columns) */
  int *pnOut,                     /* OUT: Size of buffer at *ppOut */
  char **ppOut                    /* OUT: Malloced result buffer */
){
  int rc;                         /* Return code */
  Fts3MultiSegReader *pSegcsr;    /* Seg-reader cursor for this term */
  TermSelect tsc;                 /* Object for pair-wise doclist merging */
  Fts3SegFilter filter;           /* Segment term filter configuration */

  pSegcsr = pTok->pSegcsr;
  memset(&tsc, 0, sizeof(TermSelect));

  filter.flags = FTS3_SEGMENT_IGNORE_EMPTY | FTS3_SEGMENT_REQUIRE_POS
        | (pTok->isPrefix ? FTS3_SEGMENT_PREFIX : 0)
        | (pTok->bFirst ? FTS3_SEGMENT_FIRST : 0)
        | (iColumn<p->nColumn ? FTS3_SEGMENT_COLUMN_FILTER : 0);
  filter.iCol = iColumn;
  filter.zTerm = pTok->z;
  filter.nTerm = pTok->n;

  rc = sqlite3Fts3SegReaderStart(p, pSegcsr, &filter);
  while( SQLITE_OK==rc
      && SQLITE_ROW==(rc = sqlite3Fts3SegReaderStep(p, pSegcsr)) 
  ){
    rc = fts3TermSelectMerge(p, &tsc, pSegcsr->aDoclist, pSegcsr->nDoclist);
  }

  if( rc==SQLITE_OK ){
    rc = fts3TermSelectFinishMerge(p, &tsc);
  }
  if( rc==SQLITE_OK ){
    *ppOut = tsc.aaOutput[0];
    *pnOut = tsc.anOutput[0];
  }else{
    int i;
    for(i=0; i<SizeofArray(tsc.aaOutput); i++){
      sqlite3_free(tsc.aaOutput[i]);
    }
  }

  fts3SegReaderCursorFree(pSegcsr);
  pTok->pSegcsr = 0;
  return rc;
}

/*
** This function counts the total number of docids in the doclist stored
** in buffer aList[], size nList bytes.
**
** If the isPoslist argument is true, then it is assumed that the doclist
** contains a position-list following each docid. Otherwise, it is assumed
** that the doclist is simply a list of docids stored as delta encoded 
** varints.
*/
static int fts3DoclistCountDocids(char *aList, int nList){
  int nDoc = 0;                   /* Return value */
  if( aList ){
    char *aEnd = &aList[nList];   /* Pointer to one byte after EOF */
    char *p = aList;              /* Cursor */
    while( p<aEnd ){
      nDoc++;
      while( (*p++)&0x80 );     /* Skip docid varint */
      fts3PoslistCopy(0, &p);   /* Skip over position list */
    }
  }

  return nDoc;
}

/*
** Advance the cursor to the next row in the %_content table that
** matches the search criteria.  For a MATCH search, this will be
** the next row that matches. For a full-table scan, this will be
** simply the next row in the %_content table.  For a docid lookup,
** this routine simply sets the EOF flag.
**
** Return SQLITE_OK if nothing goes wrong.  SQLITE_OK is returned
** even if we reach end-of-file.  The fts3EofMethod() will be called
** subsequently to determine whether or not an EOF was hit.
*/
static int fts3NextMethod(sqlite3_vtab_cursor *pCursor){
  int rc;
  Fts3Cursor *pCsr = (Fts3Cursor *)pCursor;
  if( pCsr->eSearch==FTS3_DOCID_SEARCH || pCsr->eSearch==FTS3_FULLSCAN_SEARCH ){
    if( SQLITE_ROW!=sqlite3_step(pCsr->pStmt) ){
      pCsr->isEof = 1;
      rc = sqlite3_reset(pCsr->pStmt);
    }else{
      pCsr->iPrevId = sqlite3_column_int64(pCsr->pStmt, 0);
      rc = SQLITE_OK;
    }
  }else{
    rc = fts3EvalNext((Fts3Cursor *)pCursor);
  }
  assert( ((Fts3Table *)pCsr->base.pVtab)->pSegments==0 );
  return rc;
}

/*
** The following are copied from sqliteInt.h.
**
** Constants for the largest and smallest possible 64-bit signed integers.
** These macros are designed to work correctly on both 32-bit and 64-bit
** compilers.
*/
#ifndef SQLITE_AMALGAMATION
# define LARGEST_INT64  (0xffffffff|(((sqlite3_int64)0x7fffffff)<<32))
# define SMALLEST_INT64 (((sqlite3_int64)-1) - LARGEST_INT64)
#endif

/*
** If the numeric type of argument pVal is "integer", then return it
** converted to a 64-bit signed integer. Otherwise, return a copy of
** the second parameter, iDefault.
*/
static sqlite3_int64 fts3DocidRange(sqlite3_value *pVal, i64 iDefault){
  if( pVal ){
    int eType = sqlite3_value_numeric_type(pVal);
    if( eType==SQLITE_INTEGER ){
      return sqlite3_value_int64(pVal);
    }
  }
  return iDefault;
}

/*
** This is the xFilter interface for the virtual table.  See
** the virtual table xFilter method documentation for additional
** information.
**
** If idxNum==FTS3_FULLSCAN_SEARCH then do a full table scan against
** the %_content table.
**
** If idxNum==FTS3_DOCID_SEARCH then do a docid lookup for a single entry
** in the %_content table.
**
** If idxNum>=FTS3_FULLTEXT_SEARCH then use the full text index.  The
** column on the left-hand side of the MATCH operator is column
** number idxNum-FTS3_FULLTEXT_SEARCH, 0 indexed.  argv[0] is the right-hand
** side of the MATCH operator.
*/
static int fts3FilterMethod(
  sqlite3_vtab_cursor *pCursor,   /* The cursor used for this query */
  int idxNum,                     /* Strategy index */
  const char *idxStr,             /* Unused */
  int nVal,                       /* Number of elements in apVal */
  sqlite3_value **apVal           /* Arguments for the indexing scheme */
){
  int rc;
  char *zSql;                     /* SQL statement used to access %_content */
  int eSearch;
  Fts3Table *p = (Fts3Table *)pCursor->pVtab;
  Fts3Cursor *pCsr = (Fts3Cursor *)pCursor;

  sqlite3_value *pCons = 0;       /* The MATCH or rowid constraint, if any */
  sqlite3_value *pLangid = 0;     /* The "langid = ?" constraint, if any */
  sqlite3_value *pDocidGe = 0;    /* The "docid >= ?" constraint, if any */
  sqlite3_value *pDocidLe = 0;    /* The "docid <= ?" constraint, if any */
  int iIdx;

  UNUSED_PARAMETER(idxStr);
  UNUSED_PARAMETER(nVal);

  eSearch = (idxNum & 0x0000FFFF);
  assert( eSearch>=0 && eSearch<=(FTS3_FULLTEXT_SEARCH+p->nColumn) );
  assert( p->pSegments==0 );

  /* Collect arguments into local variables */
  iIdx = 0;
  if( eSearch!=FTS3_FULLSCAN_SEARCH ) pCons = apVal[iIdx++];
  if( idxNum & FTS3_HAVE_LANGID ) pLangid = apVal[iIdx++];
  if( idxNum & FTS3_HAVE_DOCID_GE ) pDocidGe = apVal[iIdx++];
  if( idxNum & FTS3_HAVE_DOCID_LE ) pDocidLe = apVal[iIdx++];
  assert( iIdx==nVal );

  /* In case the cursor has been used before, clear it now. */
  sqlite3_finalize(pCsr->pStmt);
  sqlite3_free(pCsr->aDoclist);
  sqlite3_free(pCsr->aMatchinfo);
  sqlite3Fts3ExprFree(pCsr->pExpr);
  memset(&pCursor[1], 0, sizeof(Fts3Cursor)-sizeof(sqlite3_vtab_cursor));

  /* Set the lower and upper bounds on docids to return */
  pCsr->iMinDocid = fts3DocidRange(pDocidGe, SMALLEST_INT64);
  pCsr->iMaxDocid = fts3DocidRange(pDocidLe, LARGEST_INT64);

  if( idxStr ){
    pCsr->bDesc = (idxStr[0]=='D');
  }else{
    pCsr->bDesc = p->bDescIdx;
  }
  pCsr->eSearch = (i16)eSearch;

  if( eSearch!=FTS3_DOCID_SEARCH && eSearch!=FTS3_FULLSCAN_SEARCH ){
    int iCol = eSearch-FTS3_FULLTEXT_SEARCH;
    const char *zQuery = (const char *)sqlite3_value_text(pCons);

    if( zQuery==0 && sqlite3_value_type(pCons)!=SQLITE_NULL ){
      return SQLITE_NOMEM;
    }

    pCsr->iLangid = 0;
    if( pLangid ) pCsr->iLangid = sqlite3_value_int(pLangid);

    assert( p->base.zErrMsg==0 );
    rc = sqlite3Fts3ExprParse(p->pTokenizer, pCsr->iLangid,
        p->azColumn, p->bFts4, p->nColumn, iCol, zQuery, -1, &pCsr->pExpr, 
        &p->base.zErrMsg
    );
    if( rc!=SQLITE_OK ){
      return rc;
    }

    rc = fts3EvalStart(pCsr);
    sqlite3Fts3SegmentsClose(p);
    if( rc!=SQLITE_OK ) return rc;
    pCsr->pNextId = pCsr->aDoclist;
    pCsr->iPrevId = 0;
  }

  /* Compile a SELECT statement for this cursor. For a full-table-scan, the
  ** statement loops through all rows of the %_content table. For a
  ** full-text query or docid lookup, the statement retrieves a single
  ** row by docid.
  */
  if( eSearch==FTS3_FULLSCAN_SEARCH ){
    zSql = sqlite3_mprintf(
        "SELECT %s ORDER BY rowid %s",
        p->zReadExprlist, (pCsr->bDesc ? "DESC" : "ASC")
    );
    if( zSql ){
      rc = sqlite3_prepare_v2(p->db, zSql, -1, &pCsr->pStmt, 0);
      sqlite3_free(zSql);
    }else{
      rc = SQLITE_NOMEM;
    }
  }else if( eSearch==FTS3_DOCID_SEARCH ){
    rc = fts3CursorSeekStmt(pCsr, &pCsr->pStmt);
    if( rc==SQLITE_OK ){
      rc = sqlite3_bind_value(pCsr->pStmt, 1, pCons);
    }
  }
  if( rc!=SQLITE_OK ) return rc;

  return fts3NextMethod(pCursor);
}

/* 
** This is the xEof method of the virtual table. SQLite calls this 
** routine to find out if it has reached the end of a result set.
*/
static int fts3EofMethod(sqlite3_vtab_cursor *pCursor){
  return ((Fts3Cursor *)pCursor)->isEof;
}

/* 
** This is the xRowid method. The SQLite core calls this routine to
** retrieve the rowid for the current row of the result set. fts3
** exposes %_content.docid as the rowid for the virtual table. The
** rowid should be written to *pRowid.
*/
static int fts3RowidMethod(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid){
  Fts3Cursor *pCsr = (Fts3Cursor *) pCursor;
  *pRowid = pCsr->iPrevId;
  return SQLITE_OK;
}

/* 
** This is the xColumn method, called by SQLite to request a value from
** the row that the supplied cursor currently points to.
**
** If:
**
**   (iCol <  p->nColumn)   -> The value of the iCol'th user column.
**   (iCol == p->nColumn)   -> Magic column with the same name as the table.
**   (iCol == p->nColumn+1) -> Docid column
**   (iCol == p->nColumn+2) -> Langid column
*/
static int fts3ColumnMethod(
  sqlite3_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite3_context *pCtx,          /* Context for sqlite3_result_xxx() calls */
  int iCol                        /* Index of column to read value from */
){
  int rc = SQLITE_OK;             /* Return Code */
  Fts3Cursor *pCsr = (Fts3Cursor *) pCursor;
  Fts3Table *p = (Fts3Table *)pCursor->pVtab;

  /* The column value supplied by SQLite must be in range. */
  assert( iCol>=0 && iCol<=p->nColumn+2 );

  if( iCol==p->nColumn+1 ){
    /* This call is a request for the "docid" column. Since "docid" is an 
    ** alias for "rowid", use the xRowid() method to obtain the value.
    */
    sqlite3_result_int64(pCtx, pCsr->iPrevId);
  }else if( iCol==p->nColumn ){
    /* The extra column whose name is the same as the table.
    ** Return a blob which is a pointer to the cursor.  */
    sqlite3_result_blob(pCtx, &pCsr, sizeof(pCsr), SQLITE_TRANSIENT);
  }else if( iCol==p->nColumn+2 && pCsr->pExpr ){
    sqlite3_result_int64(pCtx, pCsr->iLangid);
  }else{
    /* The requested column is either a user column (one that contains 
    ** indexed data), or the language-id column.  */
    rc = fts3CursorSeek(0, pCsr);

    if( rc==SQLITE_OK ){
      if( iCol==p->nColumn+2 ){
        int iLangid = 0;
        if( p->zLanguageid ){
          iLangid = sqlite3_column_int(pCsr->pStmt, p->nColumn+1);
        }
        sqlite3_result_int(pCtx, iLangid);
      }else if( sqlite3_data_count(pCsr->pStmt)>(iCol+1) ){
        sqlite3_result_value(pCtx, sqlite3_column_value(pCsr->pStmt, iCol+1));
      }
    }
  }

  assert( ((Fts3Table *)pCsr->base.pVtab)->pSegments==0 );
  return rc;
}

/* 
** This function is the implementation of the xUpdate callback used by 
** FTS3 virtual tables. It is invoked by SQLite each time a row is to be
** inserted, updated or deleted.
*/
static int fts3UpdateMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  int nArg,                       /* Size of argument array */
  sqlite3_value **apVal,          /* Array of arguments */
  sqlite_int64 *pRowid            /* OUT: The affected (or effected) rowid */
){
  return sqlite3Fts3UpdateMethod(pVtab, nArg, apVal, pRowid);
}

/*
** Implementation of xSync() method. Flush the contents of the pending-terms
** hash-table to the database.
*/
static int fts3SyncMethod(sqlite3_vtab *pVtab){

  /* Following an incremental-merge operation, assuming that the input
  ** segments are not completely consumed (the usual case), they are updated
  ** in place to remove the entries that have already been merged. This
  ** involves updating the leaf block that contains the smallest unmerged
  ** entry and each block (if any) between the leaf and the root node. So
  ** if the height of the input segment b-trees is N, and input segments
  ** are merged eight at a time, updating the input segments at the end
  ** of an incremental-merge requires writing (8*(1+N)) blocks. N is usually
  ** small - often between 0 and 2. So the overhead of the incremental
  ** merge is somewhere between 8 and 24 blocks. To avoid this overhead
  ** dwarfing the actual productive work accomplished, the incremental merge
  ** is only attempted if it will write at least 64 leaf blocks. Hence
  ** nMinMerge.
  **
  ** Of course, updating the input segments also involves deleting a bunch
  ** of blocks from the segments table. But this is not considered overhead
  ** as it would also be required by a crisis-merge that used the same input 
  ** segments.
  */
  const u32 nMinMerge = 64;       /* Minimum amount of incr-merge work to do */

  Fts3Table *p = (Fts3Table*)pVtab;
  int rc = sqlite3Fts3PendingTermsFlush(p);

  if( rc==SQLITE_OK 
   && p->nLeafAdd>(nMinMerge/16) 
   && p->nAutoincrmerge && p->nAutoincrmerge!=0xff
  ){
    int mxLevel = 0;              /* Maximum relative level value in db */
    int A;                        /* Incr-merge parameter A */

    rc = sqlite3Fts3MaxLevel(p, &mxLevel);
    assert( rc==SQLITE_OK || mxLevel==0 );
    A = p->nLeafAdd * mxLevel;
    A += (A/2);
    if( A>(int)nMinMerge ) rc = sqlite3Fts3Incrmerge(p, A, p->nAutoincrmerge);
  }
  sqlite3Fts3SegmentsClose(p);
  return rc;
}

/*
** If it is currently unknown whether or not the FTS table has an %_stat
** table (if p->bHasStat==2), attempt to determine this (set p->bHasStat
** to 0 or 1). Return SQLITE_OK if successful, or an SQLite error code
** if an error occurs.
*/
static int fts3SetHasStat(Fts3Table *p){
  int rc = SQLITE_OK;
  if( p->bHasStat==2 ){
    const char *zFmt ="SELECT 1 FROM %Q.sqlite_master WHERE tbl_name='%q_stat'";
    char *zSql = sqlite3_mprintf(zFmt, p->zDb, p->zName);
    if( zSql ){
      sqlite3_stmt *pStmt = 0;
      rc = sqlite3_prepare_v2(p->db, zSql, -1, &pStmt, 0);
      if( rc==SQLITE_OK ){
        int bHasStat = (sqlite3_step(pStmt)==SQLITE_ROW);
        rc = sqlite3_finalize(pStmt);
        if( rc==SQLITE_OK ) p->bHasStat = bHasStat;
      }
      sqlite3_free(zSql);
    }else{
      rc = SQLITE_NOMEM;
    }
  }
  return rc;
}

/*
** Implementation of xBegin() method. 
*/
static int fts3BeginMethod(sqlite3_vtab *pVtab){
  Fts3Table *p = (Fts3Table*)pVtab;
  UNUSED_PARAMETER(pVtab);
  assert( p->pSegments==0 );
  assert( p->nPendingData==0 );
  assert( p->inTransaction!=1 );
  TESTONLY( p->inTransaction = 1 );
  TESTONLY( p->mxSavepoint = -1; );
  p->nLeafAdd = 0;
  return fts3SetHasStat(p);
}

/*
** Implementation of xCommit() method. This is a no-op. The contents of
** the pending-terms hash-table have already been flushed into the database
** by fts3SyncMethod().
*/
static int fts3CommitMethod(sqlite3_vtab *pVtab){
  TESTONLY( Fts3Table *p = (Fts3Table*)pVtab );
  UNUSED_PARAMETER(pVtab);
  assert( p->nPendingData==0 );
  assert( p->inTransaction!=0 );
  assert( p->pSegments==0 );
  TESTONLY( p->inTransaction = 0 );
  TESTONLY( p->mxSavepoint = -1; );
  return SQLITE_OK;
}

/*
** Implementation of xRollback(). Discard the contents of the pending-terms
** hash-table. Any changes made to the database are reverted by SQLite.
*/
static int fts3RollbackMethod(sqlite3_vtab *pVtab){
  Fts3Table *p = (Fts3Table*)pVtab;
  sqlite3Fts3PendingTermsClear(p);
  assert( p->inTransaction!=0 );
  TESTONLY( p->inTransaction = 0 );
  TESTONLY( p->mxSavepoint = -1; );
  return SQLITE_OK;
}

/*
** When called, *ppPoslist must point to the byte immediately following the
** end of a position-list. i.e. ( (*ppPoslist)[-1]==POS_END ). This function
** moves *ppPoslist so that it instead points to the first byte of the
** same position list.
*/
static void fts3ReversePoslist(char *pStart, char **ppPoslist){
  char *p = &(*ppPoslist)[-2];
  char c = 0;

  while( p>pStart && (c=*p--)==0 );
  while( p>pStart && (*p & 0x80) | c ){ 
    c = *p--; 
  }
  if( p>pStart ){ p = &p[2]; }
  while( *p++&0x80 );
  *ppPoslist = p;
}

/*
** Helper function used by the implementation of the overloaded snippet(),
** offsets() and optimize() SQL functions.
**
** If the value passed as the third argument is a blob of size
** sizeof(Fts3Cursor*), then the blob contents are copied to the 
** output variable *ppCsr and SQLITE_OK is returned. Otherwise, an error
** message is written to context pContext and SQLITE_ERROR returned. The
** string passed via zFunc is used as part of the error message.
*/
static int fts3FunctionArg(
  sqlite3_context *pContext,      /* SQL function call context */
  const char *zFunc,              /* Function name */
  sqlite3_value *pVal,            /* argv[0] passed to function */
  Fts3Cursor **ppCsr              /* OUT: Store cursor handle here */
){
  Fts3Cursor *pRet;
  if( sqlite3_value_type(pVal)!=SQLITE_BLOB 
   || sqlite3_value_bytes(pVal)!=sizeof(Fts3Cursor *)
  ){
    char *zErr = sqlite3_mprintf("illegal first argument to %s", zFunc);
    sqlite3_result_error(pContext, zErr, -1);
    sqlite3_free(zErr);
    return SQLITE_ERROR;
  }
  memcpy(&pRet, sqlite3_value_blob(pVal), sizeof(Fts3Cursor *));
  *ppCsr = pRet;
  return SQLITE_OK;
}

/*
** Implementation of the snippet() function for FTS3
*/
static void fts3SnippetFunc(
  sqlite3_context *pContext,      /* SQLite function call context */
  int nVal,                       /* Size of apVal[] array */
  sqlite3_value **apVal           /* Array of arguments */
){
  Fts3Cursor *pCsr;               /* Cursor handle passed through apVal[0] */
  const char *zStart = "<b>";
  const char *zEnd = "</b>";
  const char *zEllipsis = "<b>...</b>";
  int iCol = -1;
  int nToken = 15;                /* Default number of tokens in snippet */

  /* There must be at least one argument passed to this function (otherwise
  ** the non-overloaded version would have been called instead of this one).
  */
  assert( nVal>=1 );

  if( nVal>6 ){
    sqlite3_result_error(pContext, 
        "wrong number of arguments to function snippet()", -1);
    return;
  }
  if( fts3FunctionArg(pContext, "snippet", apVal[0], &pCsr) ) return;

  switch( nVal ){
    case 6: nToken = sqlite3_value_int(apVal[5]);
    case 5: iCol = sqlite3_value_int(apVal[4]);
    case 4: zEllipsis = (const char*)sqlite3_value_text(apVal[3]);
    case 3: zEnd = (const char*)sqlite3_value_text(apVal[2]);
    case 2: zStart = (const char*)sqlite3_value_text(apVal[1]);
  }
  if( !zEllipsis || !zEnd || !zStart ){
    sqlite3_result_error_nomem(pContext);
  }else if( SQLITE_OK==fts3CursorSeek(pContext, pCsr) ){
    sqlite3Fts3Snippet(pContext, pCsr, zStart, zEnd, zEllipsis, iCol, nToken);
  }
}

/*
** Implementation of the offsets() function for FTS3
*/
static void fts3OffsetsFunc(
  sqlite3_context *pContext,      /* SQLite function call context */
  int nVal,                       /* Size of argument array */
  sqlite3_value **apVal           /* Array of arguments */
){
  Fts3Cursor *pCsr;               /* Cursor handle passed through apVal[0] */

  UNUSED_PARAMETER(nVal);

  assert( nVal==1 );
  if( fts3FunctionArg(pContext, "offsets", apVal[0], &pCsr) ) return;
  assert( pCsr );
  if( SQLITE_OK==fts3CursorSeek(pContext, pCsr) ){
    sqlite3Fts3Offsets(pContext, pCsr);
  }
}

/* 
** Implementation of the special optimize() function for FTS3. This 
** function merges all segments in the database to a single segment.
** Example usage is:
**
**   SELECT optimize(t) FROM t LIMIT 1;
**
** where 't' is the name of an FTS3 table.
*/
static void fts3OptimizeFunc(
  sqlite3_context *pContext,      /* SQLite function call context */
  int nVal,                       /* Size of argument array */
  sqlite3_value **apVal           /* Array of arguments */
){
  int rc;                         /* Return code */
  Fts3Table *p;                   /* Virtual table handle */
  Fts3Cursor *pCursor;            /* Cursor handle passed through apVal[0] */

  UNUSED_PARAMETER(nVal);

  assert( nVal==1 );
  if( fts3FunctionArg(pContext, "optimize", apVal[0], &pCursor) ) return;
  p = (Fts3Table *)pCursor->base.pVtab;
  assert( p );

  rc = sqlite3Fts3Optimize(p);

  switch( rc ){
    case SQLITE_OK:
      sqlite3_result_text(pContext, "Index optimized", -1, SQLITE_STATIC);
      break;
    case SQLITE_DONE:
      sqlite3_result_text(pContext, "Index already optimal", -1, SQLITE_STATIC);
      break;
    default:
      sqlite3_result_error_code(pContext, rc);
      break;
  }
}

/*
** Implementation of the matchinfo() function for FTS3
*/
static void fts3MatchinfoFunc(
  sqlite3_context *pContext,      /* SQLite function call context */
  int nVal,                       /* Size of argument array */
  sqlite3_value **apVal           /* Array of arguments */
){
  Fts3Cursor *pCsr;               /* Cursor handle passed through apVal[0] */
  assert( nVal==1 || nVal==2 );
  if( SQLITE_OK==fts3FunctionArg(pContext, "matchinfo", apVal[0], &pCsr) ){
    const char *zArg = 0;
    if( nVal>1 ){
      zArg = (const char *)sqlite3_value_text(apVal[1]);
    }
    sqlite3Fts3Matchinfo(pContext, pCsr, zArg);
  }
}

/*
** This routine implements the xFindFunction method for the FTS3
** virtual table.
*/
static int fts3FindFunctionMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  int nArg,                       /* Number of SQL function arguments */
  const char *zName,              /* Name of SQL function */
  void (**pxFunc)(sqlite3_context*,int,sqlite3_value**), /* OUT: Result */
  void **ppArg                    /* Unused */
){
  struct Overloaded {
    const char *zName;
    void (*xFunc)(sqlite3_context*,int,sqlite3_value**);
  } aOverload[] = {
    { "snippet", fts3SnippetFunc },
    { "offsets", fts3OffsetsFunc },
    { "optimize", fts3OptimizeFunc },
    { "matchinfo", fts3MatchinfoFunc },
  };
  int i;                          /* Iterator variable */

  UNUSED_PARAMETER(pVtab);
  UNUSED_PARAMETER(nArg);
  UNUSED_PARAMETER(ppArg);

  for(i=0; i<SizeofArray(aOverload); i++){
    if( strcmp(zName, aOverload[i].zName)==0 ){
      *pxFunc = aOverload[i].xFunc;
      return 1;
    }
  }

  /* No function of the specified name was found. Return 0. */
  return 0;
}

/*
** Implementation of FTS3 xRename method. Rename an fts3 table.
*/
static int fts3RenameMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  const char *zName               /* New name of table */
){
  Fts3Table *p = (Fts3Table *)pVtab;
  sqlite3 *db = p->db;            /* Database connection */
  int rc;                         /* Return Code */

  /* At this point it must be known if the %_stat table exists or not.
  ** So bHasStat may not be 2.  */
  rc = fts3SetHasStat(p);
  
  /* As it happens, the pending terms table is always empty here. This is
  ** because an "ALTER TABLE RENAME TABLE" statement inside a transaction 
  ** always opens a savepoint transaction. And the xSavepoint() method 
  ** flushes the pending terms table. But leave the (no-op) call to
  ** PendingTermsFlush() in in case that changes.
  */
  assert( p->nPendingData==0 );
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts3PendingTermsFlush(p);
  }

  if( p->zContentTbl==0 ){
    fts3DbExec(&rc, db,
      "ALTER TABLE %Q.'%q_content'  RENAME TO '%q_content';",
      p->zDb, p->zName, zName
    );
  }

  if( p->bHasDocsize ){
    fts3DbExec(&rc, db,
      "ALTER TABLE %Q.'%q_docsize'  RENAME TO '%q_docsize';",
      p->zDb, p->zName, zName
    );
  }
  if( p->bHasStat ){
    fts3DbExec(&rc, db,
      "ALTER TABLE %Q.'%q_stat'  RENAME TO '%q_stat';",
      p->zDb, p->zName, zName
    );
  }
  fts3DbExec(&rc, db,
    "ALTER TABLE %Q.'%q_segments' RENAME TO '%q_segments';",
    p->zDb, p->zName, zName
  );
  fts3DbExec(&rc, db,
    "ALTER TABLE %Q.'%q_segdir'   RENAME TO '%q_segdir';",
    p->zDb, p->zName, zName
  );
  return rc;
}

/*
** The xSavepoint() method.
**
** Flush the contents of the pending-terms table to disk.
*/
static int fts3SavepointMethod(sqlite3_vtab *pVtab, int iSavepoint){
  int rc = SQLITE_OK;
  UNUSED_PARAMETER(iSavepoint);
  assert( ((Fts3Table *)pVtab)->inTransaction );
  assert( ((Fts3Table *)pVtab)->mxSavepoint < iSavepoint );
  TESTONLY( ((Fts3Table *)pVtab)->mxSavepoint = iSavepoint );
  if( ((Fts3Table *)pVtab)->bIgnoreSavepoint==0 ){
    rc = fts3SyncMethod(pVtab);
  }
  return rc;
}

/*
** The xRelease() method.
**
** This is a no-op.
*/
static int fts3ReleaseMethod(sqlite3_vtab *pVtab, int iSavepoint){
  TESTONLY( Fts3Table *p = (Fts3Table*)pVtab );
  UNUSED_PARAMETER(iSavepoint);
  UNUSED_PARAMETER(pVtab);
  assert( p->inTransaction );
  assert( p->mxSavepoint >= iSavepoint );
  TESTONLY( p->mxSavepoint = iSavepoint-1 );
  return SQLITE_OK;
}

/*
** The xRollbackTo() method.
**
** Discard the contents of the pending terms table.
*/
static int fts3RollbackToMethod(sqlite3_vtab *pVtab, int iSavepoint){
  Fts3Table *p = (Fts3Table*)pVtab;
  UNUSED_PARAMETER(iSavepoint);
  assert( p->inTransaction );
  assert( p->mxSavepoint >= iSavepoint );
  TESTONLY( p->mxSavepoint = iSavepoint );
  sqlite3Fts3PendingTermsClear(p);
  return SQLITE_OK;
}

static const sqlite3_module fts3Module = {
  /* iVersion      */ 2,
  /* xCreate       */ fts3CreateMethod,
  /* xConnect      */ fts3ConnectMethod,
  /* xBestIndex    */ fts3BestIndexMethod,
  /* xDisconnect   */ fts3DisconnectMethod,
  /* xDestroy      */ fts3DestroyMethod,
  /* xOpen         */ fts3OpenMethod,
  /* xClose        */ fts3CloseMethod,
  /* xFilter       */ fts3FilterMethod,
  /* xNext         */ fts3NextMethod,
  /* xEof          */ fts3EofMethod,
  /* xColumn       */ fts3ColumnMethod,
  /* xRowid        */ fts3RowidMethod,
  /* xUpdate       */ fts3UpdateMethod,
  /* xBegin        */ fts3BeginMethod,
  /* xSync         */ fts3SyncMethod,
  /* xCommit       */ fts3CommitMethod,
  /* xRollback     */ fts3RollbackMethod,
  /* xFindFunction */ fts3FindFunctionMethod,
  /* xRename */       fts3RenameMethod,
  /* xSavepoint    */ fts3SavepointMethod,
  /* xRelease      */ fts3ReleaseMethod,
  /* xRollbackTo   */ fts3RollbackToMethod,
};

/*
** This function is registered as the module destructor (called when an
** FTS3 enabled database connection is closed). It frees the memory
** allocated for the tokenizer hash table.
*/
static void hashDestroy(void *p){
  Fts3Hash *pHash = (Fts3Hash *)p;
  sqlite3Fts3HashClear(pHash);
  sqlite3_free(pHash);
}

/*
** The fts3 built-in tokenizers - "simple", "porter" and "icu"- are 
** implemented in files fts3_tokenizer1.c, fts3_porter.c and fts3_icu.c
** respectively. The following three forward declarations are for functions
** declared in these files used to retrieve the respective implementations.
**
** Calling sqlite3Fts3SimpleTokenizerModule() sets the value pointed
** to by the argument to point to the "simple" tokenizer implementation.
** And so on.
*/
SQLITE_PRIVATE void sqlite3Fts3SimpleTokenizerModule(sqlite3_tokenizer_module const**ppModule);
SQLITE_PRIVATE void sqlite3Fts3PorterTokenizerModule(sqlite3_tokenizer_module const**ppModule);
#ifndef SQLITE_DISABLE_FTS3_UNICODE
SQLITE_PRIVATE void sqlite3Fts3UnicodeTokenizer(sqlite3_tokenizer_module const**ppModule);
#endif
#ifdef SQLITE_ENABLE_ICU
SQLITE_PRIVATE void sqlite3Fts3IcuTokenizerModule(sqlite3_tokenizer_module const**ppModule);
#endif

/*
** Initialize the fts3 extension. If this extension is built as part
** of the sqlite library, then this function is called directly by
** SQLite. If fts3 is built as a dynamically loadable extension, this
** function is called by the sqlite3_extension_init() entry point.
*/
SQLITE_PRIVATE int sqlite3Fts3Init(sqlite3 *db){
  int rc = SQLITE_OK;
  Fts3Hash *pHash = 0;
  const sqlite3_tokenizer_module *pSimple = 0;
  const sqlite3_tokenizer_module *pPorter = 0;
#ifndef SQLITE_DISABLE_FTS3_UNICODE
  const sqlite3_tokenizer_module *pUnicode = 0;
#endif

#ifdef SQLITE_ENABLE_ICU
  const sqlite3_tokenizer_module *pIcu = 0;
  sqlite3Fts3IcuTokenizerModule(&pIcu);
#endif

#ifndef SQLITE_DISABLE_FTS3_UNICODE
  sqlite3Fts3UnicodeTokenizer(&pUnicode);
#endif

#ifdef SQLITE_TEST
  rc = sqlite3Fts3InitTerm(db);
  if( rc!=SQLITE_OK ) return rc;
#endif

  rc = sqlite3Fts3InitAux(db);
  if( rc!=SQLITE_OK ) return rc;

  sqlite3Fts3SimpleTokenizerModule(&pSimple);
  sqlite3Fts3PorterTokenizerModule(&pPorter);

  /* Allocate and initialize the hash-table used to store tokenizers. */
  pHash = sqlite3_malloc(sizeof(Fts3Hash));
  if( !pHash ){
    rc = SQLITE_NOMEM;
  }else{
    sqlite3Fts3HashInit(pHash, FTS3_HASH_STRING, 1);
  }

  /* Load the built-in tokenizers into the hash table */
  if( rc==SQLITE_OK ){
    if( sqlite3Fts3HashInsert(pHash, "simple", 7, (void *)pSimple)
     || sqlite3Fts3HashInsert(pHash, "porter", 7, (void *)pPorter) 

#ifndef SQLITE_DISABLE_FTS3_UNICODE
     || sqlite3Fts3HashInsert(pHash, "unicode61", 10, (void *)pUnicode) 
#endif
#ifdef SQLITE_ENABLE_ICU
     || (pIcu && sqlite3Fts3HashInsert(pHash, "icu", 4, (void *)pIcu))
#endif
    ){
      rc = SQLITE_NOMEM;
    }
  }

#ifdef SQLITE_TEST
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts3ExprInitTestInterface(db);
  }
#endif

  /* Create the virtual table wrapper around the hash-table and overload 
  ** the two scalar functions. If this is successful, register the
  ** module with sqlite.
  */
  if( SQLITE_OK==rc 
   && SQLITE_OK==(rc = sqlite3Fts3InitHashTable(db, pHash, "fts3_tokenizer"))
   && SQLITE_OK==(rc = sqlite3_overload_function(db, "snippet", -1))
   && SQLITE_OK==(rc = sqlite3_overload_function(db, "offsets", 1))
   && SQLITE_OK==(rc = sqlite3_overload_function(db, "matchinfo", 1))
   && SQLITE_OK==(rc = sqlite3_overload_function(db, "matchinfo", 2))
   && SQLITE_OK==(rc = sqlite3_overload_function(db, "optimize", 1))
  ){
    rc = sqlite3_create_module_v2(
        db, "fts3", &fts3Module, (void *)pHash, hashDestroy
    );
    if( rc==SQLITE_OK ){
      rc = sqlite3_create_module_v2(
          db, "fts4", &fts3Module, (void *)pHash, 0
      );
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts3InitTok(db, (void *)pHash);
    }
    return rc;
  }


  /* An error has occurred. Delete the hash table and return the error code. */
  assert( rc!=SQLITE_OK );
  if( pHash ){
    sqlite3Fts3HashClear(pHash);
    sqlite3_free(pHash);
  }
  return rc;
}

/*
** Allocate an Fts3MultiSegReader for each token in the expression headed
** by pExpr. 
**
** An Fts3SegReader object is a cursor that can seek or scan a range of
** entries within a single segment b-tree. An Fts3MultiSegReader uses multiple
** Fts3SegReader objects internally to provide an interface to seek or scan
** within the union of all segments of a b-tree. Hence the name.
**
** If the allocated Fts3MultiSegReader just seeks to a single entry in a
** segment b-tree (if the term is not a prefix or it is a prefix for which
** there exists prefix b-tree of the right length) then it may be traversed
** and merged incrementally. Otherwise, it has to be merged into an in-memory 
** doclist and then traversed.
*/
static void fts3EvalAllocateReaders(
  Fts3Cursor *pCsr,               /* FTS cursor handle */
  Fts3Expr *pExpr,                /* Allocate readers for this expression */
  int *pnToken,                   /* OUT: Total number of tokens in phrase. */
  int *pnOr,                      /* OUT: Total number of OR nodes in expr. */
  int *pRc                        /* IN/OUT: Error code */
){
  if( pExpr && SQLITE_OK==*pRc ){
    if( pExpr->eType==FTSQUERY_PHRASE ){
      int i;
      int nToken = pExpr->pPhrase->nToken;
      *pnToken += nToken;
      for(i=0; i<nToken; i++){
        Fts3PhraseToken *pToken = &pExpr->pPhrase->aToken[i];
        int rc = fts3TermSegReaderCursor(pCsr, 
            pToken->z, pToken->n, pToken->isPrefix, &pToken->pSegcsr
        );
        if( rc!=SQLITE_OK ){
          *pRc = rc;
          return;
        }
      }
      assert( pExpr->pPhrase->iDoclistToken==0 );
      pExpr->pPhrase->iDoclistToken = -1;
    }else{
      *pnOr += (pExpr->eType==FTSQUERY_OR);
      fts3EvalAllocateReaders(pCsr, pExpr->pLeft, pnToken, pnOr, pRc);
      fts3EvalAllocateReaders(pCsr, pExpr->pRight, pnToken, pnOr, pRc);
    }
  }
}

/*
** Arguments pList/nList contain the doclist for token iToken of phrase p.
** It is merged into the main doclist stored in p->doclist.aAll/nAll.
**
** This function assumes that pList points to a buffer allocated using
** sqlite3_malloc(). This function takes responsibility for eventually
** freeing the buffer.
*/
static void fts3EvalPhraseMergeToken(
  Fts3Table *pTab,                /* FTS Table pointer */
  Fts3Phrase *p,                  /* Phrase to merge pList/nList into */
  int iToken,                     /* Token pList/nList corresponds to */
  char *pList,                    /* Pointer to doclist */
  int nList                       /* Number of bytes in pList */
){
  assert( iToken!=p->iDoclistToken );

  if( pList==0 ){
    sqlite3_free(p->doclist.aAll);
    p->doclist.aAll = 0;
    p->doclist.nAll = 0;
  }

  else if( p->iDoclistToken<0 ){
    p->doclist.aAll = pList;
    p->doclist.nAll = nList;
  }

  else if( p->doclist.aAll==0 ){
    sqlite3_free(pList);
  }

  else {
    char *pLeft;
    char *pRight;
    int nLeft;
    int nRight;
    int nDiff;

    if( p->iDoclistToken<iToken ){
      pLeft = p->doclist.aAll;
      nLeft = p->doclist.nAll;
      pRight = pList;
      nRight = nList;
      nDiff = iToken - p->iDoclistToken;
    }else{
      pRight = p->doclist.aAll;
      nRight = p->doclist.nAll;
      pLeft = pList;
      nLeft = nList;
      nDiff = p->iDoclistToken - iToken;
    }

    fts3DoclistPhraseMerge(pTab->bDescIdx, nDiff, pLeft, nLeft, pRight,&nRight);
    sqlite3_free(pLeft);
    p->doclist.aAll = pRight;
    p->doclist.nAll = nRight;
  }

  if( iToken>p->iDoclistToken ) p->iDoclistToken = iToken;
}

/*
** Load the doclist for phrase p into p->doclist.aAll/nAll. The loaded doclist
** does not take deferred tokens into account.
**
** SQLITE_OK is returned if no error occurs, otherwise an SQLite error code.
*/
static int fts3EvalPhraseLoad(
  Fts3Cursor *pCsr,               /* FTS Cursor handle */
  Fts3Phrase *p                   /* Phrase object */
){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int iToken;
  int rc = SQLITE_OK;

  for(iToken=0; rc==SQLITE_OK && iToken<p->nToken; iToken++){
    Fts3PhraseToken *pToken = &p->aToken[iToken];
    assert( pToken->pDeferred==0 || pToken->pSegcsr==0 );

    if( pToken->pSegcsr ){
      int nThis = 0;
      char *pThis = 0;
      rc = fts3TermSelect(pTab, pToken, p->iColumn, &nThis, &pThis);
      if( rc==SQLITE_OK ){
        fts3EvalPhraseMergeToken(pTab, p, iToken, pThis, nThis);
      }
    }
    assert( pToken->pSegcsr==0 );
  }

  return rc;
}

/*
** This function is called on each phrase after the position lists for
** any deferred tokens have been loaded into memory. It updates the phrases
** current position list to include only those positions that are really
** instances of the phrase (after considering deferred tokens). If this
** means that the phrase does not appear in the current row, doclist.pList
** and doclist.nList are both zeroed.
**
** SQLITE_OK is returned if no error occurs, otherwise an SQLite error code.
*/
static int fts3EvalDeferredPhrase(Fts3Cursor *pCsr, Fts3Phrase *pPhrase){
  int iToken;                     /* Used to iterate through phrase tokens */
  char *aPoslist = 0;             /* Position list for deferred tokens */
  int nPoslist = 0;               /* Number of bytes in aPoslist */
  int iPrev = -1;                 /* Token number of previous deferred token */

  assert( pPhrase->doclist.bFreeList==0 );

  for(iToken=0; iToken<pPhrase->nToken; iToken++){
    Fts3PhraseToken *pToken = &pPhrase->aToken[iToken];
    Fts3DeferredToken *pDeferred = pToken->pDeferred;

    if( pDeferred ){
      char *pList;
      int nList;
      int rc = sqlite3Fts3DeferredTokenList(pDeferred, &pList, &nList);
      if( rc!=SQLITE_OK ) return rc;

      if( pList==0 ){
        sqlite3_free(aPoslist);
        pPhrase->doclist.pList = 0;
        pPhrase->doclist.nList = 0;
        return SQLITE_OK;

      }else if( aPoslist==0 ){
        aPoslist = pList;
        nPoslist = nList;

      }else{
        char *aOut = pList;
        char *p1 = aPoslist;
        char *p2 = aOut;

        assert( iPrev>=0 );
        fts3PoslistPhraseMerge(&aOut, iToken-iPrev, 0, 1, &p1, &p2);
        sqlite3_free(aPoslist);
        aPoslist = pList;
        nPoslist = (int)(aOut - aPoslist);
        if( nPoslist==0 ){
          sqlite3_free(aPoslist);
          pPhrase->doclist.pList = 0;
          pPhrase->doclist.nList = 0;
          return SQLITE_OK;
        }
      }
      iPrev = iToken;
    }
  }

  if( iPrev>=0 ){
    int nMaxUndeferred = pPhrase->iDoclistToken;
    if( nMaxUndeferred<0 ){
      pPhrase->doclist.pList = aPoslist;
      pPhrase->doclist.nList = nPoslist;
      pPhrase->doclist.iDocid = pCsr->iPrevId;
      pPhrase->doclist.bFreeList = 1;
    }else{
      int nDistance;
      char *p1;
      char *p2;
      char *aOut;

      if( nMaxUndeferred>iPrev ){
        p1 = aPoslist;
        p2 = pPhrase->doclist.pList;
        nDistance = nMaxUndeferred - iPrev;
      }else{
        p1 = pPhrase->doclist.pList;
        p2 = aPoslist;
        nDistance = iPrev - nMaxUndeferred;
      }

      aOut = (char *)sqlite3_malloc(nPoslist+8);
      if( !aOut ){
        sqlite3_free(aPoslist);
        return SQLITE_NOMEM;
      }
      
      pPhrase->doclist.pList = aOut;
      if( fts3PoslistPhraseMerge(&aOut, nDistance, 0, 1, &p1, &p2) ){
        pPhrase->doclist.bFreeList = 1;
        pPhrase->doclist.nList = (int)(aOut - pPhrase->doclist.pList);
      }else{
        sqlite3_free(aOut);
        pPhrase->doclist.pList = 0;
        pPhrase->doclist.nList = 0;
      }
      sqlite3_free(aPoslist);
    }
  }

  return SQLITE_OK;
}

/*
** Maximum number of tokens a phrase may have to be considered for the
** incremental doclists strategy.
*/
#define MAX_INCR_PHRASE_TOKENS 4

/*
** This function is called for each Fts3Phrase in a full-text query 
** expression to initialize the mechanism for returning rows. Once this
** function has been called successfully on an Fts3Phrase, it may be
** used with fts3EvalPhraseNext() to iterate through the matching docids.
**
** If parameter bOptOk is true, then the phrase may (or may not) use the
** incremental loading strategy. Otherwise, the entire doclist is loaded into
** memory within this call.
**
** SQLITE_OK is returned if no error occurs, otherwise an SQLite error code.
*/
static int fts3EvalPhraseStart(Fts3Cursor *pCsr, int bOptOk, Fts3Phrase *p){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int rc = SQLITE_OK;             /* Error code */
  int i;

  /* Determine if doclists may be loaded from disk incrementally. This is
  ** possible if the bOptOk argument is true, the FTS doclists will be
  ** scanned in forward order, and the phrase consists of 
  ** MAX_INCR_PHRASE_TOKENS or fewer tokens, none of which are are "^first"
  ** tokens or prefix tokens that cannot use a prefix-index.  */
  int bHaveIncr = 0;
  int bIncrOk = (bOptOk 
   && pCsr->bDesc==pTab->bDescIdx 
   && p->nToken<=MAX_INCR_PHRASE_TOKENS && p->nToken>0
   && p->nToken<=MAX_INCR_PHRASE_TOKENS && p->nToken>0
#ifdef SQLITE_TEST
   && pTab->bNoIncrDoclist==0
#endif
  );
  for(i=0; bIncrOk==1 && i<p->nToken; i++){
    Fts3PhraseToken *pToken = &p->aToken[i];
    if( pToken->bFirst || (pToken->pSegcsr!=0 && !pToken->pSegcsr->bLookup) ){
      bIncrOk = 0;
    }
    if( pToken->pSegcsr ) bHaveIncr = 1;
  }

  if( bIncrOk && bHaveIncr ){
    /* Use the incremental approach. */
    int iCol = (p->iColumn >= pTab->nColumn ? -1 : p->iColumn);
    for(i=0; rc==SQLITE_OK && i<p->nToken; i++){
      Fts3PhraseToken *pToken = &p->aToken[i];
      Fts3MultiSegReader *pSegcsr = pToken->pSegcsr;
      if( pSegcsr ){
        rc = sqlite3Fts3MsrIncrStart(pTab, pSegcsr, iCol, pToken->z, pToken->n);
      }
    }
    p->bIncr = 1;
  }else{
    /* Load the full doclist for the phrase into memory. */
    rc = fts3EvalPhraseLoad(pCsr, p);
    p->bIncr = 0;
  }

  assert( rc!=SQLITE_OK || p->nToken<1 || p->aToken[0].pSegcsr==0 || p->bIncr );
  return rc;
}

/*
** This function is used to iterate backwards (from the end to start) 
** through doclists. It is used by this module to iterate through phrase
** doclists in reverse and by the fts3_write.c module to iterate through
** pending-terms lists when writing to databases with "order=desc".
**
** The doclist may be sorted in ascending (parameter bDescIdx==0) or 
** descending (parameter bDescIdx==1) order of docid. Regardless, this
** function iterates from the end of the doclist to the beginning.
*/
SQLITE_PRIVATE void sqlite3Fts3DoclistPrev(
  int bDescIdx,                   /* True if the doclist is desc */
  char *aDoclist,                 /* Pointer to entire doclist */
  int nDoclist,                   /* Length of aDoclist in bytes */
  char **ppIter,                  /* IN/OUT: Iterator pointer */
  sqlite3_int64 *piDocid,         /* IN/OUT: Docid pointer */
  int *pnList,                    /* OUT: List length pointer */
  u8 *pbEof                       /* OUT: End-of-file flag */
){
  char *p = *ppIter;

  assert( nDoclist>0 );
  assert( *pbEof==0 );
  assert( p || *piDocid==0 );
  assert( !p || (p>aDoclist && p<&aDoclist[nDoclist]) );

  if( p==0 ){
    sqlite3_int64 iDocid = 0;
    char *pNext = 0;
    char *pDocid = aDoclist;
    char *pEnd = &aDoclist[nDoclist];
    int iMul = 1;

    while( pDocid<pEnd ){
      sqlite3_int64 iDelta;
      pDocid += sqlite3Fts3GetVarint(pDocid, &iDelta);
      iDocid += (iMul * iDelta);
      pNext = pDocid;
      fts3PoslistCopy(0, &pDocid);
      while( pDocid<pEnd && *pDocid==0 ) pDocid++;
      iMul = (bDescIdx ? -1 : 1);
    }

    *pnList = (int)(pEnd - pNext);
    *ppIter = pNext;
    *piDocid = iDocid;
  }else{
    int iMul = (bDescIdx ? -1 : 1);
    sqlite3_int64 iDelta;
    fts3GetReverseVarint(&p, aDoclist, &iDelta);
    *piDocid -= (iMul * iDelta);

    if( p==aDoclist ){
      *pbEof = 1;
    }else{
      char *pSave = p;
      fts3ReversePoslist(aDoclist, &p);
      *pnList = (int)(pSave - p);
    }
    *ppIter = p;
  }
}

/*
** Iterate forwards through a doclist.
*/
SQLITE_PRIVATE void sqlite3Fts3DoclistNext(
  int bDescIdx,                   /* True if the doclist is desc */
  char *aDoclist,                 /* Pointer to entire doclist */
  int nDoclist,                   /* Length of aDoclist in bytes */
  char **ppIter,                  /* IN/OUT: Iterator pointer */
  sqlite3_int64 *piDocid,         /* IN/OUT: Docid pointer */
  u8 *pbEof                       /* OUT: End-of-file flag */
){
  char *p = *ppIter;

  assert( nDoclist>0 );
  assert( *pbEof==0 );
  assert( p || *piDocid==0 );
  assert( !p || (p>=aDoclist && p<=&aDoclist[nDoclist]) );

  if( p==0 ){
    p = aDoclist;
    p += sqlite3Fts3GetVarint(p, piDocid);
  }else{
    fts3PoslistCopy(0, &p);
    if( p>=&aDoclist[nDoclist] ){
      *pbEof = 1;
    }else{
      sqlite3_int64 iVar;
      p += sqlite3Fts3GetVarint(p, &iVar);
      *piDocid += ((bDescIdx ? -1 : 1) * iVar);
    }
  }

  *ppIter = p;
}

/*
** Advance the iterator pDL to the next entry in pDL->aAll/nAll. Set *pbEof
** to true if EOF is reached.
*/
static void fts3EvalDlPhraseNext(
  Fts3Table *pTab,
  Fts3Doclist *pDL,
  u8 *pbEof
){
  char *pIter;                            /* Used to iterate through aAll */
  char *pEnd = &pDL->aAll[pDL->nAll];     /* 1 byte past end of aAll */
 
  if( pDL->pNextDocid ){
    pIter = pDL->pNextDocid;
  }else{
    pIter = pDL->aAll;
  }

  if( pIter>=pEnd ){
    /* We have already reached the end of this doclist. EOF. */
    *pbEof = 1;
  }else{
    sqlite3_int64 iDelta;
    pIter += sqlite3Fts3GetVarint(pIter, &iDelta);
    if( pTab->bDescIdx==0 || pDL->pNextDocid==0 ){
      pDL->iDocid += iDelta;
    }else{
      pDL->iDocid -= iDelta;
    }
    pDL->pList = pIter;
    fts3PoslistCopy(0, &pIter);
    pDL->nList = (int)(pIter - pDL->pList);

    /* pIter now points just past the 0x00 that terminates the position-
    ** list for document pDL->iDocid. However, if this position-list was
    ** edited in place by fts3EvalNearTrim(), then pIter may not actually
    ** point to the start of the next docid value. The following line deals
    ** with this case by advancing pIter past the zero-padding added by
    ** fts3EvalNearTrim().  */
    while( pIter<pEnd && *pIter==0 ) pIter++;

    pDL->pNextDocid = pIter;
    assert( pIter>=&pDL->aAll[pDL->nAll] || *pIter );
    *pbEof = 0;
  }
}

/*
** Helper type used by fts3EvalIncrPhraseNext() and incrPhraseTokenNext().
*/
typedef struct TokenDoclist TokenDoclist;
struct TokenDoclist {
  int bIgnore;
  sqlite3_int64 iDocid;
  char *pList;
  int nList;
};

/*
** Token pToken is an incrementally loaded token that is part of a 
** multi-token phrase. Advance it to the next matching document in the
** database and populate output variable *p with the details of the new
** entry. Or, if the iterator has reached EOF, set *pbEof to true.
**
** If an error occurs, return an SQLite error code. Otherwise, return 
** SQLITE_OK.
*/
static int incrPhraseTokenNext(
  Fts3Table *pTab,                /* Virtual table handle */
  Fts3Phrase *pPhrase,            /* Phrase to advance token of */
  int iToken,                     /* Specific token to advance */
  TokenDoclist *p,                /* OUT: Docid and doclist for new entry */
  u8 *pbEof                       /* OUT: True if iterator is at EOF */
){
  int rc = SQLITE_OK;

  if( pPhrase->iDoclistToken==iToken ){
    assert( p->bIgnore==0 );
    assert( pPhrase->aToken[iToken].pSegcsr==0 );
    fts3EvalDlPhraseNext(pTab, &pPhrase->doclist, pbEof);
    p->pList = pPhrase->doclist.pList;
    p->nList = pPhrase->doclist.nList;
    p->iDocid = pPhrase->doclist.iDocid;
  }else{
    Fts3PhraseToken *pToken = &pPhrase->aToken[iToken];
    assert( pToken->pDeferred==0 );
    assert( pToken->pSegcsr || pPhrase->iDoclistToken>=0 );
    if( pToken->pSegcsr ){
      assert( p->bIgnore==0 );
      rc = sqlite3Fts3MsrIncrNext(
          pTab, pToken->pSegcsr, &p->iDocid, &p->pList, &p->nList
      );
      if( p->pList==0 ) *pbEof = 1;
    }else{
      p->bIgnore = 1;
    }
  }

  return rc;
}


/*
** The phrase iterator passed as the second argument:
**
**   * features at least one token that uses an incremental doclist, and 
**
**   * does not contain any deferred tokens.
**
** Advance it to the next matching documnent in the database and populate
** the Fts3Doclist.pList and nList fields. 
**
** If there is no "next" entry and no error occurs, then *pbEof is set to
** 1 before returning. Otherwise, if no error occurs and the iterator is
** successfully advanced, *pbEof is set to 0.
**
** If an error occurs, return an SQLite error code. Otherwise, return 
** SQLITE_OK.
*/
static int fts3EvalIncrPhraseNext(
  Fts3Cursor *pCsr,               /* FTS Cursor handle */
  Fts3Phrase *p,                  /* Phrase object to advance to next docid */
  u8 *pbEof                       /* OUT: Set to 1 if EOF */
){
  int rc = SQLITE_OK;
  Fts3Doclist *pDL = &p->doclist;
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  u8 bEof = 0;

  /* This is only called if it is guaranteed that the phrase has at least
  ** one incremental token. In which case the bIncr flag is set. */
  assert( p->bIncr==1 );

  if( p->nToken==1 && p->bIncr ){
    rc = sqlite3Fts3MsrIncrNext(pTab, p->aToken[0].pSegcsr, 
        &pDL->iDocid, &pDL->pList, &pDL->nList
    );
    if( pDL->pList==0 ) bEof = 1;
  }else{
    int bDescDoclist = pCsr->bDesc;
    struct TokenDoclist a[MAX_INCR_PHRASE_TOKENS];

    memset(a, 0, sizeof(a));
    assert( p->nToken<=MAX_INCR_PHRASE_TOKENS );
    assert( p->iDoclistToken<MAX_INCR_PHRASE_TOKENS );

    while( bEof==0 ){
      int bMaxSet = 0;
      sqlite3_int64 iMax = 0;     /* Largest docid for all iterators */
      int i;                      /* Used to iterate through tokens */

      /* Advance the iterator for each token in the phrase once. */
      for(i=0; rc==SQLITE_OK && i<p->nToken && bEof==0; i++){
        rc = incrPhraseTokenNext(pTab, p, i, &a[i], &bEof);
        if( a[i].bIgnore==0 && (bMaxSet==0 || DOCID_CMP(iMax, a[i].iDocid)<0) ){
          iMax = a[i].iDocid;
          bMaxSet = 1;
        }
      }
      assert( rc!=SQLITE_OK || (p->nToken>=1 && a[p->nToken-1].bIgnore==0) );
      assert( rc!=SQLITE_OK || bMaxSet );

      /* Keep advancing iterators until they all point to the same document */
      for(i=0; i<p->nToken; i++){
        while( rc==SQLITE_OK && bEof==0 
            && a[i].bIgnore==0 && DOCID_CMP(a[i].iDocid, iMax)<0 
        ){
          rc = incrPhraseTokenNext(pTab, p, i, &a[i], &bEof);
          if( DOCID_CMP(a[i].iDocid, iMax)>0 ){
            iMax = a[i].iDocid;
            i = 0;
          }
        }
      }

      /* Check if the current entries really are a phrase match */
      if( bEof==0 ){
        int nList = 0;
        int nByte = a[p->nToken-1].nList;
        char *aDoclist = sqlite3_malloc(nByte+1);
        if( !aDoclist ) return SQLITE_NOMEM;
        memcpy(aDoclist, a[p->nToken-1].pList, nByte+1);

        for(i=0; i<(p->nToken-1); i++){
          if( a[i].bIgnore==0 ){
            char *pL = a[i].pList;
            char *pR = aDoclist;
            char *pOut = aDoclist;
            int nDist = p->nToken-1-i;
            int res = fts3PoslistPhraseMerge(&pOut, nDist, 0, 1, &pL, &pR);
            if( res==0 ) break;
            nList = (int)(pOut - aDoclist);
          }
        }
        if( i==(p->nToken-1) ){
          pDL->iDocid = iMax;
          pDL->pList = aDoclist;
          pDL->nList = nList;
          pDL->bFreeList = 1;
          break;
        }
        sqlite3_free(aDoclist);
      }
    }
  }

  *pbEof = bEof;
  return rc;
}

/*
** Attempt to move the phrase iterator to point to the next matching docid. 
** If an error occurs, return an SQLite error code. Otherwise, return 
** SQLITE_OK.
**
** If there is no "next" entry and no error occurs, then *pbEof is set to
** 1 before returning. Otherwise, if no error occurs and the iterator is
** successfully advanced, *pbEof is set to 0.
*/
static int fts3EvalPhraseNext(
  Fts3Cursor *pCsr,               /* FTS Cursor handle */
  Fts3Phrase *p,                  /* Phrase object to advance to next docid */
  u8 *pbEof                       /* OUT: Set to 1 if EOF */
){
  int rc = SQLITE_OK;
  Fts3Doclist *pDL = &p->doclist;
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;

  if( p->bIncr ){
    rc = fts3EvalIncrPhraseNext(pCsr, p, pbEof);
  }else if( pCsr->bDesc!=pTab->bDescIdx && pDL->nAll ){
    sqlite3Fts3DoclistPrev(pTab->bDescIdx, pDL->aAll, pDL->nAll, 
        &pDL->pNextDocid, &pDL->iDocid, &pDL->nList, pbEof
    );
    pDL->pList = pDL->pNextDocid;
  }else{
    fts3EvalDlPhraseNext(pTab, pDL, pbEof);
  }

  return rc;
}

/*
**
** If *pRc is not SQLITE_OK when this function is called, it is a no-op.
** Otherwise, fts3EvalPhraseStart() is called on all phrases within the
** expression. Also the Fts3Expr.bDeferred variable is set to true for any
** expressions for which all descendent tokens are deferred.
**
** If parameter bOptOk is zero, then it is guaranteed that the
** Fts3Phrase.doclist.aAll/nAll variables contain the entire doclist for
** each phrase in the expression (subject to deferred token processing).
** Or, if bOptOk is non-zero, then one or more tokens within the expression
** may be loaded incrementally, meaning doclist.aAll/nAll is not available.
**
** If an error occurs within this function, *pRc is set to an SQLite error
** code before returning.
*/
static void fts3EvalStartReaders(
  Fts3Cursor *pCsr,               /* FTS Cursor handle */
  Fts3Expr *pExpr,                /* Expression to initialize phrases in */
  int *pRc                        /* IN/OUT: Error code */
){
  if( pExpr && SQLITE_OK==*pRc ){
    if( pExpr->eType==FTSQUERY_PHRASE ){
      int i;
      int nToken = pExpr->pPhrase->nToken;
      for(i=0; i<nToken; i++){
        if( pExpr->pPhrase->aToken[i].pDeferred==0 ) break;
      }
      pExpr->bDeferred = (i==nToken);
      *pRc = fts3EvalPhraseStart(pCsr, 1, pExpr->pPhrase);
    }else{
      fts3EvalStartReaders(pCsr, pExpr->pLeft, pRc);
      fts3EvalStartReaders(pCsr, pExpr->pRight, pRc);
      pExpr->bDeferred = (pExpr->pLeft->bDeferred && pExpr->pRight->bDeferred);
    }
  }
}

/*
** An array of the following structures is assembled as part of the process
** of selecting tokens to defer before the query starts executing (as part
** of the xFilter() method). There is one element in the array for each
** token in the FTS expression.
**
** Tokens are divided into AND/NEAR clusters. All tokens in a cluster belong
** to phrases that are connected only by AND and NEAR operators (not OR or
** NOT). When determining tokens to defer, each AND/NEAR cluster is considered
** separately. The root of a tokens AND/NEAR cluster is stored in 
** Fts3TokenAndCost.pRoot.
*/
typedef struct Fts3TokenAndCost Fts3TokenAndCost;
struct Fts3TokenAndCost {
  Fts3Phrase *pPhrase;            /* The phrase the token belongs to */
  int iToken;                     /* Position of token in phrase */
  Fts3PhraseToken *pToken;        /* The token itself */
  Fts3Expr *pRoot;                /* Root of NEAR/AND cluster */
  int nOvfl;                      /* Number of overflow pages to load doclist */
  int iCol;                       /* The column the token must match */
};

/*
** This function is used to populate an allocated Fts3TokenAndCost array.
**
** If *pRc is not SQLITE_OK when this function is called, it is a no-op.
** Otherwise, if an error occurs during execution, *pRc is set to an
** SQLite error code.
*/
static void fts3EvalTokenCosts(
  Fts3Cursor *pCsr,               /* FTS Cursor handle */
  Fts3Expr *pRoot,                /* Root of current AND/NEAR cluster */
  Fts3Expr *pExpr,                /* Expression to consider */
  Fts3TokenAndCost **ppTC,        /* Write new entries to *(*ppTC)++ */
  Fts3Expr ***ppOr,               /* Write new OR root to *(*ppOr)++ */
  int *pRc                        /* IN/OUT: Error code */
){
  if( *pRc==SQLITE_OK ){
    if( pExpr->eType==FTSQUERY_PHRASE ){
      Fts3Phrase *pPhrase = pExpr->pPhrase;
      int i;
      for(i=0; *pRc==SQLITE_OK && i<pPhrase->nToken; i++){
        Fts3TokenAndCost *pTC = (*ppTC)++;
        pTC->pPhrase = pPhrase;
        pTC->iToken = i;
        pTC->pRoot = pRoot;
        pTC->pToken = &pPhrase->aToken[i];
        pTC->iCol = pPhrase->iColumn;
        *pRc = sqlite3Fts3MsrOvfl(pCsr, pTC->pToken->pSegcsr, &pTC->nOvfl);
      }
    }else if( pExpr->eType!=FTSQUERY_NOT ){
      assert( pExpr->eType==FTSQUERY_OR
           || pExpr->eType==FTSQUERY_AND
           || pExpr->eType==FTSQUERY_NEAR
      );
      assert( pExpr->pLeft && pExpr->pRight );
      if( pExpr->eType==FTSQUERY_OR ){
        pRoot = pExpr->pLeft;
        **ppOr = pRoot;
        (*ppOr)++;
      }
      fts3EvalTokenCosts(pCsr, pRoot, pExpr->pLeft, ppTC, ppOr, pRc);
      if( pExpr->eType==FTSQUERY_OR ){
        pRoot = pExpr->pRight;
        **ppOr = pRoot;
        (*ppOr)++;
      }
      fts3EvalTokenCosts(pCsr, pRoot, pExpr->pRight, ppTC, ppOr, pRc);
    }
  }
}

/*
** Determine the average document (row) size in pages. If successful,
** write this value to *pnPage and return SQLITE_OK. Otherwise, return
** an SQLite error code.
**
** The average document size in pages is calculated by first calculating 
** determining the average size in bytes, B. If B is less than the amount
** of data that will fit on a single leaf page of an intkey table in
** this database, then the average docsize is 1. Otherwise, it is 1 plus
** the number of overflow pages consumed by a record B bytes in size.
*/
static int fts3EvalAverageDocsize(Fts3Cursor *pCsr, int *pnPage){
  if( pCsr->nRowAvg==0 ){
    /* The average document size, which is required to calculate the cost
    ** of each doclist, has not yet been determined. Read the required 
    ** data from the %_stat table to calculate it.
    **
    ** Entry 0 of the %_stat table is a blob containing (nCol+1) FTS3 
    ** varints, where nCol is the number of columns in the FTS3 table.
    ** The first varint is the number of documents currently stored in
    ** the table. The following nCol varints contain the total amount of
    ** data stored in all rows of each column of the table, from left
    ** to right.
    */
    int rc;
    Fts3Table *p = (Fts3Table*)pCsr->base.pVtab;
    sqlite3_stmt *pStmt;
    sqlite3_int64 nDoc = 0;
    sqlite3_int64 nByte = 0;
    const char *pEnd;
    const char *a;

    rc = sqlite3Fts3SelectDoctotal(p, &pStmt);
    if( rc!=SQLITE_OK ) return rc;
    a = sqlite3_column_blob(pStmt, 0);
    assert( a );

    pEnd = &a[sqlite3_column_bytes(pStmt, 0)];
    a += sqlite3Fts3GetVarint(a, &nDoc);
    while( a<pEnd ){
      a += sqlite3Fts3GetVarint(a, &nByte);
    }
    if( nDoc==0 || nByte==0 ){
      sqlite3_reset(pStmt);
      return FTS_CORRUPT_VTAB;
    }

    pCsr->nDoc = nDoc;
    pCsr->nRowAvg = (int)(((nByte / nDoc) + p->nPgsz) / p->nPgsz);
    assert( pCsr->nRowAvg>0 ); 
    rc = sqlite3_reset(pStmt);
    if( rc!=SQLITE_OK ) return rc;
  }

  *pnPage = pCsr->nRowAvg;
  return SQLITE_OK;
}

/*
** This function is called to select the tokens (if any) that will be 
** deferred. The array aTC[] has already been populated when this is
** called.
**
** This function is called once for each AND/NEAR cluster in the 
** expression. Each invocation determines which tokens to defer within
** the cluster with root node pRoot. See comments above the definition
** of struct Fts3TokenAndCost for more details.
**
** If no error occurs, SQLITE_OK is returned and sqlite3Fts3DeferToken()
** called on each token to defer. Otherwise, an SQLite error code is
** returned.
*/
static int fts3EvalSelectDeferred(
  Fts3Cursor *pCsr,               /* FTS Cursor handle */
  Fts3Expr *pRoot,                /* Consider tokens with this root node */
  Fts3TokenAndCost *aTC,          /* Array of expression tokens and costs */
  int nTC                         /* Number of entries in aTC[] */
){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int nDocSize = 0;               /* Number of pages per doc loaded */
  int rc = SQLITE_OK;             /* Return code */
  int ii;                         /* Iterator variable for various purposes */
  int nOvfl = 0;                  /* Total overflow pages used by doclists */
  int nToken = 0;                 /* Total number of tokens in cluster */

  int nMinEst = 0;                /* The minimum count for any phrase so far. */
  int nLoad4 = 1;                 /* (Phrases that will be loaded)^4. */

  /* Tokens are never deferred for FTS tables created using the content=xxx
  ** option. The reason being that it is not guaranteed that the content
  ** table actually contains the same data as the index. To prevent this from
  ** causing any problems, the deferred token optimization is completely
  ** disabled for content=xxx tables. */
  if( pTab->zContentTbl ){
    return SQLITE_OK;
  }

  /* Count the tokens in this AND/NEAR cluster. If none of the doclists
  ** associated with the tokens spill onto overflow pages, or if there is
  ** only 1 token, exit early. No tokens to defer in this case. */
  for(ii=0; ii<nTC; ii++){
    if( aTC[ii].pRoot==pRoot ){
      nOvfl += aTC[ii].nOvfl;
      nToken++;
    }
  }
  if( nOvfl==0 || nToken<2 ) return SQLITE_OK;

  /* Obtain the average docsize (in pages). */
  rc = fts3EvalAverageDocsize(pCsr, &nDocSize);
  assert( rc!=SQLITE_OK || nDocSize>0 );


  /* Iterate through all tokens in this AND/NEAR cluster, in ascending order 
  ** of the number of overflow pages that will be loaded by the pager layer 
  ** to retrieve the entire doclist for the token from the full-text index.
  ** Load the doclists for tokens that are either:
  **
  **   a. The cheapest token in the entire query (i.e. the one visited by the
  **      first iteration of this loop), or
  **
  **   b. Part of a multi-token phrase.
  **
  ** After each token doclist is loaded, merge it with the others from the
  ** same phrase and count the number of documents that the merged doclist
  ** contains. Set variable "nMinEst" to the smallest number of documents in 
  ** any phrase doclist for which 1 or more token doclists have been loaded.
  ** Let nOther be the number of other phrases for which it is certain that
  ** one or more tokens will not be deferred.
  **
  ** Then, for each token, defer it if loading the doclist would result in
  ** loading N or more overflow pages into memory, where N is computed as:
  **
  **    (nMinEst + 4^nOther - 1) / (4^nOther)
  */
  for(ii=0; ii<nToken && rc==SQLITE_OK; ii++){
    int iTC;                      /* Used to iterate through aTC[] array. */
    Fts3TokenAndCost *pTC = 0;    /* Set to cheapest remaining token. */

    /* Set pTC to point to the cheapest remaining token. */
    for(iTC=0; iTC<nTC; iTC++){
      if( aTC[iTC].pToken && aTC[iTC].pRoot==pRoot 
       && (!pTC || aTC[iTC].nOvfl<pTC->nOvfl) 
      ){
        pTC = &aTC[iTC];
      }
    }
    assert( pTC );

    if( ii && pTC->nOvfl>=((nMinEst+(nLoad4/4)-1)/(nLoad4/4))*nDocSize ){
      /* The number of overflow pages to load for this (and therefore all
      ** subsequent) tokens is greater than the estimated number of pages 
      ** that will be loaded if all subsequent tokens are deferred.
      */
      Fts3PhraseToken *pToken = pTC->pToken;
      rc = sqlite3Fts3DeferToken(pCsr, pToken, pTC->iCol);
      fts3SegReaderCursorFree(pToken->pSegcsr);
      pToken->pSegcsr = 0;
    }else{
      /* Set nLoad4 to the value of (4^nOther) for the next iteration of the
      ** for-loop. Except, limit the value to 2^24 to prevent it from 
      ** overflowing the 32-bit integer it is stored in. */
      if( ii<12 ) nLoad4 = nLoad4*4;

      if( ii==0 || (pTC->pPhrase->nToken>1 && ii!=nToken-1) ){
        /* Either this is the cheapest token in the entire query, or it is
        ** part of a multi-token phrase. Either way, the entire doclist will
        ** (eventually) be loaded into memory. It may as well be now. */
        Fts3PhraseToken *pToken = pTC->pToken;
        int nList = 0;
        char *pList = 0;
        rc = fts3TermSelect(pTab, pToken, pTC->iCol, &nList, &pList);
        assert( rc==SQLITE_OK || pList==0 );
        if( rc==SQLITE_OK ){
          int nCount;
          fts3EvalPhraseMergeToken(pTab, pTC->pPhrase, pTC->iToken,pList,nList);
          nCount = fts3DoclistCountDocids(
              pTC->pPhrase->doclist.aAll, pTC->pPhrase->doclist.nAll
          );
          if( ii==0 || nCount<nMinEst ) nMinEst = nCount;
        }
      }
    }
    pTC->pToken = 0;
  }

  return rc;
}

/*
** This function is called from within the xFilter method. It initializes
** the full-text query currently stored in pCsr->pExpr. To iterate through
** the results of a query, the caller does:
**
**    fts3EvalStart(pCsr);
**    while( 1 ){
**      fts3EvalNext(pCsr);
**      if( pCsr->bEof ) break;
**      ... return row pCsr->iPrevId to the caller ...
**    }
*/
static int fts3EvalStart(Fts3Cursor *pCsr){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int rc = SQLITE_OK;
  int nToken = 0;
  int nOr = 0;

  /* Allocate a MultiSegReader for each token in the expression. */
  fts3EvalAllocateReaders(pCsr, pCsr->pExpr, &nToken, &nOr, &rc);

  /* Determine which, if any, tokens in the expression should be deferred. */
#ifndef SQLITE_DISABLE_FTS4_DEFERRED
  if( rc==SQLITE_OK && nToken>1 && pTab->bFts4 ){
    Fts3TokenAndCost *aTC;
    Fts3Expr **apOr;
    aTC = (Fts3TokenAndCost *)sqlite3_malloc(
        sizeof(Fts3TokenAndCost) * nToken
      + sizeof(Fts3Expr *) * nOr * 2
    );
    apOr = (Fts3Expr **)&aTC[nToken];

    if( !aTC ){
      rc = SQLITE_NOMEM;
    }else{
      int ii;
      Fts3TokenAndCost *pTC = aTC;
      Fts3Expr **ppOr = apOr;

      fts3EvalTokenCosts(pCsr, 0, pCsr->pExpr, &pTC, &ppOr, &rc);
      nToken = (int)(pTC-aTC);
      nOr = (int)(ppOr-apOr);

      if( rc==SQLITE_OK ){
        rc = fts3EvalSelectDeferred(pCsr, 0, aTC, nToken);
        for(ii=0; rc==SQLITE_OK && ii<nOr; ii++){
          rc = fts3EvalSelectDeferred(pCsr, apOr[ii], aTC, nToken);
        }
      }

      sqlite3_free(aTC);
    }
  }
#endif

  fts3EvalStartReaders(pCsr, pCsr->pExpr, &rc);
  return rc;
}

/*
** Invalidate the current position list for phrase pPhrase.
*/
static void fts3EvalInvalidatePoslist(Fts3Phrase *pPhrase){
  if( pPhrase->doclist.bFreeList ){
    sqlite3_free(pPhrase->doclist.pList);
  }
  pPhrase->doclist.pList = 0;
  pPhrase->doclist.nList = 0;
  pPhrase->doclist.bFreeList = 0;
}

/*
** This function is called to edit the position list associated with
** the phrase object passed as the fifth argument according to a NEAR
** condition. For example:
**
**     abc NEAR/5 "def ghi"
**
** Parameter nNear is passed the NEAR distance of the expression (5 in
** the example above). When this function is called, *paPoslist points to
** the position list, and *pnToken is the number of phrase tokens in, the
** phrase on the other side of the NEAR operator to pPhrase. For example,
** if pPhrase refers to the "def ghi" phrase, then *paPoslist points to
** the position list associated with phrase "abc".
**
** All positions in the pPhrase position list that are not sufficiently
** close to a position in the *paPoslist position list are removed. If this
** leaves 0 positions, zero is returned. Otherwise, non-zero.
**
** Before returning, *paPoslist is set to point to the position lsit 
** associated with pPhrase. And *pnToken is set to the number of tokens in
** pPhrase.
*/
static int fts3EvalNearTrim(
  int nNear,                      /* NEAR distance. As in "NEAR/nNear". */
  char *aTmp,                     /* Temporary space to use */
  char **paPoslist,               /* IN/OUT: Position list */
  int *pnToken,                   /* IN/OUT: Tokens in phrase of *paPoslist */
  Fts3Phrase *pPhrase             /* The phrase object to trim the doclist of */
){
  int nParam1 = nNear + pPhrase->nToken;
  int nParam2 = nNear + *pnToken;
  int nNew;
  char *p2; 
  char *pOut; 
  int res;

  assert( pPhrase->doclist.pList );

  p2 = pOut = pPhrase->doclist.pList;
  res = fts3PoslistNearMerge(
    &pOut, aTmp, nParam1, nParam2, paPoslist, &p2
  );
  if( res ){
    nNew = (int)(pOut - pPhrase->doclist.pList) - 1;
    assert( pPhrase->doclist.pList[nNew]=='\0' );
    assert( nNew<=pPhrase->doclist.nList && nNew>0 );
    memset(&pPhrase->doclist.pList[nNew], 0, pPhrase->doclist.nList - nNew);
    pPhrase->doclist.nList = nNew;
    *paPoslist = pPhrase->doclist.pList;
    *pnToken = pPhrase->nToken;
  }

  return res;
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is called.
** Otherwise, it advances the expression passed as the second argument to
** point to the next matching row in the database. Expressions iterate through
** matching rows in docid order. Ascending order if Fts3Cursor.bDesc is zero,
** or descending if it is non-zero.
**
** If an error occurs, *pRc is set to an SQLite error code. Otherwise, if
** successful, the following variables in pExpr are set:
**
**   Fts3Expr.bEof                (non-zero if EOF - there is no next row)
**   Fts3Expr.iDocid              (valid if bEof==0. The docid of the next row)
**
** If the expression is of type FTSQUERY_PHRASE, and the expression is not
** at EOF, then the following variables are populated with the position list
** for the phrase for the visited row:
**
**   FTs3Expr.pPhrase->doclist.nList        (length of pList in bytes)
**   FTs3Expr.pPhrase->doclist.pList        (pointer to position list)
**
** It says above that this function advances the expression to the next
** matching row. This is usually true, but there are the following exceptions:
**
**   1. Deferred tokens are not taken into account. If a phrase consists
**      entirely of deferred tokens, it is assumed to match every row in
**      the db. In this case the position-list is not populated at all. 
**
**      Or, if a phrase contains one or more deferred tokens and one or
**      more non-deferred tokens, then the expression is advanced to the 
**      next possible match, considering only non-deferred tokens. In other
**      words, if the phrase is "A B C", and "B" is deferred, the expression
**      is advanced to the next row that contains an instance of "A * C", 
**      where "*" may match any single token. The position list in this case
**      is populated as for "A * C" before returning.
**
**   2. NEAR is treated as AND. If the expression is "x NEAR y", it is 
**      advanced to point to the next row that matches "x AND y".
** 
** See fts3EvalTestDeferredAndNear() for details on testing if a row is
** really a match, taking into account deferred tokens and NEAR operators.
*/
static void fts3EvalNextRow(
  Fts3Cursor *pCsr,               /* FTS Cursor handle */
  Fts3Expr *pExpr,                /* Expr. to advance to next matching row */
  int *pRc                        /* IN/OUT: Error code */
){
  if( *pRc==SQLITE_OK ){
    int bDescDoclist = pCsr->bDesc;         /* Used by DOCID_CMP() macro */
    assert( pExpr->bEof==0 );
    pExpr->bStart = 1;

    switch( pExpr->eType ){
      case FTSQUERY_NEAR:
      case FTSQUERY_AND: {
        Fts3Expr *pLeft = pExpr->pLeft;
        Fts3Expr *pRight = pExpr->pRight;
        assert( !pLeft->bDeferred || !pRight->bDeferred );

        if( pLeft->bDeferred ){
          /* LHS is entirely deferred. So we assume it matches every row.
          ** Advance the RHS iterator to find the next row visited. */
          fts3EvalNextRow(pCsr, pRight, pRc);
          pExpr->iDocid = pRight->iDocid;
          pExpr->bEof = pRight->bEof;
        }else if( pRight->bDeferred ){
          /* RHS is entirely deferred. So we assume it matches every row.
          ** Advance the LHS iterator to find the next row visited. */
          fts3EvalNextRow(pCsr, pLeft, pRc);
          pExpr->iDocid = pLeft->iDocid;
          pExpr->bEof = pLeft->bEof;
        }else{
          /* Neither the RHS or LHS are deferred. */
          fts3EvalNextRow(pCsr, pLeft, pRc);
          fts3EvalNextRow(pCsr, pRight, pRc);
          while( !pLeft->bEof && !pRight->bEof && *pRc==SQLITE_OK ){
            sqlite3_int64 iDiff = DOCID_CMP(pLeft->iDocid, pRight->iDocid);
            if( iDiff==0 ) break;
            if( iDiff<0 ){
              fts3EvalNextRow(pCsr, pLeft, pRc);
            }else{
              fts3EvalNextRow(pCsr, pRight, pRc);
            }
          }
          pExpr->iDocid = pLeft->iDocid;
          pExpr->bEof = (pLeft->bEof || pRight->bEof);
        }
        break;
      }
  
      case FTSQUERY_OR: {
        Fts3Expr *pLeft = pExpr->pLeft;
        Fts3Expr *pRight = pExpr->pRight;
        sqlite3_int64 iCmp = DOCID_CMP(pLeft->iDocid, pRight->iDocid);

        assert( pLeft->bStart || pLeft->iDocid==pRight->iDocid );
        assert( pRight->bStart || pLeft->iDocid==pRight->iDocid );

        if( pRight->bEof || (pLeft->bEof==0 && iCmp<0) ){
          fts3EvalNextRow(pCsr, pLeft, pRc);
        }else if( pLeft->bEof || (pRight->bEof==0 && iCmp>0) ){
          fts3EvalNextRow(pCsr, pRight, pRc);
        }else{
          fts3EvalNextRow(pCsr, pLeft, pRc);
          fts3EvalNextRow(pCsr, pRight, pRc);
        }

        pExpr->bEof = (pLeft->bEof && pRight->bEof);
        iCmp = DOCID_CMP(pLeft->iDocid, pRight->iDocid);
        if( pRight->bEof || (pLeft->bEof==0 &&  iCmp<0) ){
          pExpr->iDocid = pLeft->iDocid;
        }else{
          pExpr->iDocid = pRight->iDocid;
        }

        break;
      }

      case FTSQUERY_NOT: {
        Fts3Expr *pLeft = pExpr->pLeft;
        Fts3Expr *pRight = pExpr->pRight;

        if( pRight->bStart==0 ){
          fts3EvalNextRow(pCsr, pRight, pRc);
          assert( *pRc!=SQLITE_OK || pRight->bStart );
        }

        fts3EvalNextRow(pCsr, pLeft, pRc);
        if( pLeft->bEof==0 ){
          while( !*pRc 
              && !pRight->bEof 
              && DOCID_CMP(pLeft->iDocid, pRight->iDocid)>0 
          ){
            fts3EvalNextRow(pCsr, pRight, pRc);
          }
        }
        pExpr->iDocid = pLeft->iDocid;
        pExpr->bEof = pLeft->bEof;
        break;
      }

      default: {
        Fts3Phrase *pPhrase = pExpr->pPhrase;
        fts3EvalInvalidatePoslist(pPhrase);
        *pRc = fts3EvalPhraseNext(pCsr, pPhrase, &pExpr->bEof);
        pExpr->iDocid = pPhrase->doclist.iDocid;
        break;
      }
    }
  }
}

/*
** If *pRc is not SQLITE_OK, or if pExpr is not the root node of a NEAR
** cluster, then this function returns 1 immediately.
**
** Otherwise, it checks if the current row really does match the NEAR 
** expression, using the data currently stored in the position lists 
** (Fts3Expr->pPhrase.doclist.pList/nList) for each phrase in the expression. 
**
** If the current row is a match, the position list associated with each
** phrase in the NEAR expression is edited in place to contain only those
** phrase instances sufficiently close to their peers to satisfy all NEAR
** constraints. In this case it returns 1. If the NEAR expression does not 
** match the current row, 0 is returned. The position lists may or may not
** be edited if 0 is returned.
*/
static int fts3EvalNearTest(Fts3Expr *pExpr, int *pRc){
  int res = 1;

  /* The following block runs if pExpr is the root of a NEAR query.
  ** For example, the query:
  **
  **         "w" NEAR "x" NEAR "y" NEAR "z"
  **
  ** which is represented in tree form as:
  **
  **                               |
  **                          +--NEAR--+      <-- root of NEAR query
  **                          |        |
  **                     +--NEAR--+   "z"
  **                     |        |
  **                +--NEAR--+   "y"
  **                |        |
  **               "w"      "x"
  **
  ** The right-hand child of a NEAR node is always a phrase. The 
  ** left-hand child may be either a phrase or a NEAR node. There are
  ** no exceptions to this - it's the way the parser in fts3_expr.c works.
  */
  if( *pRc==SQLITE_OK 
   && pExpr->eType==FTSQUERY_NEAR 
   && pExpr->bEof==0
   && (pExpr->pParent==0 || pExpr->pParent->eType!=FTSQUERY_NEAR)
  ){
    Fts3Expr *p; 
    int nTmp = 0;                 /* Bytes of temp space */
    char *aTmp;                   /* Temp space for PoslistNearMerge() */

    /* Allocate temporary working space. */
    for(p=pExpr; p->pLeft; p=p->pLeft){
      nTmp += p->pRight->pPhrase->doclist.nList;
    }
    nTmp += p->pPhrase->doclist.nList;
    if( nTmp==0 ){
      res = 0;
    }else{
      aTmp = sqlite3_malloc(nTmp*2);
      if( !aTmp ){
        *pRc = SQLITE_NOMEM;
        res = 0;
      }else{
        char *aPoslist = p->pPhrase->doclist.pList;
        int nToken = p->pPhrase->nToken;

        for(p=p->pParent;res && p && p->eType==FTSQUERY_NEAR; p=p->pParent){
          Fts3Phrase *pPhrase = p->pRight->pPhrase;
          int nNear = p->nNear;
          res = fts3EvalNearTrim(nNear, aTmp, &aPoslist, &nToken, pPhrase);
        }

        aPoslist = pExpr->pRight->pPhrase->doclist.pList;
        nToken = pExpr->pRight->pPhrase->nToken;
        for(p=pExpr->pLeft; p && res; p=p->pLeft){
          int nNear;
          Fts3Phrase *pPhrase;
          assert( p->pParent && p->pParent->pLeft==p );
          nNear = p->pParent->nNear;
          pPhrase = (
              p->eType==FTSQUERY_NEAR ? p->pRight->pPhrase : p->pPhrase
              );
          res = fts3EvalNearTrim(nNear, aTmp, &aPoslist, &nToken, pPhrase);
        }
      }

      sqlite3_free(aTmp);
    }
  }

  return res;
}

/*
** This function is a helper function for fts3EvalTestDeferredAndNear().
** Assuming no error occurs or has occurred, It returns non-zero if the
** expression passed as the second argument matches the row that pCsr 
** currently points to, or zero if it does not.
**
** If *pRc is not SQLITE_OK when this function is called, it is a no-op.
** If an error occurs during execution of this function, *pRc is set to 
** the appropriate SQLite error code. In this case the returned value is 
** undefined.
*/
static int fts3EvalTestExpr(
  Fts3Cursor *pCsr,               /* FTS cursor handle */
  Fts3Expr *pExpr,                /* Expr to test. May or may not be root. */
  int *pRc                        /* IN/OUT: Error code */
){
  int bHit = 1;                   /* Return value */
  if( *pRc==SQLITE_OK ){
    switch( pExpr->eType ){
      case FTSQUERY_NEAR:
      case FTSQUERY_AND:
        bHit = (
            fts3EvalTestExpr(pCsr, pExpr->pLeft, pRc)
         && fts3EvalTestExpr(pCsr, pExpr->pRight, pRc)
         && fts3EvalNearTest(pExpr, pRc)
        );

        /* If the NEAR expression does not match any rows, zero the doclist for 
        ** all phrases involved in the NEAR. This is because the snippet(),
        ** offsets() and matchinfo() functions are not supposed to recognize 
        ** any instances of phrases that are part of unmatched NEAR queries. 
        ** For example if this expression:
        **
        **    ... MATCH 'a OR (b NEAR c)'
        **
        ** is matched against a row containing:
        **
        **        'a b d e'
        **
        ** then any snippet() should ony highlight the "a" term, not the "b"
        ** (as "b" is part of a non-matching NEAR clause).
        */
        if( bHit==0 
         && pExpr->eType==FTSQUERY_NEAR 
         && (pExpr->pParent==0 || pExpr->pParent->eType!=FTSQUERY_NEAR)
        ){
          Fts3Expr *p;
          for(p=pExpr; p->pPhrase==0; p=p->pLeft){
            if( p->pRight->iDocid==pCsr->iPrevId ){
              fts3EvalInvalidatePoslist(p->pRight->pPhrase);
            }
          }
          if( p->iDocid==pCsr->iPrevId ){
            fts3EvalInvalidatePoslist(p->pPhrase);
          }
        }

        break;

      case FTSQUERY_OR: {
        int bHit1 = fts3EvalTestExpr(pCsr, pExpr->pLeft, pRc);
        int bHit2 = fts3EvalTestExpr(pCsr, pExpr->pRight, pRc);
        bHit = bHit1 || bHit2;
        break;
      }

      case FTSQUERY_NOT:
        bHit = (
            fts3EvalTestExpr(pCsr, pExpr->pLeft, pRc)
         && !fts3EvalTestExpr(pCsr, pExpr->pRight, pRc)
        );
        break;

      default: {
#ifndef SQLITE_DISABLE_FTS4_DEFERRED
        if( pCsr->pDeferred 
         && (pExpr->iDocid==pCsr->iPrevId || pExpr->bDeferred)
        ){
          Fts3Phrase *pPhrase = pExpr->pPhrase;
          assert( pExpr->bDeferred || pPhrase->doclist.bFreeList==0 );
          if( pExpr->bDeferred ){
            fts3EvalInvalidatePoslist(pPhrase);
          }
          *pRc = fts3EvalDeferredPhrase(pCsr, pPhrase);
          bHit = (pPhrase->doclist.pList!=0);
          pExpr->iDocid = pCsr->iPrevId;
        }else
#endif
        {
          bHit = (pExpr->bEof==0 && pExpr->iDocid==pCsr->iPrevId);
        }
        break;
      }
    }
  }
  return bHit;
}

/*
** This function is called as the second part of each xNext operation when
** iterating through the results of a full-text query. At this point the
** cursor points to a row that matches the query expression, with the
** following caveats:
**
**   * Up until this point, "NEAR" operators in the expression have been
**     treated as "AND".
**
**   * Deferred tokens have not yet been considered.
**
** If *pRc is not SQLITE_OK when this function is called, it immediately
** returns 0. Otherwise, it tests whether or not after considering NEAR
** operators and deferred tokens the current row is still a match for the
** expression. It returns 1 if both of the following are true:
**
**   1. *pRc is SQLITE_OK when this function returns, and
**
**   2. After scanning the current FTS table row for the deferred tokens,
**      it is determined that the row does *not* match the query.
**
** Or, if no error occurs and it seems the current row does match the FTS
** query, return 0.
*/
static int fts3EvalTestDeferredAndNear(Fts3Cursor *pCsr, int *pRc){
  int rc = *pRc;
  int bMiss = 0;
  if( rc==SQLITE_OK ){

    /* If there are one or more deferred tokens, load the current row into
    ** memory and scan it to determine the position list for each deferred
    ** token. Then, see if this row is really a match, considering deferred
    ** tokens and NEAR operators (neither of which were taken into account
    ** earlier, by fts3EvalNextRow()). 
    */
    if( pCsr->pDeferred ){
      rc = fts3CursorSeek(0, pCsr);
      if( rc==SQLITE_OK ){
        rc = sqlite3Fts3CacheDeferredDoclists(pCsr);
      }
    }
    bMiss = (0==fts3EvalTestExpr(pCsr, pCsr->pExpr, &rc));

    /* Free the position-lists accumulated for each deferred token above. */
    sqlite3Fts3FreeDeferredDoclists(pCsr);
    *pRc = rc;
  }
  return (rc==SQLITE_OK && bMiss);
}

/*
** Advance to the next document that matches the FTS expression in
** Fts3Cursor.pExpr.
*/
static int fts3EvalNext(Fts3Cursor *pCsr){
  int rc = SQLITE_OK;             /* Return Code */
  Fts3Expr *pExpr = pCsr->pExpr;
  assert( pCsr->isEof==0 );
  if( pExpr==0 ){
    pCsr->isEof = 1;
  }else{
    do {
      if( pCsr->isRequireSeek==0 ){
        sqlite3_reset(pCsr->pStmt);
      }
      assert( sqlite3_data_count(pCsr->pStmt)==0 );
      fts3EvalNextRow(pCsr, pExpr, &rc);
      pCsr->isEof = pExpr->bEof;
      pCsr->isRequireSeek = 1;
      pCsr->isMatchinfoNeeded = 1;
      pCsr->iPrevId = pExpr->iDocid;
    }while( pCsr->isEof==0 && fts3EvalTestDeferredAndNear(pCsr, &rc) );
  }

  /* Check if the cursor is past the end of the docid range specified
  ** by Fts3Cursor.iMinDocid/iMaxDocid. If so, set the EOF flag.  */
  if( rc==SQLITE_OK && (
        (pCsr->bDesc==0 && pCsr->iPrevId>pCsr->iMaxDocid)
     || (pCsr->bDesc!=0 && pCsr->iPrevId<pCsr->iMinDocid)
  )){
    pCsr->isEof = 1;
  }

  return rc;
}

/*
** Restart interation for expression pExpr so that the next call to
** fts3EvalNext() visits the first row. Do not allow incremental 
** loading or merging of phrase doclists for this iteration.
**
** If *pRc is other than SQLITE_OK when this function is called, it is
** a no-op. If an error occurs within this function, *pRc is set to an
** SQLite error code before returning.
*/
static void fts3EvalRestart(
  Fts3Cursor *pCsr,
  Fts3Expr *pExpr,
  int *pRc
){
  if( pExpr && *pRc==SQLITE_OK ){
    Fts3Phrase *pPhrase = pExpr->pPhrase;

    if( pPhrase ){
      fts3EvalInvalidatePoslist(pPhrase);
      if( pPhrase->bIncr ){
        int i;
        for(i=0; i<pPhrase->nToken; i++){
          Fts3PhraseToken *pToken = &pPhrase->aToken[i];
          assert( pToken->pDeferred==0 );
          if( pToken->pSegcsr ){
            sqlite3Fts3MsrIncrRestart(pToken->pSegcsr);
          }
        }
        *pRc = fts3EvalPhraseStart(pCsr, 0, pPhrase);
      }
      pPhrase->doclist.pNextDocid = 0;
      pPhrase->doclist.iDocid = 0;
    }

    pExpr->iDocid = 0;
    pExpr->bEof = 0;
    pExpr->bStart = 0;

    fts3EvalRestart(pCsr, pExpr->pLeft, pRc);
    fts3EvalRestart(pCsr, pExpr->pRight, pRc);
  }
}

/*
** After allocating the Fts3Expr.aMI[] array for each phrase in the 
** expression rooted at pExpr, the cursor iterates through all rows matched
** by pExpr, calling this function for each row. This function increments
** the values in Fts3Expr.aMI[] according to the position-list currently
** found in Fts3Expr.pPhrase->doclist.pList for each of the phrase 
** expression nodes.
*/
static void fts3EvalUpdateCounts(Fts3Expr *pExpr){
  if( pExpr ){
    Fts3Phrase *pPhrase = pExpr->pPhrase;
    if( pPhrase && pPhrase->doclist.pList ){
      int iCol = 0;
      char *p = pPhrase->doclist.pList;

      assert( *p );
      while( 1 ){
        u8 c = 0;
        int iCnt = 0;
        while( 0xFE & (*p | c) ){
          if( (c&0x80)==0 ) iCnt++;
          c = *p++ & 0x80;
        }

        /* aMI[iCol*3 + 1] = Number of occurrences
        ** aMI[iCol*3 + 2] = Number of rows containing at least one instance
        */
        pExpr->aMI[iCol*3 + 1] += iCnt;
        pExpr->aMI[iCol*3 + 2] += (iCnt>0);
        if( *p==0x00 ) break;
        p++;
        p += fts3GetVarint32(p, &iCol);
      }
    }

    fts3EvalUpdateCounts(pExpr->pLeft);
    fts3EvalUpdateCounts(pExpr->pRight);
  }
}

/*
** Expression pExpr must be of type FTSQUERY_PHRASE.
**
** If it is not already allocated and populated, this function allocates and
** populates the Fts3Expr.aMI[] array for expression pExpr. If pExpr is part
** of a NEAR expression, then it also allocates and populates the same array
** for all other phrases that are part of the NEAR expression.
**
** SQLITE_OK is returned if the aMI[] array is successfully allocated and
** populated. Otherwise, if an error occurs, an SQLite error code is returned.
*/
static int fts3EvalGatherStats(
  Fts3Cursor *pCsr,               /* Cursor object */
  Fts3Expr *pExpr                 /* FTSQUERY_PHRASE expression */
){
  int rc = SQLITE_OK;             /* Return code */

  assert( pExpr->eType==FTSQUERY_PHRASE );
  if( pExpr->aMI==0 ){
    Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
    Fts3Expr *pRoot;                /* Root of NEAR expression */
    Fts3Expr *p;                    /* Iterator used for several purposes */

    sqlite3_int64 iPrevId = pCsr->iPrevId;
    sqlite3_int64 iDocid;
    u8 bEof;

    /* Find the root of the NEAR expression */
    pRoot = pExpr;
    while( pRoot->pParent && pRoot->pParent->eType==FTSQUERY_NEAR ){
      pRoot = pRoot->pParent;
    }
    iDocid = pRoot->iDocid;
    bEof = pRoot->bEof;
    assert( pRoot->bStart );

    /* Allocate space for the aMSI[] array of each FTSQUERY_PHRASE node */
    for(p=pRoot; p; p=p->pLeft){
      Fts3Expr *pE = (p->eType==FTSQUERY_PHRASE?p:p->pRight);
      assert( pE->aMI==0 );
      pE->aMI = (u32 *)sqlite3_malloc(pTab->nColumn * 3 * sizeof(u32));
      if( !pE->aMI ) return SQLITE_NOMEM;
      memset(pE->aMI, 0, pTab->nColumn * 3 * sizeof(u32));
    }

    fts3EvalRestart(pCsr, pRoot, &rc);

    while( pCsr->isEof==0 && rc==SQLITE_OK ){

      do {
        /* Ensure the %_content statement is reset. */
        if( pCsr->isRequireSeek==0 ) sqlite3_reset(pCsr->pStmt);
        assert( sqlite3_data_count(pCsr->pStmt)==0 );

        /* Advance to the next document */
        fts3EvalNextRow(pCsr, pRoot, &rc);
        pCsr->isEof = pRoot->bEof;
        pCsr->isRequireSeek = 1;
        pCsr->isMatchinfoNeeded = 1;
        pCsr->iPrevId = pRoot->iDocid;
      }while( pCsr->isEof==0 
           && pRoot->eType==FTSQUERY_NEAR 
           && fts3EvalTestDeferredAndNear(pCsr, &rc) 
      );

      if( rc==SQLITE_OK && pCsr->isEof==0 ){
        fts3EvalUpdateCounts(pRoot);
      }
    }

    pCsr->isEof = 0;
    pCsr->iPrevId = iPrevId;

    if( bEof ){
      pRoot->bEof = bEof;
    }else{
      /* Caution: pRoot may iterate through docids in ascending or descending
      ** order. For this reason, even though it seems more defensive, the 
      ** do loop can not be written:
      **
      **   do {...} while( pRoot->iDocid<iDocid && rc==SQLITE_OK );
      */
      fts3EvalRestart(pCsr, pRoot, &rc);
      do {
        fts3EvalNextRow(pCsr, pRoot, &rc);
        assert( pRoot->bEof==0 );
      }while( pRoot->iDocid!=iDocid && rc==SQLITE_OK );
      fts3EvalTestDeferredAndNear(pCsr, &rc);
    }
  }
  return rc;
}

/*
** This function is used by the matchinfo() module to query a phrase 
** expression node for the following information:
**
**   1. The total number of occurrences of the phrase in each column of 
**      the FTS table (considering all rows), and
**
**   2. For each column, the number of rows in the table for which the
**      column contains at least one instance of the phrase.
**
** If no error occurs, SQLITE_OK is returned and the values for each column
** written into the array aiOut as follows:
**
**   aiOut[iCol*3 + 1] = Number of occurrences
**   aiOut[iCol*3 + 2] = Number of rows containing at least one instance
**
** Caveats:
**
**   * If a phrase consists entirely of deferred tokens, then all output 
**     values are set to the number of documents in the table. In other
**     words we assume that very common tokens occur exactly once in each 
**     column of each row of the table.
**
**   * If a phrase contains some deferred tokens (and some non-deferred 
**     tokens), count the potential occurrence identified by considering
**     the non-deferred tokens instead of actual phrase occurrences.
**
**   * If the phrase is part of a NEAR expression, then only phrase instances
**     that meet the NEAR constraint are included in the counts.
*/
SQLITE_PRIVATE int sqlite3Fts3EvalPhraseStats(
  Fts3Cursor *pCsr,               /* FTS cursor handle */
  Fts3Expr *pExpr,                /* Phrase expression */
  u32 *aiOut                      /* Array to write results into (see above) */
){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int rc = SQLITE_OK;
  int iCol;

  if( pExpr->bDeferred && pExpr->pParent->eType!=FTSQUERY_NEAR ){
    assert( pCsr->nDoc>0 );
    for(iCol=0; iCol<pTab->nColumn; iCol++){
      aiOut[iCol*3 + 1] = (u32)pCsr->nDoc;
      aiOut[iCol*3 + 2] = (u32)pCsr->nDoc;
    }
  }else{
    rc = fts3EvalGatherStats(pCsr, pExpr);
    if( rc==SQLITE_OK ){
      assert( pExpr->aMI );
      for(iCol=0; iCol<pTab->nColumn; iCol++){
        aiOut[iCol*3 + 1] = pExpr->aMI[iCol*3 + 1];
        aiOut[iCol*3 + 2] = pExpr->aMI[iCol*3 + 2];
      }
    }
  }

  return rc;
}

/*
** The expression pExpr passed as the second argument to this function
** must be of type FTSQUERY_PHRASE. 
**
** The returned value is either NULL or a pointer to a buffer containing
** a position-list indicating the occurrences of the phrase in column iCol
** of the current row. 
**
** More specifically, the returned buffer contains 1 varint for each 
** occurrence of the phrase in the column, stored using the normal (delta+2) 
** compression and is terminated by either an 0x01 or 0x00 byte. For example,
** if the requested column contains "a b X c d X X" and the position-list
** for 'X' is requested, the buffer returned may contain:
**
**     0x04 0x05 0x03 0x01   or   0x04 0x05 0x03 0x00
**
** This function works regardless of whether or not the phrase is deferred,
** incremental, or neither.
*/
SQLITE_PRIVATE int sqlite3Fts3EvalPhrasePoslist(
  Fts3Cursor *pCsr,               /* FTS3 cursor object */
  Fts3Expr *pExpr,                /* Phrase to return doclist for */
  int iCol,                       /* Column to return position list for */
  char **ppOut                    /* OUT: Pointer to position list */
){
  Fts3Phrase *pPhrase = pExpr->pPhrase;
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  char *pIter;
  int iThis;
  sqlite3_int64 iDocid;

  /* If this phrase is applies specifically to some column other than 
  ** column iCol, return a NULL pointer.  */
  *ppOut = 0;
  assert( iCol>=0 && iCol<pTab->nColumn );
  if( (pPhrase->iColumn<pTab->nColumn && pPhrase->iColumn!=iCol) ){
    return SQLITE_OK;
  }

  iDocid = pExpr->iDocid;
  pIter = pPhrase->doclist.pList;
  if( iDocid!=pCsr->iPrevId || pExpr->bEof ){
    int bDescDoclist = pTab->bDescIdx;      /* For DOCID_CMP macro */
    int iMul;                     /* +1 if csr dir matches index dir, else -1 */
    int bOr = 0;
    u8 bEof = 0;
    u8 bTreeEof = 0;
    Fts3Expr *p;                  /* Used to iterate from pExpr to root */
    Fts3Expr *pNear;              /* Most senior NEAR ancestor (or pExpr) */

    /* Check if this phrase descends from an OR expression node. If not, 
    ** return NULL. Otherwise, the entry that corresponds to docid 
    ** pCsr->iPrevId may lie earlier in the doclist buffer. Or, if the
    ** tree that the node is part of has been marked as EOF, but the node
    ** itself is not EOF, then it may point to an earlier entry. */
    pNear = pExpr;
    for(p=pExpr->pParent; p; p=p->pParent){
      if( p->eType==FTSQUERY_OR ) bOr = 1;
      if( p->eType==FTSQUERY_NEAR ) pNear = p;
      if( p->bEof ) bTreeEof = 1;
    }
    if( bOr==0 ) return SQLITE_OK;

    /* This is the descendent of an OR node. In this case we cannot use
    ** an incremental phrase. Load the entire doclist for the phrase
    ** into memory in this case.  */
    if( pPhrase->bIncr ){
      int rc = SQLITE_OK;
      int bEofSave = pExpr->bEof;
      fts3EvalRestart(pCsr, pExpr, &rc);
      while( rc==SQLITE_OK && !pExpr->bEof ){
        fts3EvalNextRow(pCsr, pExpr, &rc);
        if( bEofSave==0 && pExpr->iDocid==iDocid ) break;
      }
      pIter = pPhrase->doclist.pList;
      assert( rc!=SQLITE_OK || pPhrase->bIncr==0 );
      if( rc!=SQLITE_OK ) return rc;
    }
    
    iMul = ((pCsr->bDesc==bDescDoclist) ? 1 : -1);
    while( bTreeEof==1 
        && pNear->bEof==0
        && (DOCID_CMP(pNear->iDocid, pCsr->iPrevId) * iMul)<0
    ){
      int rc = SQLITE_OK;
      fts3EvalNextRow(pCsr, pExpr, &rc);
      if( rc!=SQLITE_OK ) return rc;
      iDocid = pExpr->iDocid;
      pIter = pPhrase->doclist.pList;
    }

    bEof = (pPhrase->doclist.nAll==0);
    assert( bDescDoclist==0 || bDescDoclist==1 );
    assert( pCsr->bDesc==0 || pCsr->bDesc==1 );

    if( bEof==0 ){
      if( pCsr->bDesc==bDescDoclist ){
        int dummy;
        if( pNear->bEof ){
          /* This expression is already at EOF. So position it to point to the
          ** last entry in the doclist at pPhrase->doclist.aAll[]. Variable
          ** iDocid is already set for this entry, so all that is required is
          ** to set pIter to point to the first byte of the last position-list
          ** in the doclist. 
          **
          ** It would also be correct to set pIter and iDocid to zero. In
          ** this case, the first call to sqltie3Fts4DoclistPrev() below
          ** would also move the iterator to point to the last entry in the 
          ** doclist. However, this is expensive, as to do so it has to 
          ** iterate through the entire doclist from start to finish (since
          ** it does not know the docid for the last entry).  */
          pIter = &pPhrase->doclist.aAll[pPhrase->doclist.nAll-1];
          fts3ReversePoslist(pPhrase->doclist.aAll, &pIter);
        }
        while( (pIter==0 || DOCID_CMP(iDocid, pCsr->iPrevId)>0 ) && bEof==0 ){
          sqlite3Fts3DoclistPrev(
              bDescDoclist, pPhrase->doclist.aAll, pPhrase->doclist.nAll, 
              &pIter, &iDocid, &dummy, &bEof
          );
        }
      }else{
        if( pNear->bEof ){
          pIter = 0;
          iDocid = 0;
        }
        while( (pIter==0 || DOCID_CMP(iDocid, pCsr->iPrevId)<0 ) && bEof==0 ){
          sqlite3Fts3DoclistNext(
              bDescDoclist, pPhrase->doclist.aAll, pPhrase->doclist.nAll, 
              &pIter, &iDocid, &bEof
          );
        }
      }
    }

    if( bEof || iDocid!=pCsr->iPrevId ) pIter = 0;
  }
  if( pIter==0 ) return SQLITE_OK;

  if( *pIter==0x01 ){
    pIter++;
    pIter += fts3GetVarint32(pIter, &iThis);
  }else{
    iThis = 0;
  }
  while( iThis<iCol ){
    fts3ColumnlistCopy(0, &pIter);
    if( *pIter==0x00 ) return 0;
    pIter++;
    pIter += fts3GetVarint32(pIter, &iThis);
  }

  *ppOut = ((iCol==iThis)?pIter:0);
  return SQLITE_OK;
}

/*
** Free all components of the Fts3Phrase structure that were allocated by
** the eval module. Specifically, this means to free:
**
**   * the contents of pPhrase->doclist, and
**   * any Fts3MultiSegReader objects held by phrase tokens.
*/
SQLITE_PRIVATE void sqlite3Fts3EvalPhraseCleanup(Fts3Phrase *pPhrase){
  if( pPhrase ){
    int i;
    sqlite3_free(pPhrase->doclist.aAll);
    fts3EvalInvalidatePoslist(pPhrase);
    memset(&pPhrase->doclist, 0, sizeof(Fts3Doclist));
    for(i=0; i<pPhrase->nToken; i++){
      fts3SegReaderCursorFree(pPhrase->aToken[i].pSegcsr);
      pPhrase->aToken[i].pSegcsr = 0;
    }
  }
}


/*
** Return SQLITE_CORRUPT_VTAB.
*/
#ifdef SQLITE_DEBUG
SQLITE_PRIVATE int sqlite3Fts3Corrupt(){
  return SQLITE_CORRUPT_VTAB;
}
#endif

#if !SQLITE_CORE
/*
** Initialize API pointer table, if required.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
SQLITE_API int sqlite3_fts3_init(
  sqlite3 *db, 
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  SQLITE_EXTENSION_INIT2(pApi)
  return sqlite3Fts3Init(db);
}
#endif

#endif

/************** End of fts3.c ************************************************/
/************** Begin file fts3_aux.c ****************************************/
/*
** 2011 Jan 27
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <string.h> */
/* #include <assert.h> */

typedef struct Fts3auxTable Fts3auxTable;
typedef struct Fts3auxCursor Fts3auxCursor;

struct Fts3auxTable {
  sqlite3_vtab base;              /* Base class used by SQLite core */
  Fts3Table *pFts3Tab;
};

struct Fts3auxCursor {
  sqlite3_vtab_cursor base;       /* Base class used by SQLite core */
  Fts3MultiSegReader csr;        /* Must be right after "base" */
  Fts3SegFilter filter;
  char *zStop;
  int nStop;                      /* Byte-length of string zStop */
  int iLangid;                    /* Language id to query */
  int isEof;                      /* True if cursor is at EOF */
  sqlite3_int64 iRowid;           /* Current rowid */

  int iCol;                       /* Current value of 'col' column */
  int nStat;                      /* Size of aStat[] array */
  struct Fts3auxColstats {
    sqlite3_int64 nDoc;           /* 'documents' values for current csr row */
    sqlite3_int64 nOcc;           /* 'occurrences' values for current csr row */
  } *aStat;
};

/*
** Schema of the terms table.
*/
#define FTS3_AUX_SCHEMA \
  "CREATE TABLE x(term, col, documents, occurrences, languageid HIDDEN)"

/*
** This function does all the work for both the xConnect and xCreate methods.
** These tables have no persistent representation of their own, so xConnect
** and xCreate are identical operations.
*/
static int fts3auxConnectMethod(
  sqlite3 *db,                    /* Database connection */
  void *pUnused,                  /* Unused */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVtab,          /* OUT: New sqlite3_vtab object */
  char **pzErr                    /* OUT: sqlite3_malloc'd error message */
){
  char const *zDb;                /* Name of database (e.g. "main") */
  char const *zFts3;              /* Name of fts3 table */
  int nDb;                        /* Result of strlen(zDb) */
  int nFts3;                      /* Result of strlen(zFts3) */
  int nByte;                      /* Bytes of space to allocate here */
  int rc;                         /* value returned by declare_vtab() */
  Fts3auxTable *p;                /* Virtual table object to return */

  UNUSED_PARAMETER(pUnused);

  /* The user should invoke this in one of two forms:
  **
  **     CREATE VIRTUAL TABLE xxx USING fts4aux(fts4-table);
  **     CREATE VIRTUAL TABLE xxx USING fts4aux(fts4-table-db, fts4-table);
  */
  if( argc!=4 && argc!=5 ) goto bad_args;

  zDb = argv[1]; 
  nDb = (int)strlen(zDb);
  if( argc==5 ){
    if( nDb==4 && 0==sqlite3_strnicmp("temp", zDb, 4) ){
      zDb = argv[3]; 
      nDb = (int)strlen(zDb);
      zFts3 = argv[4];
    }else{
      goto bad_args;
    }
  }else{
    zFts3 = argv[3];
  }
  nFts3 = (int)strlen(zFts3);

  rc = sqlite3_declare_vtab(db, FTS3_AUX_SCHEMA);
  if( rc!=SQLITE_OK ) return rc;

  nByte = sizeof(Fts3auxTable) + sizeof(Fts3Table) + nDb + nFts3 + 2;
  p = (Fts3auxTable *)sqlite3_malloc(nByte);
  if( !p ) return SQLITE_NOMEM;
  memset(p, 0, nByte);

  p->pFts3Tab = (Fts3Table *)&p[1];
  p->pFts3Tab->zDb = (char *)&p->pFts3Tab[1];
  p->pFts3Tab->zName = &p->pFts3Tab->zDb[nDb+1];
  p->pFts3Tab->db = db;
  p->pFts3Tab->nIndex = 1;

  memcpy((char *)p->pFts3Tab->zDb, zDb, nDb);
  memcpy((char *)p->pFts3Tab->zName, zFts3, nFts3);
  sqlite3Fts3Dequote((char *)p->pFts3Tab->zName);

  *ppVtab = (sqlite3_vtab *)p;
  return SQLITE_OK;

 bad_args:
  *pzErr = sqlite3_mprintf("invalid arguments to fts4aux constructor");
  return SQLITE_ERROR;
}

/*
** This function does the work for both the xDisconnect and xDestroy methods.
** These tables have no persistent representation of their own, so xDisconnect
** and xDestroy are identical operations.
*/
static int fts3auxDisconnectMethod(sqlite3_vtab *pVtab){
  Fts3auxTable *p = (Fts3auxTable *)pVtab;
  Fts3Table *pFts3 = p->pFts3Tab;
  int i;

  /* Free any prepared statements held */
  for(i=0; i<SizeofArray(pFts3->aStmt); i++){
    sqlite3_finalize(pFts3->aStmt[i]);
  }
  sqlite3_free(pFts3->zSegmentsTbl);
  sqlite3_free(p);
  return SQLITE_OK;
}

#define FTS4AUX_EQ_CONSTRAINT 1
#define FTS4AUX_GE_CONSTRAINT 2
#define FTS4AUX_LE_CONSTRAINT 4

/*
** xBestIndex - Analyze a WHERE and ORDER BY clause.
*/
static int fts3auxBestIndexMethod(
  sqlite3_vtab *pVTab, 
  sqlite3_index_info *pInfo
){
  int i;
  int iEq = -1;
  int iGe = -1;
  int iLe = -1;
  int iLangid = -1;
  int iNext = 1;                  /* Next free argvIndex value */

  UNUSED_PARAMETER(pVTab);

  /* This vtab delivers always results in "ORDER BY term ASC" order. */
  if( pInfo->nOrderBy==1 
   && pInfo->aOrderBy[0].iColumn==0 
   && pInfo->aOrderBy[0].desc==0
  ){
    pInfo->orderByConsumed = 1;
  }

  /* Search for equality and range constraints on the "term" column. 
  ** And equality constraints on the hidden "languageid" column. */
  for(i=0; i<pInfo->nConstraint; i++){
    if( pInfo->aConstraint[i].usable ){
      int op = pInfo->aConstraint[i].op;
      int iCol = pInfo->aConstraint[i].iColumn;

      if( iCol==0 ){
        if( op==SQLITE_INDEX_CONSTRAINT_EQ ) iEq = i;
        if( op==SQLITE_INDEX_CONSTRAINT_LT ) iLe = i;
        if( op==SQLITE_INDEX_CONSTRAINT_LE ) iLe = i;
        if( op==SQLITE_INDEX_CONSTRAINT_GT ) iGe = i;
        if( op==SQLITE_INDEX_CONSTRAINT_GE ) iGe = i;
      }
      if( iCol==4 ){
        if( op==SQLITE_INDEX_CONSTRAINT_EQ ) iLangid = i;
      }
    }
  }

  if( iEq>=0 ){
    pInfo->idxNum = FTS4AUX_EQ_CONSTRAINT;
    pInfo->aConstraintUsage[iEq].argvIndex = iNext++;
    pInfo->estimatedCost = 5;
  }else{
    pInfo->idxNum = 0;
    pInfo->estimatedCost = 20000;
    if( iGe>=0 ){
      pInfo->idxNum += FTS4AUX_GE_CONSTRAINT;
      pInfo->aConstraintUsage[iGe].argvIndex = iNext++;
      pInfo->estimatedCost /= 2;
    }
    if( iLe>=0 ){
      pInfo->idxNum += FTS4AUX_LE_CONSTRAINT;
      pInfo->aConstraintUsage[iLe].argvIndex = iNext++;
      pInfo->estimatedCost /= 2;
    }
  }
  if( iLangid>=0 ){
    pInfo->aConstraintUsage[iLangid].argvIndex = iNext++;
    pInfo->estimatedCost--;
  }

  return SQLITE_OK;
}

/*
** xOpen - Open a cursor.
*/
static int fts3auxOpenMethod(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCsr){
  Fts3auxCursor *pCsr;            /* Pointer to cursor object to return */

  UNUSED_PARAMETER(pVTab);

  pCsr = (Fts3auxCursor *)sqlite3_malloc(sizeof(Fts3auxCursor));
  if( !pCsr ) return SQLITE_NOMEM;
  memset(pCsr, 0, sizeof(Fts3auxCursor));

  *ppCsr = (sqlite3_vtab_cursor *)pCsr;
  return SQLITE_OK;
}

/*
** xClose - Close a cursor.
*/
static int fts3auxCloseMethod(sqlite3_vtab_cursor *pCursor){
  Fts3Table *pFts3 = ((Fts3auxTable *)pCursor->pVtab)->pFts3Tab;
  Fts3auxCursor *pCsr = (Fts3auxCursor *)pCursor;

  sqlite3Fts3SegmentsClose(pFts3);
  sqlite3Fts3SegReaderFinish(&pCsr->csr);
  sqlite3_free((void *)pCsr->filter.zTerm);
  sqlite3_free(pCsr->zStop);
  sqlite3_free(pCsr->aStat);
  sqlite3_free(pCsr);
  return SQLITE_OK;
}

static int fts3auxGrowStatArray(Fts3auxCursor *pCsr, int nSize){
  if( nSize>pCsr->nStat ){
    struct Fts3auxColstats *aNew;
    aNew = (struct Fts3auxColstats *)sqlite3_realloc(pCsr->aStat, 
        sizeof(struct Fts3auxColstats) * nSize
    );
    if( aNew==0 ) return SQLITE_NOMEM;
    memset(&aNew[pCsr->nStat], 0, 
        sizeof(struct Fts3auxColstats) * (nSize - pCsr->nStat)
    );
    pCsr->aStat = aNew;
    pCsr->nStat = nSize;
  }
  return SQLITE_OK;
}

/*
** xNext - Advance the cursor to the next row, if any.
*/
static int fts3auxNextMethod(sqlite3_vtab_cursor *pCursor){
  Fts3auxCursor *pCsr = (Fts3auxCursor *)pCursor;
  Fts3Table *pFts3 = ((Fts3auxTable *)pCursor->pVtab)->pFts3Tab;
  int rc;

  /* Increment our pretend rowid value. */
  pCsr->iRowid++;

  for(pCsr->iCol++; pCsr->iCol<pCsr->nStat; pCsr->iCol++){
    if( pCsr->aStat[pCsr->iCol].nDoc>0 ) return SQLITE_OK;
  }

  rc = sqlite3Fts3SegReaderStep(pFts3, &pCsr->csr);
  if( rc==SQLITE_ROW ){
    int i = 0;
    int nDoclist = pCsr->csr.nDoclist;
    char *aDoclist = pCsr->csr.aDoclist;
    int iCol;

    int eState = 0;

    if( pCsr->zStop ){
      int n = (pCsr->nStop<pCsr->csr.nTerm) ? pCsr->nStop : pCsr->csr.nTerm;
      int mc = memcmp(pCsr->zStop, pCsr->csr.zTerm, n);
      if( mc<0 || (mc==0 && pCsr->csr.nTerm>pCsr->nStop) ){
        pCsr->isEof = 1;
        return SQLITE_OK;
      }
    }

    if( fts3auxGrowStatArray(pCsr, 2) ) return SQLITE_NOMEM;
    memset(pCsr->aStat, 0, sizeof(struct Fts3auxColstats) * pCsr->nStat);
    iCol = 0;

    while( i<nDoclist ){
      sqlite3_int64 v = 0;

      i += sqlite3Fts3GetVarint(&aDoclist[i], &v);
      switch( eState ){
        /* State 0. In this state the integer just read was a docid. */
        case 0:
          pCsr->aStat[0].nDoc++;
          eState = 1;
          iCol = 0;
          break;

        /* State 1. In this state we are expecting either a 1, indicating
        ** that the following integer will be a column number, or the
        ** start of a position list for column 0.  
        ** 
        ** The only difference between state 1 and state 2 is that if the
        ** integer encountered in state 1 is not 0 or 1, then we need to
        ** increment the column 0 "nDoc" count for this term.
        */
        case 1:
          assert( iCol==0 );
          if( v>1 ){
            pCsr->aStat[1].nDoc++;
          }
          eState = 2;
          /* fall through */

        case 2:
          if( v==0 ){       /* 0x00. Next integer will be a docid. */
            eState = 0;
          }else if( v==1 ){ /* 0x01. Next integer will be a column number. */
            eState = 3;
          }else{            /* 2 or greater. A position. */
            pCsr->aStat[iCol+1].nOcc++;
            pCsr->aStat[0].nOcc++;
          }
          break;

        /* State 3. The integer just read is a column number. */
        default: assert( eState==3 );
          iCol = (int)v;
          if( fts3auxGrowStatArray(pCsr, iCol+2) ) return SQLITE_NOMEM;
          pCsr->aStat[iCol+1].nDoc++;
          eState = 2;
          break;
      }
    }

    pCsr->iCol = 0;
    rc = SQLITE_OK;
  }else{
    pCsr->isEof = 1;
  }
  return rc;
}

/*
** xFilter - Initialize a cursor to point at the start of its data.
*/
static int fts3auxFilterMethod(
  sqlite3_vtab_cursor *pCursor,   /* The cursor used for this query */
  int idxNum,                     /* Strategy index */
  const char *idxStr,             /* Unused */
  int nVal,                       /* Number of elements in apVal */
  sqlite3_value **apVal           /* Arguments for the indexing scheme */
){
  Fts3auxCursor *pCsr = (Fts3auxCursor *)pCursor;
  Fts3Table *pFts3 = ((Fts3auxTable *)pCursor->pVtab)->pFts3Tab;
  int rc;
  int isScan = 0;
  int iLangVal = 0;               /* Language id to query */

  int iEq = -1;                   /* Index of term=? value in apVal */
  int iGe = -1;                   /* Index of term>=? value in apVal */
  int iLe = -1;                   /* Index of term<=? value in apVal */
  int iLangid = -1;               /* Index of languageid=? value in apVal */
  int iNext = 0;

  UNUSED_PARAMETER(nVal);
  UNUSED_PARAMETER(idxStr);

  assert( idxStr==0 );
  assert( idxNum==FTS4AUX_EQ_CONSTRAINT || idxNum==0
       || idxNum==FTS4AUX_LE_CONSTRAINT || idxNum==FTS4AUX_GE_CONSTRAINT
       || idxNum==(FTS4AUX_LE_CONSTRAINT|FTS4AUX_GE_CONSTRAINT)
  );

  if( idxNum==FTS4AUX_EQ_CONSTRAINT ){
    iEq = iNext++;
  }else{
    isScan = 1;
    if( idxNum & FTS4AUX_GE_CONSTRAINT ){
      iGe = iNext++;
    }
    if( idxNum & FTS4AUX_LE_CONSTRAINT ){
      iLe = iNext++;
    }
  }
  if( iNext<nVal ){
    iLangid = iNext++;
  }

  /* In case this cursor is being reused, close and zero it. */
  testcase(pCsr->filter.zTerm);
  sqlite3Fts3SegReaderFinish(&pCsr->csr);
  sqlite3_free((void *)pCsr->filter.zTerm);
  sqlite3_free(pCsr->aStat);
  memset(&pCsr->csr, 0, ((u8*)&pCsr[1]) - (u8*)&pCsr->csr);

  pCsr->filter.flags = FTS3_SEGMENT_REQUIRE_POS|FTS3_SEGMENT_IGNORE_EMPTY;
  if( isScan ) pCsr->filter.flags |= FTS3_SEGMENT_SCAN;

  if( iEq>=0 || iGe>=0 ){
    const unsigned char *zStr = sqlite3_value_text(apVal[0]);
    assert( (iEq==0 && iGe==-1) || (iEq==-1 && iGe==0) );
    if( zStr ){
      pCsr->filter.zTerm = sqlite3_mprintf("%s", zStr);
      pCsr->filter.nTerm = sqlite3_value_bytes(apVal[0]);
      if( pCsr->filter.zTerm==0 ) return SQLITE_NOMEM;
    }
  }

  if( iLe>=0 ){
    pCsr->zStop = sqlite3_mprintf("%s", sqlite3_value_text(apVal[iLe]));
    pCsr->nStop = sqlite3_value_bytes(apVal[iLe]);
    if( pCsr->zStop==0 ) return SQLITE_NOMEM;
  }
  
  if( iLangid>=0 ){
    iLangVal = sqlite3_value_int(apVal[iLangid]);

    /* If the user specified a negative value for the languageid, use zero
    ** instead. This works, as the "languageid=?" constraint will also
    ** be tested by the VDBE layer. The test will always be false (since
    ** this module will not return a row with a negative languageid), and
    ** so the overall query will return zero rows.  */
    if( iLangVal<0 ) iLangVal = 0;
  }
  pCsr->iLangid = iLangVal;

  rc = sqlite3Fts3SegReaderCursor(pFts3, iLangVal, 0, FTS3_SEGCURSOR_ALL,
      pCsr->filter.zTerm, pCsr->filter.nTerm, 0, isScan, &pCsr->csr
  );
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts3SegReaderStart(pFts3, &pCsr->csr, &pCsr->filter);
  }

  if( rc==SQLITE_OK ) rc = fts3auxNextMethod(pCursor);
  return rc;
}

/*
** xEof - Return true if the cursor is at EOF, or false otherwise.
*/
static int fts3auxEofMethod(sqlite3_vtab_cursor *pCursor){
  Fts3auxCursor *pCsr = (Fts3auxCursor *)pCursor;
  return pCsr->isEof;
}

/*
** xColumn - Return a column value.
*/
static int fts3auxColumnMethod(
  sqlite3_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite3_context *pCtx,          /* Context for sqlite3_result_xxx() calls */
  int iCol                        /* Index of column to read value from */
){
  Fts3auxCursor *p = (Fts3auxCursor *)pCursor;

  assert( p->isEof==0 );
  switch( iCol ){
    case 0: /* term */
      sqlite3_result_text(pCtx, p->csr.zTerm, p->csr.nTerm, SQLITE_TRANSIENT);
      break;

    case 1: /* col */
      if( p->iCol ){
        sqlite3_result_int(pCtx, p->iCol-1);
      }else{
        sqlite3_result_text(pCtx, "*", -1, SQLITE_STATIC);
      }
      break;

    case 2: /* documents */
      sqlite3_result_int64(pCtx, p->aStat[p->iCol].nDoc);
      break;

    case 3: /* occurrences */
      sqlite3_result_int64(pCtx, p->aStat[p->iCol].nOcc);
      break;

    default: /* languageid */
      assert( iCol==4 );
      sqlite3_result_int(pCtx, p->iLangid);
      break;
  }

  return SQLITE_OK;
}

/*
** xRowid - Return the current rowid for the cursor.
*/
static int fts3auxRowidMethod(
  sqlite3_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite_int64 *pRowid            /* OUT: Rowid value */
){
  Fts3auxCursor *pCsr = (Fts3auxCursor *)pCursor;
  *pRowid = pCsr->iRowid;
  return SQLITE_OK;
}

/*
** Register the fts3aux module with database connection db. Return SQLITE_OK
** if successful or an error code if sqlite3_create_module() fails.
*/
SQLITE_PRIVATE int sqlite3Fts3InitAux(sqlite3 *db){
  static const sqlite3_module fts3aux_module = {
     0,                           /* iVersion      */
     fts3auxConnectMethod,        /* xCreate       */
     fts3auxConnectMethod,        /* xConnect      */
     fts3auxBestIndexMethod,      /* xBestIndex    */
     fts3auxDisconnectMethod,     /* xDisconnect   */
     fts3auxDisconnectMethod,     /* xDestroy      */
     fts3auxOpenMethod,           /* xOpen         */
     fts3auxCloseMethod,          /* xClose        */
     fts3auxFilterMethod,         /* xFilter       */
     fts3auxNextMethod,           /* xNext         */
     fts3auxEofMethod,            /* xEof          */
     fts3auxColumnMethod,         /* xColumn       */
     fts3auxRowidMethod,          /* xRowid        */
     0,                           /* xUpdate       */
     0,                           /* xBegin        */
     0,                           /* xSync         */
     0,                           /* xCommit       */
     0,                           /* xRollback     */
     0,                           /* xFindFunction */
     0,                           /* xRename       */
     0,                           /* xSavepoint    */
     0,                           /* xRelease      */
     0                            /* xRollbackTo   */
  };
  int rc;                         /* Return code */

  rc = sqlite3_create_module(db, "fts4aux", &fts3aux_module, 0);
  return rc;
}

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */

/************** End of fts3_aux.c ********************************************/
/************** Begin file fts3_expr.c ***************************************/
/*
** 2008 Nov 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This module contains code that implements a parser for fts3 query strings
** (the right-hand argument to the MATCH operator). Because the supported 
** syntax is relatively simple, the whole tokenizer/parser system is
** hand-coded. 
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/*
** By default, this module parses the legacy syntax that has been 
** traditionally used by fts3. Or, if SQLITE_ENABLE_FTS3_PARENTHESIS
** is defined, then it uses the new syntax. The differences between
** the new and the old syntaxes are:
**
**  a) The new syntax supports parenthesis. The old does not.
**
**  b) The new syntax supports the AND and NOT operators. The old does not.
**
**  c) The old syntax supports the "-" token qualifier. This is not 
**     supported by the new syntax (it is replaced by the NOT operator).
**
**  d) When using the old syntax, the OR operator has a greater precedence
**     than an implicit AND. When using the new, both implicity and explicit
**     AND operators have a higher precedence than OR.
**
** If compiled with SQLITE_TEST defined, then this module exports the
** symbol "int sqlite3_fts3_enable_parentheses". Setting this variable
** to zero causes the module to use the old syntax. If it is set to 
** non-zero the new syntax is activated. This is so both syntaxes can
** be tested using a single build of testfixture.
**
** The following describes the syntax supported by the fts3 MATCH
** operator in a similar format to that used by the lemon parser
** generator. This module does not use actually lemon, it uses a
** custom parser.
**
**   query ::= andexpr (OR andexpr)*.
**
**   andexpr ::= notexpr (AND? notexpr)*.
**
**   notexpr ::= nearexpr (NOT nearexpr|-TOKEN)*.
**   notexpr ::= LP query RP.
**
**   nearexpr ::= phrase (NEAR distance_opt nearexpr)*.
**
**   distance_opt ::= .
**   distance_opt ::= / INTEGER.
**
**   phrase ::= TOKEN.
**   phrase ::= COLUMN:TOKEN.
**   phrase ::= "TOKEN TOKEN TOKEN...".
*/

#ifdef SQLITE_TEST
SQLITE_API int sqlite3_fts3_enable_parentheses = 0;
#else
# ifdef SQLITE_ENABLE_FTS3_PARENTHESIS 
#  define sqlite3_fts3_enable_parentheses 1
# else
#  define sqlite3_fts3_enable_parentheses 0
# endif
#endif

/*
** Default span for NEAR operators.
*/
#define SQLITE_FTS3_DEFAULT_NEAR_PARAM 10

/* #include <string.h> */
/* #include <assert.h> */

/*
** isNot:
**   This variable is used by function getNextNode(). When getNextNode() is
**   called, it sets ParseContext.isNot to true if the 'next node' is a 
**   FTSQUERY_PHRASE with a unary "-" attached to it. i.e. "mysql" in the
**   FTS3 query "sqlite -mysql". Otherwise, ParseContext.isNot is set to
**   zero.
*/
typedef struct ParseContext ParseContext;
struct ParseContext {
  sqlite3_tokenizer *pTokenizer;      /* Tokenizer module */
  int iLangid;                        /* Language id used with tokenizer */
  const char **azCol;                 /* Array of column names for fts3 table */
  int bFts4;                          /* True to allow FTS4-only syntax */
  int nCol;                           /* Number of entries in azCol[] */
  int iDefaultCol;                    /* Default column to query */
  int isNot;                          /* True if getNextNode() sees a unary - */
  sqlite3_context *pCtx;              /* Write error message here */
  int nNest;                          /* Number of nested brackets */
};

/*
** This function is equivalent to the standard isspace() function. 
**
** The standard isspace() can be awkward to use safely, because although it
** is defined to accept an argument of type int, its behavior when passed
** an integer that falls outside of the range of the unsigned char type
** is undefined (and sometimes, "undefined" means segfault). This wrapper
** is defined to accept an argument of type char, and always returns 0 for
** any values that fall outside of the range of the unsigned char type (i.e.
** negative values).
*/
static int fts3isspace(char c){
  return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\v' || c=='\f';
}

/*
** Allocate nByte bytes of memory using sqlite3_malloc(). If successful,
** zero the memory before returning a pointer to it. If unsuccessful, 
** return NULL.
*/
static void *fts3MallocZero(int nByte){
  void *pRet = sqlite3_malloc(nByte);
  if( pRet ) memset(pRet, 0, nByte);
  return pRet;
}

SQLITE_PRIVATE int sqlite3Fts3OpenTokenizer(
  sqlite3_tokenizer *pTokenizer,
  int iLangid,
  const char *z,
  int n,
  sqlite3_tokenizer_cursor **ppCsr
){
  sqlite3_tokenizer_module const *pModule = pTokenizer->pModule;
  sqlite3_tokenizer_cursor *pCsr = 0;
  int rc;

  rc = pModule->xOpen(pTokenizer, z, n, &pCsr);
  assert( rc==SQLITE_OK || pCsr==0 );
  if( rc==SQLITE_OK ){
    pCsr->pTokenizer = pTokenizer;
    if( pModule->iVersion>=1 ){
      rc = pModule->xLanguageid(pCsr, iLangid);
      if( rc!=SQLITE_OK ){
        pModule->xClose(pCsr);
        pCsr = 0;
      }
    }
  }
  *ppCsr = pCsr;
  return rc;
}

/*
** Function getNextNode(), which is called by fts3ExprParse(), may itself
** call fts3ExprParse(). So this forward declaration is required.
*/
static int fts3ExprParse(ParseContext *, const char *, int, Fts3Expr **, int *);

/*
** Extract the next token from buffer z (length n) using the tokenizer
** and other information (column names etc.) in pParse. Create an Fts3Expr
** structure of type FTSQUERY_PHRASE containing a phrase consisting of this
** single token and set *ppExpr to point to it. If the end of the buffer is
** reached before a token is found, set *ppExpr to zero. It is the
** responsibility of the caller to eventually deallocate the allocated 
** Fts3Expr structure (if any) by passing it to sqlite3_free().
**
** Return SQLITE_OK if successful, or SQLITE_NOMEM if a memory allocation
** fails.
*/
static int getNextToken(
  ParseContext *pParse,                   /* fts3 query parse context */
  int iCol,                               /* Value for Fts3Phrase.iColumn */
  const char *z, int n,                   /* Input string */
  Fts3Expr **ppExpr,                      /* OUT: expression */
  int *pnConsumed                         /* OUT: Number of bytes consumed */
){
  sqlite3_tokenizer *pTokenizer = pParse->pTokenizer;
  sqlite3_tokenizer_module const *pModule = pTokenizer->pModule;
  int rc;
  sqlite3_tokenizer_cursor *pCursor;
  Fts3Expr *pRet = 0;
  int i = 0;

  /* Set variable i to the maximum number of bytes of input to tokenize. */
  for(i=0; i<n; i++){
    if( sqlite3_fts3_enable_parentheses && (z[i]=='(' || z[i]==')') ) break;
    if( z[i]=='*' || z[i]=='"' ) break;
  }

  *pnConsumed = i;
  rc = sqlite3Fts3OpenTokenizer(pTokenizer, pParse->iLangid, z, i, &pCursor);
  if( rc==SQLITE_OK ){
    const char *zToken;
    int nToken = 0, iStart = 0, iEnd = 0, iPosition = 0;
    int nByte;                               /* total space to allocate */

    rc = pModule->xNext(pCursor, &zToken, &nToken, &iStart, &iEnd, &iPosition);
    if( rc==SQLITE_OK ){
      nByte = sizeof(Fts3Expr) + sizeof(Fts3Phrase) + nToken;
      pRet = (Fts3Expr *)fts3MallocZero(nByte);
      if( !pRet ){
        rc = SQLITE_NOMEM;
      }else{
        pRet->eType = FTSQUERY_PHRASE;
        pRet->pPhrase = (Fts3Phrase *)&pRet[1];
        pRet->pPhrase->nToken = 1;
        pRet->pPhrase->iColumn = iCol;
        pRet->pPhrase->aToken[0].n = nToken;
        pRet->pPhrase->aToken[0].z = (char *)&pRet->pPhrase[1];
        memcpy(pRet->pPhrase->aToken[0].z, zToken, nToken);

        if( iEnd<n && z[iEnd]=='*' ){
          pRet->pPhrase->aToken[0].isPrefix = 1;
          iEnd++;
        }

        while( 1 ){
          if( !sqlite3_fts3_enable_parentheses 
           && iStart>0 && z[iStart-1]=='-' 
          ){
            pParse->isNot = 1;
            iStart--;
          }else if( pParse->bFts4 && iStart>0 && z[iStart-1]=='^' ){
            pRet->pPhrase->aToken[0].bFirst = 1;
            iStart--;
          }else{
            break;
          }
        }

      }
      *pnConsumed = iEnd;
    }else if( i && rc==SQLITE_DONE ){
      rc = SQLITE_OK;
    }

    pModule->xClose(pCursor);
  }
  
  *ppExpr = pRet;
  return rc;
}


/*
** Enlarge a memory allocation.  If an out-of-memory allocation occurs,
** then free the old allocation.
*/
static void *fts3ReallocOrFree(void *pOrig, int nNew){
  void *pRet = sqlite3_realloc(pOrig, nNew);
  if( !pRet ){
    sqlite3_free(pOrig);
  }
  return pRet;
}

/*
** Buffer zInput, length nInput, contains the contents of a quoted string
** that appeared as part of an fts3 query expression. Neither quote character
** is included in the buffer. This function attempts to tokenize the entire
** input buffer and create an Fts3Expr structure of type FTSQUERY_PHRASE 
** containing the results.
**
** If successful, SQLITE_OK is returned and *ppExpr set to point at the
** allocated Fts3Expr structure. Otherwise, either SQLITE_NOMEM (out of memory
** error) or SQLITE_ERROR (tokenization error) is returned and *ppExpr set
** to 0.
*/
static int getNextString(
  ParseContext *pParse,                   /* fts3 query parse context */
  const char *zInput, int nInput,         /* Input string */
  Fts3Expr **ppExpr                       /* OUT: expression */
){
  sqlite3_tokenizer *pTokenizer = pParse->pTokenizer;
  sqlite3_tokenizer_module const *pModule = pTokenizer->pModule;
  int rc;
  Fts3Expr *p = 0;
  sqlite3_tokenizer_cursor *pCursor = 0;
  char *zTemp = 0;
  int nTemp = 0;

  const int nSpace = sizeof(Fts3Expr) + sizeof(Fts3Phrase);
  int nToken = 0;

  /* The final Fts3Expr data structure, including the Fts3Phrase,
  ** Fts3PhraseToken structures token buffers are all stored as a single 
  ** allocation so that the expression can be freed with a single call to
  ** sqlite3_free(). Setting this up requires a two pass approach.
  **
  ** The first pass, in the block below, uses a tokenizer cursor to iterate
  ** through the tokens in the expression. This pass uses fts3ReallocOrFree()
  ** to assemble data in two dynamic buffers:
  **
  **   Buffer p: Points to the Fts3Expr structure, followed by the Fts3Phrase
  **             structure, followed by the array of Fts3PhraseToken 
  **             structures. This pass only populates the Fts3PhraseToken array.
  **
  **   Buffer zTemp: Contains copies of all tokens.
  **
  ** The second pass, in the block that begins "if( rc==SQLITE_DONE )" below,
  ** appends buffer zTemp to buffer p, and fills in the Fts3Expr and Fts3Phrase
  ** structures.
  */
  rc = sqlite3Fts3OpenTokenizer(
      pTokenizer, pParse->iLangid, zInput, nInput, &pCursor);
  if( rc==SQLITE_OK ){
    int ii;
    for(ii=0; rc==SQLITE_OK; ii++){
      const char *zByte;
      int nByte = 0, iBegin = 0, iEnd = 0, iPos = 0;
      rc = pModule->xNext(pCursor, &zByte, &nByte, &iBegin, &iEnd, &iPos);
      if( rc==SQLITE_OK ){
        Fts3PhraseToken *pToken;

        p = fts3ReallocOrFree(p, nSpace + ii*sizeof(Fts3PhraseToken));
        if( !p ) goto no_mem;

        zTemp = fts3ReallocOrFree(zTemp, nTemp + nByte);
        if( !zTemp ) goto no_mem;

        assert( nToken==ii );
        pToken = &((Fts3Phrase *)(&p[1]))->aToken[ii];
        memset(pToken, 0, sizeof(Fts3PhraseToken));

        memcpy(&zTemp[nTemp], zByte, nByte);
        nTemp += nByte;

        pToken->n = nByte;
        pToken->isPrefix = (iEnd<nInput && zInput[iEnd]=='*');
        pToken->bFirst = (iBegin>0 && zInput[iBegin-1]=='^');
        nToken = ii+1;
      }
    }

    pModule->xClose(pCursor);
    pCursor = 0;
  }

  if( rc==SQLITE_DONE ){
    int jj;
    char *zBuf = 0;

    p = fts3ReallocOrFree(p, nSpace + nToken*sizeof(Fts3PhraseToken) + nTemp);
    if( !p ) goto no_mem;
    memset(p, 0, (char *)&(((Fts3Phrase *)&p[1])->aToken[0])-(char *)p);
    p->eType = FTSQUERY_PHRASE;
    p->pPhrase = (Fts3Phrase *)&p[1];
    p->pPhrase->iColumn = pParse->iDefaultCol;
    p->pPhrase->nToken = nToken;

    zBuf = (char *)&p->pPhrase->aToken[nToken];
    if( zTemp ){
      memcpy(zBuf, zTemp, nTemp);
      sqlite3_free(zTemp);
    }else{
      assert( nTemp==0 );
    }

    for(jj=0; jj<p->pPhrase->nToken; jj++){
      p->pPhrase->aToken[jj].z = zBuf;
      zBuf += p->pPhrase->aToken[jj].n;
    }
    rc = SQLITE_OK;
  }

  *ppExpr = p;
  return rc;
no_mem:

  if( pCursor ){
    pModule->xClose(pCursor);
  }
  sqlite3_free(zTemp);
  sqlite3_free(p);
  *ppExpr = 0;
  return SQLITE_NOMEM;
}

/*
** The output variable *ppExpr is populated with an allocated Fts3Expr 
** structure, or set to 0 if the end of the input buffer is reached.
**
** Returns an SQLite error code. SQLITE_OK if everything works, SQLITE_NOMEM
** if a malloc failure occurs, or SQLITE_ERROR if a parse error is encountered.
** If SQLITE_ERROR is returned, pContext is populated with an error message.
*/
static int getNextNode(
  ParseContext *pParse,                   /* fts3 query parse context */
  const char *z, int n,                   /* Input string */
  Fts3Expr **ppExpr,                      /* OUT: expression */
  int *pnConsumed                         /* OUT: Number of bytes consumed */
){
  static const struct Fts3Keyword {
    char *z;                              /* Keyword text */
    unsigned char n;                      /* Length of the keyword */
    unsigned char parenOnly;              /* Only valid in paren mode */
    unsigned char eType;                  /* Keyword code */
  } aKeyword[] = {
    { "OR" ,  2, 0, FTSQUERY_OR   },
    { "AND",  3, 1, FTSQUERY_AND  },
    { "NOT",  3, 1, FTSQUERY_NOT  },
    { "NEAR", 4, 0, FTSQUERY_NEAR }
  };
  int ii;
  int iCol;
  int iColLen;
  int rc;
  Fts3Expr *pRet = 0;

  const char *zInput = z;
  int nInput = n;

  pParse->isNot = 0;

  /* Skip over any whitespace before checking for a keyword, an open or
  ** close bracket, or a quoted string. 
  */
  while( nInput>0 && fts3isspace(*zInput) ){
    nInput--;
    zInput++;
  }
  if( nInput==0 ){
    return SQLITE_DONE;
  }

  /* See if we are dealing with a keyword. */
  for(ii=0; ii<(int)(sizeof(aKeyword)/sizeof(struct Fts3Keyword)); ii++){
    const struct Fts3Keyword *pKey = &aKeyword[ii];

    if( (pKey->parenOnly & ~sqlite3_fts3_enable_parentheses)!=0 ){
      continue;
    }

    if( nInput>=pKey->n && 0==memcmp(zInput, pKey->z, pKey->n) ){
      int nNear = SQLITE_FTS3_DEFAULT_NEAR_PARAM;
      int nKey = pKey->n;
      char cNext;

      /* If this is a "NEAR" keyword, check for an explicit nearness. */
      if( pKey->eType==FTSQUERY_NEAR ){
        assert( nKey==4 );
        if( zInput[4]=='/' && zInput[5]>='0' && zInput[5]<='9' ){
          nNear = 0;
          for(nKey=5; zInput[nKey]>='0' && zInput[nKey]<='9'; nKey++){
            nNear = nNear * 10 + (zInput[nKey] - '0');
          }
        }
      }

      /* At this point this is probably a keyword. But for that to be true,
      ** the next byte must contain either whitespace, an open or close
      ** parenthesis, a quote character, or EOF. 
      */
      cNext = zInput[nKey];
      if( fts3isspace(cNext) 
       || cNext=='"' || cNext=='(' || cNext==')' || cNext==0
      ){
        pRet = (Fts3Expr *)fts3MallocZero(sizeof(Fts3Expr));
        if( !pRet ){
          return SQLITE_NOMEM;
        }
        pRet->eType = pKey->eType;
        pRet->nNear = nNear;
        *ppExpr = pRet;
        *pnConsumed = (int)((zInput - z) + nKey);
        return SQLITE_OK;
      }

      /* Turns out that wasn't a keyword after all. This happens if the
      ** user has supplied a token such as "ORacle". Continue.
      */
    }
  }

  /* See if we are dealing with a quoted phrase. If this is the case, then
  ** search for the closing quote and pass the whole string to getNextString()
  ** for processing. This is easy to do, as fts3 has no syntax for escaping
  ** a quote character embedded in a string.
  */
  if( *zInput=='"' ){
    for(ii=1; ii<nInput && zInput[ii]!='"'; ii++);
    *pnConsumed = (int)((zInput - z) + ii + 1);
    if( ii==nInput ){
      return SQLITE_ERROR;
    }
    return getNextString(pParse, &zInput[1], ii-1, ppExpr);
  }

  if( sqlite3_fts3_enable_parentheses ){
    if( *zInput=='(' ){
      int nConsumed = 0;
      pParse->nNest++;
      rc = fts3ExprParse(pParse, zInput+1, nInput-1, ppExpr, &nConsumed);
      if( rc==SQLITE_OK && !*ppExpr ){ rc = SQLITE_DONE; }
      *pnConsumed = (int)(zInput - z) + 1 + nConsumed;
      return rc;
    }else if( *zInput==')' ){
      pParse->nNest--;
      *pnConsumed = (int)((zInput - z) + 1);
      *ppExpr = 0;
      return SQLITE_DONE;
    }
  }

  /* If control flows to this point, this must be a regular token, or 
  ** the end of the input. Read a regular token using the sqlite3_tokenizer
  ** interface. Before doing so, figure out if there is an explicit
  ** column specifier for the token. 
  **
  ** TODO: Strangely, it is not possible to associate a column specifier
  ** with a quoted phrase, only with a single token. Not sure if this was
  ** an implementation artifact or an intentional decision when fts3 was
  ** first implemented. Whichever it was, this module duplicates the 
  ** limitation.
  */
  iCol = pParse->iDefaultCol;
  iColLen = 0;
  for(ii=0; ii<pParse->nCol; ii++){
    const char *zStr = pParse->azCol[ii];
    int nStr = (int)strlen(zStr);
    if( nInput>nStr && zInput[nStr]==':' 
     && sqlite3_strnicmp(zStr, zInput, nStr)==0 
    ){
      iCol = ii;
      iColLen = (int)((zInput - z) + nStr + 1);
      break;
    }
  }
  rc = getNextToken(pParse, iCol, &z[iColLen], n-iColLen, ppExpr, pnConsumed);
  *pnConsumed += iColLen;
  return rc;
}

/*
** The argument is an Fts3Expr structure for a binary operator (any type
** except an FTSQUERY_PHRASE). Return an integer value representing the
** precedence of the operator. Lower values have a higher precedence (i.e.
** group more tightly). For example, in the C language, the == operator
** groups more tightly than ||, and would therefore have a higher precedence.
**
** When using the new fts3 query syntax (when SQLITE_ENABLE_FTS3_PARENTHESIS
** is defined), the order of the operators in precedence from highest to
** lowest is:
**
**   NEAR
**   NOT
**   AND (including implicit ANDs)
**   OR
**
** Note that when using the old query syntax, the OR operator has a higher
** precedence than the AND operator.
*/
static int opPrecedence(Fts3Expr *p){
  assert( p->eType!=FTSQUERY_PHRASE );
  if( sqlite3_fts3_enable_parentheses ){
    return p->eType;
  }else if( p->eType==FTSQUERY_NEAR ){
    return 1;
  }else if( p->eType==FTSQUERY_OR ){
    return 2;
  }
  assert( p->eType==FTSQUERY_AND );
  return 3;
}

/*
** Argument ppHead contains a pointer to the current head of a query 
** expression tree being parsed. pPrev is the expression node most recently
** inserted into the tree. This function adds pNew, which is always a binary
** operator node, into the expression tree based on the relative precedence
** of pNew and the existing nodes of the tree. This may result in the head
** of the tree changing, in which case *ppHead is set to the new root node.
*/
static void insertBinaryOperator(
  Fts3Expr **ppHead,       /* Pointer to the root node of a tree */
  Fts3Expr *pPrev,         /* Node most recently inserted into the tree */
  Fts3Expr *pNew           /* New binary node to insert into expression tree */
){
  Fts3Expr *pSplit = pPrev;
  while( pSplit->pParent && opPrecedence(pSplit->pParent)<=opPrecedence(pNew) ){
    pSplit = pSplit->pParent;
  }

  if( pSplit->pParent ){
    assert( pSplit->pParent->pRight==pSplit );
    pSplit->pParent->pRight = pNew;
    pNew->pParent = pSplit->pParent;
  }else{
    *ppHead = pNew;
  }
  pNew->pLeft = pSplit;
  pSplit->pParent = pNew;
}

/*
** Parse the fts3 query expression found in buffer z, length n. This function
** returns either when the end of the buffer is reached or an unmatched 
** closing bracket - ')' - is encountered.
**
** If successful, SQLITE_OK is returned, *ppExpr is set to point to the
** parsed form of the expression and *pnConsumed is set to the number of
** bytes read from buffer z. Otherwise, *ppExpr is set to 0 and SQLITE_NOMEM
** (out of memory error) or SQLITE_ERROR (parse error) is returned.
*/
static int fts3ExprParse(
  ParseContext *pParse,                   /* fts3 query parse context */
  const char *z, int n,                   /* Text of MATCH query */
  Fts3Expr **ppExpr,                      /* OUT: Parsed query structure */
  int *pnConsumed                         /* OUT: Number of bytes consumed */
){
  Fts3Expr *pRet = 0;
  Fts3Expr *pPrev = 0;
  Fts3Expr *pNotBranch = 0;               /* Only used in legacy parse mode */
  int nIn = n;
  const char *zIn = z;
  int rc = SQLITE_OK;
  int isRequirePhrase = 1;

  while( rc==SQLITE_OK ){
    Fts3Expr *p = 0;
    int nByte = 0;

    rc = getNextNode(pParse, zIn, nIn, &p, &nByte);
    assert( nByte>0 || (rc!=SQLITE_OK && p==0) );
    if( rc==SQLITE_OK ){
      if( p ){
        int isPhrase;

        if( !sqlite3_fts3_enable_parentheses 
            && p->eType==FTSQUERY_PHRASE && pParse->isNot 
        ){
          /* Create an implicit NOT operator. */
          Fts3Expr *pNot = fts3MallocZero(sizeof(Fts3Expr));
          if( !pNot ){
            sqlite3Fts3ExprFree(p);
            rc = SQLITE_NOMEM;
            goto exprparse_out;
          }
          pNot->eType = FTSQUERY_NOT;
          pNot->pRight = p;
          p->pParent = pNot;
          if( pNotBranch ){
            pNot->pLeft = pNotBranch;
            pNotBranch->pParent = pNot;
          }
          pNotBranch = pNot;
          p = pPrev;
        }else{
          int eType = p->eType;
          isPhrase = (eType==FTSQUERY_PHRASE || p->pLeft);

          /* The isRequirePhrase variable is set to true if a phrase or
          ** an expression contained in parenthesis is required. If a
          ** binary operator (AND, OR, NOT or NEAR) is encounted when
          ** isRequirePhrase is set, this is a syntax error.
          */
          if( !isPhrase && isRequirePhrase ){
            sqlite3Fts3ExprFree(p);
            rc = SQLITE_ERROR;
            goto exprparse_out;
          }

          if( isPhrase && !isRequirePhrase ){
            /* Insert an implicit AND operator. */
            Fts3Expr *pAnd;
            assert( pRet && pPrev );
            pAnd = fts3MallocZero(sizeof(Fts3Expr));
            if( !pAnd ){
              sqlite3Fts3ExprFree(p);
              rc = SQLITE_NOMEM;
              goto exprparse_out;
            }
            pAnd->eType = FTSQUERY_AND;
            insertBinaryOperator(&pRet, pPrev, pAnd);
            pPrev = pAnd;
          }

          /* This test catches attempts to make either operand of a NEAR
           ** operator something other than a phrase. For example, either of
           ** the following:
           **
           **    (bracketed expression) NEAR phrase
           **    phrase NEAR (bracketed expression)
           **
           ** Return an error in either case.
           */
          if( pPrev && (
            (eType==FTSQUERY_NEAR && !isPhrase && pPrev->eType!=FTSQUERY_PHRASE)
         || (eType!=FTSQUERY_PHRASE && isPhrase && pPrev->eType==FTSQUERY_NEAR)
          )){
            sqlite3Fts3ExprFree(p);
            rc = SQLITE_ERROR;
            goto exprparse_out;
          }

          if( isPhrase ){
            if( pRet ){
              assert( pPrev && pPrev->pLeft && pPrev->pRight==0 );
              pPrev->pRight = p;
              p->pParent = pPrev;
            }else{
              pRet = p;
            }
          }else{
            insertBinaryOperator(&pRet, pPrev, p);
          }
          isRequirePhrase = !isPhrase;
        }
        pPrev = p;
      }
      assert( nByte>0 );
    }
    assert( rc!=SQLITE_OK || (nByte>0 && nByte<=nIn) );
    nIn -= nByte;
    zIn += nByte;
  }

  if( rc==SQLITE_DONE && pRet && isRequirePhrase ){
    rc = SQLITE_ERROR;
  }

  if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
    if( !sqlite3_fts3_enable_parentheses && pNotBranch ){
      if( !pRet ){
        rc = SQLITE_ERROR;
      }else{
        Fts3Expr *pIter = pNotBranch;
        while( pIter->pLeft ){
          pIter = pIter->pLeft;
        }
        pIter->pLeft = pRet;
        pRet->pParent = pIter;
        pRet = pNotBranch;
      }
    }
  }
  *pnConsumed = n - nIn;

exprparse_out:
  if( rc!=SQLITE_OK ){
    sqlite3Fts3ExprFree(pRet);
    sqlite3Fts3ExprFree(pNotBranch);
    pRet = 0;
  }
  *ppExpr = pRet;
  return rc;
}

/*
** Return SQLITE_ERROR if the maximum depth of the expression tree passed 
** as the only argument is more than nMaxDepth.
*/
static int fts3ExprCheckDepth(Fts3Expr *p, int nMaxDepth){
  int rc = SQLITE_OK;
  if( p ){
    if( nMaxDepth<0 ){ 
      rc = SQLITE_TOOBIG;
    }else{
      rc = fts3ExprCheckDepth(p->pLeft, nMaxDepth-1);
      if( rc==SQLITE_OK ){
        rc = fts3ExprCheckDepth(p->pRight, nMaxDepth-1);
      }
    }
  }
  return rc;
}

/*
** This function attempts to transform the expression tree at (*pp) to
** an equivalent but more balanced form. The tree is modified in place.
** If successful, SQLITE_OK is returned and (*pp) set to point to the 
** new root expression node. 
**
** nMaxDepth is the maximum allowable depth of the balanced sub-tree.
**
** Otherwise, if an error occurs, an SQLite error code is returned and 
** expression (*pp) freed.
*/
static int fts3ExprBalance(Fts3Expr **pp, int nMaxDepth){
  int rc = SQLITE_OK;             /* Return code */
  Fts3Expr *pRoot = *pp;          /* Initial root node */
  Fts3Expr *pFree = 0;            /* List of free nodes. Linked by pParent. */
  int eType = pRoot->eType;       /* Type of node in this tree */

  if( nMaxDepth==0 ){
    rc = SQLITE_ERROR;
  }

  if( rc==SQLITE_OK && (eType==FTSQUERY_AND || eType==FTSQUERY_OR) ){
    Fts3Expr **apLeaf;
    apLeaf = (Fts3Expr **)sqlite3_malloc(sizeof(Fts3Expr *) * nMaxDepth);
    if( 0==apLeaf ){
      rc = SQLITE_NOMEM;
    }else{
      memset(apLeaf, 0, sizeof(Fts3Expr *) * nMaxDepth);
    }

    if( rc==SQLITE_OK ){
      int i;
      Fts3Expr *p;

      /* Set $p to point to the left-most leaf in the tree of eType nodes. */
      for(p=pRoot; p->eType==eType; p=p->pLeft){
        assert( p->pParent==0 || p->pParent->pLeft==p );
        assert( p->pLeft && p->pRight );
      }

      /* This loop runs once for each leaf in the tree of eType nodes. */
      while( 1 ){
        int iLvl;
        Fts3Expr *pParent = p->pParent;     /* Current parent of p */

        assert( pParent==0 || pParent->pLeft==p );
        p->pParent = 0;
        if( pParent ){
          pParent->pLeft = 0;
        }else{
          pRoot = 0;
        }
        rc = fts3ExprBalance(&p, nMaxDepth-1);
        if( rc!=SQLITE_OK ) break;

        for(iLvl=0; p && iLvl<nMaxDepth; iLvl++){
          if( apLeaf[iLvl]==0 ){
            apLeaf[iLvl] = p;
            p = 0;
          }else{
            assert( pFree );
            pFree->pLeft = apLeaf[iLvl];
            pFree->pRight = p;
            pFree->pLeft->pParent = pFree;
            pFree->pRight->pParent = pFree;

            p = pFree;
            pFree = pFree->pParent;
            p->pParent = 0;
            apLeaf[iLvl] = 0;
          }
        }
        if( p ){
          sqlite3Fts3ExprFree(p);
          rc = SQLITE_TOOBIG;
          break;
        }

        /* If that was the last leaf node, break out of the loop */
        if( pParent==0 ) break;

        /* Set $p to point to the next leaf in the tree of eType nodes */
        for(p=pParent->pRight; p->eType==eType; p=p->pLeft);

        /* Remove pParent from the original tree. */
        assert( pParent->pParent==0 || pParent->pParent->pLeft==pParent );
        pParent->pRight->pParent = pParent->pParent;
        if( pParent->pParent ){
          pParent->pParent->pLeft = pParent->pRight;
        }else{
          assert( pParent==pRoot );
          pRoot = pParent->pRight;
        }

        /* Link pParent into the free node list. It will be used as an
        ** internal node of the new tree.  */
        pParent->pParent = pFree;
        pFree = pParent;
      }

      if( rc==SQLITE_OK ){
        p = 0;
        for(i=0; i<nMaxDepth; i++){
          if( apLeaf[i] ){
            if( p==0 ){
              p = apLeaf[i];
              p->pParent = 0;
            }else{
              assert( pFree!=0 );
              pFree->pRight = p;
              pFree->pLeft = apLeaf[i];
              pFree->pLeft->pParent = pFree;
              pFree->pRight->pParent = pFree;

              p = pFree;
              pFree = pFree->pParent;
              p->pParent = 0;
            }
          }
        }
        pRoot = p;
      }else{
        /* An error occurred. Delete the contents of the apLeaf[] array 
        ** and pFree list. Everything else is cleaned up by the call to
        ** sqlite3Fts3ExprFree(pRoot) below.  */
        Fts3Expr *pDel;
        for(i=0; i<nMaxDepth; i++){
          sqlite3Fts3ExprFree(apLeaf[i]);
        }
        while( (pDel=pFree)!=0 ){
          pFree = pDel->pParent;
          sqlite3_free(pDel);
        }
      }

      assert( pFree==0 );
      sqlite3_free( apLeaf );
    }
  }

  if( rc!=SQLITE_OK ){
    sqlite3Fts3ExprFree(pRoot);
    pRoot = 0;
  }
  *pp = pRoot;
  return rc;
}

/*
** This function is similar to sqlite3Fts3ExprParse(), with the following
** differences:
**
**   1. It does not do expression rebalancing.
**   2. It does not check that the expression does not exceed the 
**      maximum allowable depth.
**   3. Even if it fails, *ppExpr may still be set to point to an 
**      expression tree. It should be deleted using sqlite3Fts3ExprFree()
**      in this case.
*/
static int fts3ExprParseUnbalanced(
  sqlite3_tokenizer *pTokenizer,      /* Tokenizer module */
  int iLangid,                        /* Language id for tokenizer */
  char **azCol,                       /* Array of column names for fts3 table */
  int bFts4,                          /* True to allow FTS4-only syntax */
  int nCol,                           /* Number of entries in azCol[] */
  int iDefaultCol,                    /* Default column to query */
  const char *z, int n,               /* Text of MATCH query */
  Fts3Expr **ppExpr                   /* OUT: Parsed query structure */
){
  int nParsed;
  int rc;
  ParseContext sParse;

  memset(&sParse, 0, sizeof(ParseContext));
  sParse.pTokenizer = pTokenizer;
  sParse.iLangid = iLangid;
  sParse.azCol = (const char **)azCol;
  sParse.nCol = nCol;
  sParse.iDefaultCol = iDefaultCol;
  sParse.bFts4 = bFts4;
  if( z==0 ){
    *ppExpr = 0;
    return SQLITE_OK;
  }
  if( n<0 ){
    n = (int)strlen(z);
  }
  rc = fts3ExprParse(&sParse, z, n, ppExpr, &nParsed);
  assert( rc==SQLITE_OK || *ppExpr==0 );

  /* Check for mismatched parenthesis */
  if( rc==SQLITE_OK && sParse.nNest ){
    rc = SQLITE_ERROR;
  }
  
  return rc;
}

/*
** Parameters z and n contain a pointer to and length of a buffer containing
** an fts3 query expression, respectively. This function attempts to parse the
** query expression and create a tree of Fts3Expr structures representing the
** parsed expression. If successful, *ppExpr is set to point to the head
** of the parsed expression tree and SQLITE_OK is returned. If an error
** occurs, either SQLITE_NOMEM (out-of-memory error) or SQLITE_ERROR (parse
** error) is returned and *ppExpr is set to 0.
**
** If parameter n is a negative number, then z is assumed to point to a
** nul-terminated string and the length is determined using strlen().
**
** The first parameter, pTokenizer, is passed the fts3 tokenizer module to
** use to normalize query tokens while parsing the expression. The azCol[]
** array, which is assumed to contain nCol entries, should contain the names
** of each column in the target fts3 table, in order from left to right. 
** Column names must be nul-terminated strings.
**
** The iDefaultCol parameter should be passed the index of the table column
** that appears on the left-hand-side of the MATCH operator (the default
** column to match against for tokens for which a column name is not explicitly
** specified as part of the query string), or -1 if tokens may by default
** match any table column.
*/
SQLITE_PRIVATE int sqlite3Fts3ExprParse(
  sqlite3_tokenizer *pTokenizer,      /* Tokenizer module */
  int iLangid,                        /* Language id for tokenizer */
  char **azCol,                       /* Array of column names for fts3 table */
  int bFts4,                          /* True to allow FTS4-only syntax */
  int nCol,                           /* Number of entries in azCol[] */
  int iDefaultCol,                    /* Default column to query */
  const char *z, int n,               /* Text of MATCH query */
  Fts3Expr **ppExpr,                  /* OUT: Parsed query structure */
  char **pzErr                        /* OUT: Error message (sqlite3_malloc) */
){
  int rc = fts3ExprParseUnbalanced(
      pTokenizer, iLangid, azCol, bFts4, nCol, iDefaultCol, z, n, ppExpr
  );
  
  /* Rebalance the expression. And check that its depth does not exceed
  ** SQLITE_FTS3_MAX_EXPR_DEPTH.  */
  if( rc==SQLITE_OK && *ppExpr ){
    rc = fts3ExprBalance(ppExpr, SQLITE_FTS3_MAX_EXPR_DEPTH);
    if( rc==SQLITE_OK ){
      rc = fts3ExprCheckDepth(*ppExpr, SQLITE_FTS3_MAX_EXPR_DEPTH);
    }
  }

  if( rc!=SQLITE_OK ){
    sqlite3Fts3ExprFree(*ppExpr);
    *ppExpr = 0;
    if( rc==SQLITE_TOOBIG ){
      *pzErr = sqlite3_mprintf(
          "FTS expression tree is too large (maximum depth %d)", 
          SQLITE_FTS3_MAX_EXPR_DEPTH
      );
      rc = SQLITE_ERROR;
    }else if( rc==SQLITE_ERROR ){
      *pzErr = sqlite3_mprintf("malformed MATCH expression: [%s]", z);
    }
  }

  return rc;
}

/*
** Free a single node of an expression tree.
*/
static void fts3FreeExprNode(Fts3Expr *p){
  assert( p->eType==FTSQUERY_PHRASE || p->pPhrase==0 );
  sqlite3Fts3EvalPhraseCleanup(p->pPhrase);
  sqlite3_free(p->aMI);
  sqlite3_free(p);
}

/*
** Free a parsed fts3 query expression allocated by sqlite3Fts3ExprParse().
**
** This function would be simpler if it recursively called itself. But
** that would mean passing a sufficiently large expression to ExprParse()
** could cause a stack overflow.
*/
SQLITE_PRIVATE void sqlite3Fts3ExprFree(Fts3Expr *pDel){
  Fts3Expr *p;
  assert( pDel==0 || pDel->pParent==0 );
  for(p=pDel; p && (p->pLeft||p->pRight); p=(p->pLeft ? p->pLeft : p->pRight)){
    assert( p->pParent==0 || p==p->pParent->pRight || p==p->pParent->pLeft );
  }
  while( p ){
    Fts3Expr *pParent = p->pParent;
    fts3FreeExprNode(p);
    if( pParent && p==pParent->pLeft && pParent->pRight ){
      p = pParent->pRight;
      while( p && (p->pLeft || p->pRight) ){
        assert( p==p->pParent->pRight || p==p->pParent->pLeft );
        p = (p->pLeft ? p->pLeft : p->pRight);
      }
    }else{
      p = pParent;
    }
  }
}

/****************************************************************************
*****************************************************************************
** Everything after this point is just test code.
*/

#ifdef SQLITE_TEST

/* #include <stdio.h> */

/*
** Function to query the hash-table of tokenizers (see README.tokenizers).
*/
static int queryTestTokenizer(
  sqlite3 *db, 
  const char *zName,  
  const sqlite3_tokenizer_module **pp
){
  int rc;
  sqlite3_stmt *pStmt;
  const char zSql[] = "SELECT fts3_tokenizer(?)";

  *pp = 0;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  sqlite3_bind_text(pStmt, 1, zName, -1, SQLITE_STATIC);
  if( SQLITE_ROW==sqlite3_step(pStmt) ){
    if( sqlite3_column_type(pStmt, 0)==SQLITE_BLOB ){
      memcpy((void *)pp, sqlite3_column_blob(pStmt, 0), sizeof(*pp));
    }
  }

  return sqlite3_finalize(pStmt);
}

/*
** Return a pointer to a buffer containing a text representation of the
** expression passed as the first argument. The buffer is obtained from
** sqlite3_malloc(). It is the responsibility of the caller to use 
** sqlite3_free() to release the memory. If an OOM condition is encountered,
** NULL is returned.
**
** If the second argument is not NULL, then its contents are prepended to 
** the returned expression text and then freed using sqlite3_free().
*/
static char *exprToString(Fts3Expr *pExpr, char *zBuf){
  if( pExpr==0 ){
    return sqlite3_mprintf("");
  }
  switch( pExpr->eType ){
    case FTSQUERY_PHRASE: {
      Fts3Phrase *pPhrase = pExpr->pPhrase;
      int i;
      zBuf = sqlite3_mprintf(
          "%zPHRASE %d 0", zBuf, pPhrase->iColumn);
      for(i=0; zBuf && i<pPhrase->nToken; i++){
        zBuf = sqlite3_mprintf("%z %.*s%s", zBuf, 
            pPhrase->aToken[i].n, pPhrase->aToken[i].z,
            (pPhrase->aToken[i].isPrefix?"+":"")
        );
      }
      return zBuf;
    }

    case FTSQUERY_NEAR:
      zBuf = sqlite3_mprintf("%zNEAR/%d ", zBuf, pExpr->nNear);
      break;
    case FTSQUERY_NOT:
      zBuf = sqlite3_mprintf("%zNOT ", zBuf);
      break;
    case FTSQUERY_AND:
      zBuf = sqlite3_mprintf("%zAND ", zBuf);
      break;
    case FTSQUERY_OR:
      zBuf = sqlite3_mprintf("%zOR ", zBuf);
      break;
  }

  if( zBuf ) zBuf = sqlite3_mprintf("%z{", zBuf);
  if( zBuf ) zBuf = exprToString(pExpr->pLeft, zBuf);
  if( zBuf ) zBuf = sqlite3_mprintf("%z} {", zBuf);

  if( zBuf ) zBuf = exprToString(pExpr->pRight, zBuf);
  if( zBuf ) zBuf = sqlite3_mprintf("%z}", zBuf);

  return zBuf;
}

/*
** This is the implementation of a scalar SQL function used to test the 
** expression parser. It should be called as follows:
**
**   fts3_exprtest(<tokenizer>, <expr>, <column 1>, ...);
**
** The first argument, <tokenizer>, is the name of the fts3 tokenizer used
** to parse the query expression (see README.tokenizers). The second argument
** is the query expression to parse. Each subsequent argument is the name
** of a column of the fts3 table that the query expression may refer to.
** For example:
**
**   SELECT fts3_exprtest('simple', 'Bill col2:Bloggs', 'col1', 'col2');
*/
static void fts3ExprTest(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  sqlite3_tokenizer_module const *pModule = 0;
  sqlite3_tokenizer *pTokenizer = 0;
  int rc;
  char **azCol = 0;
  const char *zExpr;
  int nExpr;
  int nCol;
  int ii;
  Fts3Expr *pExpr;
  char *zBuf = 0;
  sqlite3 *db = sqlite3_context_db_handle(context);

  if( argc<3 ){
    sqlite3_result_error(context, 
        "Usage: fts3_exprtest(tokenizer, expr, col1, ...", -1
    );
    return;
  }

  rc = queryTestTokenizer(db,
                          (const char *)sqlite3_value_text(argv[0]), &pModule);
  if( rc==SQLITE_NOMEM ){
    sqlite3_result_error_nomem(context);
    goto exprtest_out;
  }else if( !pModule ){
    sqlite3_result_error(context, "No such tokenizer module", -1);
    goto exprtest_out;
  }

  rc = pModule->xCreate(0, 0, &pTokenizer);
  assert( rc==SQLITE_NOMEM || rc==SQLITE_OK );
  if( rc==SQLITE_NOMEM ){
    sqlite3_result_error_nomem(context);
    goto exprtest_out;
  }
  pTokenizer->pModule = pModule;

  zExpr = (const char *)sqlite3_value_text(argv[1]);
  nExpr = sqlite3_value_bytes(argv[1]);
  nCol = argc-2;
  azCol = (char **)sqlite3_malloc(nCol*sizeof(char *));
  if( !azCol ){
    sqlite3_result_error_nomem(context);
    goto exprtest_out;
  }
  for(ii=0; ii<nCol; ii++){
    azCol[ii] = (char *)sqlite3_value_text(argv[ii+2]);
  }

  if( sqlite3_user_data(context) ){
    char *zDummy = 0;
    rc = sqlite3Fts3ExprParse(
        pTokenizer, 0, azCol, 0, nCol, nCol, zExpr, nExpr, &pExpr, &zDummy
    );
    assert( rc==SQLITE_OK || pExpr==0 );
    sqlite3_free(zDummy);
  }else{
    rc = fts3ExprParseUnbalanced(
        pTokenizer, 0, azCol, 0, nCol, nCol, zExpr, nExpr, &pExpr
    );
  }

  if( rc!=SQLITE_OK && rc!=SQLITE_NOMEM ){
    sqlite3Fts3ExprFree(pExpr);
    sqlite3_result_error(context, "Error parsing expression", -1);
  }else if( rc==SQLITE_NOMEM || !(zBuf = exprToString(pExpr, 0)) ){
    sqlite3_result_error_nomem(context);
  }else{
    sqlite3_result_text(context, zBuf, -1, SQLITE_TRANSIENT);
    sqlite3_free(zBuf);
  }

  sqlite3Fts3ExprFree(pExpr);

exprtest_out:
  if( pModule && pTokenizer ){
    rc = pModule->xDestroy(pTokenizer);
  }
  sqlite3_free(azCol);
}

/*
** Register the query expression parser test function fts3_exprtest() 
** with database connection db. 
*/
SQLITE_PRIVATE int sqlite3Fts3ExprInitTestInterface(sqlite3* db){
  int rc = sqlite3_create_function(
      db, "fts3_exprtest", -1, SQLITE_UTF8, 0, fts3ExprTest, 0, 0
  );
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "fts3_exprtest_rebalance", 
        -1, SQLITE_UTF8, (void *)1, fts3ExprTest, 0, 0
    );
  }
  return rc;
}

#endif
#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */

/************** End of fts3_expr.c *******************************************/
/************** Begin file fts3_hash.c ***************************************/
/*
** 2001 September 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This is the implementation of generic hash-tables used in SQLite.
** We've modified it slightly to serve as a standalone hash table
** implementation for the full-text indexing module.
*/

/*
** The code in this file is only compiled if:
**
**     * The FTS3 module is being built as an extension
**       (in which case SQLITE_CORE is not defined), or
**
**     * The FTS3 module is being built into the core of
**       SQLite (in which case SQLITE_ENABLE_FTS3 is defined).
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <assert.h> */
/* #include <stdlib.h> */
/* #include <string.h> */


/*
** Malloc and Free functions
*/
static void *fts3HashMalloc(int n){
  void *p = sqlite3_malloc(n);
  if( p ){
    memset(p, 0, n);
  }
  return p;
}
static void fts3HashFree(void *p){
  sqlite3_free(p);
}

/* Turn bulk memory into a hash table object by initializing the
** fields of the Hash structure.
**
** "pNew" is a pointer to the hash table that is to be initialized.
** keyClass is one of the constants 
** FTS3_HASH_BINARY or FTS3_HASH_STRING.  The value of keyClass 
** determines what kind of key the hash table will use.  "copyKey" is
** true if the hash table should make its own private copy of keys and
** false if it should just use the supplied pointer.
*/
SQLITE_PRIVATE void sqlite3Fts3HashInit(Fts3Hash *pNew, char keyClass, char copyKey){
  assert( pNew!=0 );
  assert( keyClass>=FTS3_HASH_STRING && keyClass<=FTS3_HASH_BINARY );
  pNew->keyClass = keyClass;
  pNew->copyKey = copyKey;
  pNew->first = 0;
  pNew->count = 0;
  pNew->htsize = 0;
  pNew->ht = 0;
}

/* Remove all entries from a hash table.  Reclaim all memory.
** Call this routine to delete a hash table or to reset a hash table
** to the empty state.
*/
SQLITE_PRIVATE void sqlite3Fts3HashClear(Fts3Hash *pH){
  Fts3HashElem *elem;         /* For looping over all elements of the table */

  assert( pH!=0 );
  elem = pH->first;
  pH->first = 0;
  fts3HashFree(pH->ht);
  pH->ht = 0;
  pH->htsize = 0;
  while( elem ){
    Fts3HashElem *next_elem = elem->next;
    if( pH->copyKey && elem->pKey ){
      fts3HashFree(elem->pKey);
    }
    fts3HashFree(elem);
    elem = next_elem;
  }
  pH->count = 0;
}

/*
** Hash and comparison functions when the mode is FTS3_HASH_STRING
*/
static int fts3StrHash(const void *pKey, int nKey){
  const char *z = (const char *)pKey;
  unsigned h = 0;
  if( nKey<=0 ) nKey = (int) strlen(z);
  while( nKey > 0  ){
    h = (h<<3) ^ h ^ *z++;
    nKey--;
  }
  return (int)(h & 0x7fffffff);
}
static int fts3StrCompare(const void *pKey1, int n1, const void *pKey2, int n2){
  if( n1!=n2 ) return 1;
  return strncmp((const char*)pKey1,(const char*)pKey2,n1);
}

/*
** Hash and comparison functions when the mode is FTS3_HASH_BINARY
*/
static int fts3BinHash(const void *pKey, int nKey){
  int h = 0;
  const char *z = (const char *)pKey;
  while( nKey-- > 0 ){
    h = (h<<3) ^ h ^ *(z++);
  }
  return h & 0x7fffffff;
}
static int fts3BinCompare(const void *pKey1, int n1, const void *pKey2, int n2){
  if( n1!=n2 ) return 1;
  return memcmp(pKey1,pKey2,n1);
}

/*
** Return a pointer to the appropriate hash function given the key class.
**
** The C syntax in this function definition may be unfamilar to some 
** programmers, so we provide the following additional explanation:
**
** The name of the function is "ftsHashFunction".  The function takes a
** single parameter "keyClass".  The return value of ftsHashFunction()
** is a pointer to another function.  Specifically, the return value
** of ftsHashFunction() is a pointer to a function that takes two parameters
** with types "const void*" and "int" and returns an "int".
*/
static int (*ftsHashFunction(int keyClass))(const void*,int){
  if( keyClass==FTS3_HASH_STRING ){
    return &fts3StrHash;
  }else{
    assert( keyClass==FTS3_HASH_BINARY );
    return &fts3BinHash;
  }
}

/*
** Return a pointer to the appropriate hash function given the key class.
**
** For help in interpreted the obscure C code in the function definition,
** see the header comment on the previous function.
*/
static int (*ftsCompareFunction(int keyClass))(const void*,int,const void*,int){
  if( keyClass==FTS3_HASH_STRING ){
    return &fts3StrCompare;
  }else{
    assert( keyClass==FTS3_HASH_BINARY );
    return &fts3BinCompare;
  }
}

/* Link an element into the hash table
*/
static void fts3HashInsertElement(
  Fts3Hash *pH,            /* The complete hash table */
  struct _fts3ht *pEntry,  /* The entry into which pNew is inserted */
  Fts3HashElem *pNew       /* The element to be inserted */
){
  Fts3HashElem *pHead;     /* First element already in pEntry */
  pHead = pEntry->chain;
  if( pHead ){
    pNew->next = pHead;
    pNew->prev = pHead->prev;
    if( pHead->prev ){ pHead->prev->next = pNew; }
    else             { pH->first = pNew; }
    pHead->prev = pNew;
  }else{
    pNew->next = pH->first;
    if( pH->first ){ pH->first->prev = pNew; }
    pNew->prev = 0;
    pH->first = pNew;
  }
  pEntry->count++;
  pEntry->chain = pNew;
}


/* Resize the hash table so that it cantains "new_size" buckets.
** "new_size" must be a power of 2.  The hash table might fail 
** to resize if sqliteMalloc() fails.
**
** Return non-zero if a memory allocation error occurs.
*/
static int fts3Rehash(Fts3Hash *pH, int new_size){
  struct _fts3ht *new_ht;          /* The new hash table */
  Fts3HashElem *elem, *next_elem;  /* For looping over existing elements */
  int (*xHash)(const void*,int);   /* The hash function */

  assert( (new_size & (new_size-1))==0 );
  new_ht = (struct _fts3ht *)fts3HashMalloc( new_size*sizeof(struct _fts3ht) );
  if( new_ht==0 ) return 1;
  fts3HashFree(pH->ht);
  pH->ht = new_ht;
  pH->htsize = new_size;
  xHash = ftsHashFunction(pH->keyClass);
  for(elem=pH->first, pH->first=0; elem; elem = next_elem){
    int h = (*xHash)(elem->pKey, elem->nKey) & (new_size-1);
    next_elem = elem->next;
    fts3HashInsertElement(pH, &new_ht[h], elem);
  }
  return 0;
}

/* This function (for internal use only) locates an element in an
** hash table that matches the given key.  The hash for this key has
** already been computed and is passed as the 4th parameter.
*/
static Fts3HashElem *fts3FindElementByHash(
  const Fts3Hash *pH, /* The pH to be searched */
  const void *pKey,   /* The key we are searching for */
  int nKey,
  int h               /* The hash for this key. */
){
  Fts3HashElem *elem;            /* Used to loop thru the element list */
  int count;                     /* Number of elements left to test */
  int (*xCompare)(const void*,int,const void*,int);  /* comparison function */

  if( pH->ht ){
    struct _fts3ht *pEntry = &pH->ht[h];
    elem = pEntry->chain;
    count = pEntry->count;
    xCompare = ftsCompareFunction(pH->keyClass);
    while( count-- && elem ){
      if( (*xCompare)(elem->pKey,elem->nKey,pKey,nKey)==0 ){ 
        return elem;
      }
      elem = elem->next;
    }
  }
  return 0;
}

/* Remove a single entry from the hash table given a pointer to that
** element and a hash on the element's key.
*/
static void fts3RemoveElementByHash(
  Fts3Hash *pH,         /* The pH containing "elem" */
  Fts3HashElem* elem,   /* The element to be removed from the pH */
  int h                 /* Hash value for the element */
){
  struct _fts3ht *pEntry;
  if( elem->prev ){
    elem->prev->next = elem->next; 
  }else{
    pH->first = elem->next;
  }
  if( elem->next ){
    elem->next->prev = elem->prev;
  }
  pEntry = &pH->ht[h];
  if( pEntry->chain==elem ){
    pEntry->chain = elem->next;
  }
  pEntry->count--;
  if( pEntry->count<=0 ){
    pEntry->chain = 0;
  }
  if( pH->copyKey && elem->pKey ){
    fts3HashFree(elem->pKey);
  }
  fts3HashFree( elem );
  pH->count--;
  if( pH->count<=0 ){
    assert( pH->first==0 );
    assert( pH->count==0 );
    fts3HashClear(pH);
  }
}

SQLITE_PRIVATE Fts3HashElem *sqlite3Fts3HashFindElem(
  const Fts3Hash *pH, 
  const void *pKey, 
  int nKey
){
  int h;                          /* A hash on key */
  int (*xHash)(const void*,int);  /* The hash function */

  if( pH==0 || pH->ht==0 ) return 0;
  xHash = ftsHashFunction(pH->keyClass);
  assert( xHash!=0 );
  h = (*xHash)(pKey,nKey);
  assert( (pH->htsize & (pH->htsize-1))==0 );
  return fts3FindElementByHash(pH,pKey,nKey, h & (pH->htsize-1));
}

/* 
** Attempt to locate an element of the hash table pH with a key
** that matches pKey,nKey.  Return the data for this element if it is
** found, or NULL if there is no match.
*/
SQLITE_PRIVATE void *sqlite3Fts3HashFind(const Fts3Hash *pH, const void *pKey, int nKey){
  Fts3HashElem *pElem;            /* The element that matches key (if any) */

  pElem = sqlite3Fts3HashFindElem(pH, pKey, nKey);
  return pElem ? pElem->data : 0;
}

/* Insert an element into the hash table pH.  The key is pKey,nKey
** and the data is "data".
**
** If no element exists with a matching key, then a new
** element is created.  A copy of the key is made if the copyKey
** flag is set.  NULL is returned.
**
** If another element already exists with the same key, then the
** new data replaces the old data and the old data is returned.
** The key is not copied in this instance.  If a malloc fails, then
** the new data is returned and the hash table is unchanged.
**
** If the "data" parameter to this function is NULL, then the
** element corresponding to "key" is removed from the hash table.
*/
SQLITE_PRIVATE void *sqlite3Fts3HashInsert(
  Fts3Hash *pH,        /* The hash table to insert into */
  const void *pKey,    /* The key */
  int nKey,            /* Number of bytes in the key */
  void *data           /* The data */
){
  int hraw;                 /* Raw hash value of the key */
  int h;                    /* the hash of the key modulo hash table size */
  Fts3HashElem *elem;       /* Used to loop thru the element list */
  Fts3HashElem *new_elem;   /* New element added to the pH */
  int (*xHash)(const void*,int);  /* The hash function */

  assert( pH!=0 );
  xHash = ftsHashFunction(pH->keyClass);
  assert( xHash!=0 );
  hraw = (*xHash)(pKey, nKey);
  assert( (pH->htsize & (pH->htsize-1))==0 );
  h = hraw & (pH->htsize-1);
  elem = fts3FindElementByHash(pH,pKey,nKey,h);
  if( elem ){
    void *old_data = elem->data;
    if( data==0 ){
      fts3RemoveElementByHash(pH,elem,h);
    }else{
      elem->data = data;
    }
    return old_data;
  }
  if( data==0 ) return 0;
  if( (pH->htsize==0 && fts3Rehash(pH,8))
   || (pH->count>=pH->htsize && fts3Rehash(pH, pH->htsize*2))
  ){
    pH->count = 0;
    return data;
  }
  assert( pH->htsize>0 );
  new_elem = (Fts3HashElem*)fts3HashMalloc( sizeof(Fts3HashElem) );
  if( new_elem==0 ) return data;
  if( pH->copyKey && pKey!=0 ){
    new_elem->pKey = fts3HashMalloc( nKey );
    if( new_elem->pKey==0 ){
      fts3HashFree(new_elem);
      return data;
    }
    memcpy((void*)new_elem->pKey, pKey, nKey);
  }else{
    new_elem->pKey = (void*)pKey;
  }
  new_elem->nKey = nKey;
  pH->count++;
  assert( pH->htsize>0 );
  assert( (pH->htsize & (pH->htsize-1))==0 );
  h = hraw & (pH->htsize-1);
  fts3HashInsertElement(pH, &pH->ht[h], new_elem);
  new_elem->data = data;
  return 0;
}

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */

/************** End of fts3_hash.c *******************************************/
/************** Begin file fts3_porter.c *************************************/
/*
** 2006 September 30
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Implementation of the full-text-search tokenizer that implements
** a Porter stemmer.
*/

/*
** The code in this file is only compiled if:
**
**     * The FTS3 module is being built as an extension
**       (in which case SQLITE_CORE is not defined), or
**
**     * The FTS3 module is being built into the core of
**       SQLite (in which case SQLITE_ENABLE_FTS3 is defined).
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <assert.h> */
/* #include <stdlib.h> */
/* #include <stdio.h> */
/* #include <string.h> */


/*
** Class derived from sqlite3_tokenizer
*/
typedef struct porter_tokenizer {
  sqlite3_tokenizer base;      /* Base class */
} porter_tokenizer;

/*
** Class derived from sqlite3_tokenizer_cursor
*/
typedef struct porter_tokenizer_cursor {
  sqlite3_tokenizer_cursor base;
  const char *zInput;          /* input we are tokenizing */
  int nInput;                  /* size of the input */
  int iOffset;                 /* current position in zInput */
  int iToken;                  /* index of next token to be returned */
  char *zToken;                /* storage for current token */
  int nAllocated;              /* space allocated to zToken buffer */
} porter_tokenizer_cursor;


/*
** Create a new tokenizer instance.
*/
static int porterCreate(
  int argc, const char * const *argv,
  sqlite3_tokenizer **ppTokenizer
){
  porter_tokenizer *t;

  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(argv);

  t = (porter_tokenizer *) sqlite3_malloc(sizeof(*t));
  if( t==NULL ) return SQLITE_NOMEM;
  memset(t, 0, sizeof(*t));
  *ppTokenizer = &t->base;
  return SQLITE_OK;
}

/*
** Destroy a tokenizer
*/
static int porterDestroy(sqlite3_tokenizer *pTokenizer){
  sqlite3_free(pTokenizer);
  return SQLITE_OK;
}

/*
** Prepare to begin tokenizing a particular string.  The input
** string to be tokenized is zInput[0..nInput-1].  A cursor
** used to incrementally tokenize this string is returned in 
** *ppCursor.
*/
static int porterOpen(
  sqlite3_tokenizer *pTokenizer,         /* The tokenizer */
  const char *zInput, int nInput,        /* String to be tokenized */
  sqlite3_tokenizer_cursor **ppCursor    /* OUT: Tokenization cursor */
){
  porter_tokenizer_cursor *c;

  UNUSED_PARAMETER(pTokenizer);

  c = (porter_tokenizer_cursor *) sqlite3_malloc(sizeof(*c));
  if( c==NULL ) return SQLITE_NOMEM;

  c->zInput = zInput;
  if( zInput==0 ){
    c->nInput = 0;
  }else if( nInput<0 ){
    c->nInput = (int)strlen(zInput);
  }else{
    c->nInput = nInput;
  }
  c->iOffset = 0;                 /* start tokenizing at the beginning */
  c->iToken = 0;
  c->zToken = NULL;               /* no space allocated, yet. */
  c->nAllocated = 0;

  *ppCursor = &c->base;
  return SQLITE_OK;
}

/*
** Close a tokenization cursor previously opened by a call to
** porterOpen() above.
*/
static int porterClose(sqlite3_tokenizer_cursor *pCursor){
  porter_tokenizer_cursor *c = (porter_tokenizer_cursor *) pCursor;
  sqlite3_free(c->zToken);
  sqlite3_free(c);
  return SQLITE_OK;
}
/*
** Vowel or consonant
*/
static const char cType[] = {
   0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0,
   1, 1, 1, 2, 1
};

/*
** isConsonant() and isVowel() determine if their first character in
** the string they point to is a consonant or a vowel, according
** to Porter ruls.  
**
** A consonate is any letter other than 'a', 'e', 'i', 'o', or 'u'.
** 'Y' is a consonant unless it follows another consonant,
** in which case it is a vowel.
**
** In these routine, the letters are in reverse order.  So the 'y' rule
** is that 'y' is a consonant unless it is followed by another
** consonent.
*/
static int isVowel(const char*);
static int isConsonant(const char *z){
  int j;
  char x = *z;
  if( x==0 ) return 0;
  assert( x>='a' && x<='z' );
  j = cType[x-'a'];
  if( j<2 ) return j;
  return z[1]==0 || isVowel(z + 1);
}
static int isVowel(const char *z){
  int j;
  char x = *z;
  if( x==0 ) return 0;
  assert( x>='a' && x<='z' );
  j = cType[x-'a'];
  if( j<2 ) return 1-j;
  return isConsonant(z + 1);
}

/*
** Let any sequence of one or more vowels be represented by V and let
** C be sequence of one or more consonants.  Then every word can be
** represented as:
**
**           [C] (VC){m} [V]
**
** In prose:  A word is an optional consonant followed by zero or
** vowel-consonant pairs followed by an optional vowel.  "m" is the
** number of vowel consonant pairs.  This routine computes the value
** of m for the first i bytes of a word.
**
** Return true if the m-value for z is 1 or more.  In other words,
** return true if z contains at least one vowel that is followed
** by a consonant.
**
** In this routine z[] is in reverse order.  So we are really looking
** for an instance of of a consonant followed by a vowel.
*/
static int m_gt_0(const char *z){
  while( isVowel(z) ){ z++; }
  if( *z==0 ) return 0;
  while( isConsonant(z) ){ z++; }
  return *z!=0;
}

/* Like mgt0 above except we are looking for a value of m which is
** exactly 1
*/
static int m_eq_1(const char *z){
  while( isVowel(z) ){ z++; }
  if( *z==0 ) return 0;
  while( isConsonant(z) ){ z++; }
  if( *z==0 ) return 0;
  while( isVowel(z) ){ z++; }
  if( *z==0 ) return 1;
  while( isConsonant(z) ){ z++; }
  return *z==0;
}

/* Like mgt0 above except we are looking for a value of m>1 instead
** or m>0
*/
static int m_gt_1(const char *z){
  while( isVowel(z) ){ z++; }
  if( *z==0 ) return 0;
  while( isConsonant(z) ){ z++; }
  if( *z==0 ) return 0;
  while( isVowel(z) ){ z++; }
  if( *z==0 ) return 0;
  while( isConsonant(z) ){ z++; }
  return *z!=0;
}

/*
** Return TRUE if there is a vowel anywhere within z[0..n-1]
*/
static int hasVowel(const char *z){
  while( isConsonant(z) ){ z++; }
  return *z!=0;
}

/*
** Return TRUE if the word ends in a double consonant.
**
** The text is reversed here. So we are really looking at
** the first two characters of z[].
*/
static int doubleConsonant(const char *z){
  return isConsonant(z) && z[0]==z[1];
}

/*
** Return TRUE if the word ends with three letters which
** are consonant-vowel-consonent and where the final consonant
** is not 'w', 'x', or 'y'.
**
** The word is reversed here.  So we are really checking the
** first three letters and the first one cannot be in [wxy].
*/
static int star_oh(const char *z){
  return
    isConsonant(z) &&
    z[0]!='w' && z[0]!='x' && z[0]!='y' &&
    isVowel(z+1) &&
    isConsonant(z+2);
}

/*
** If the word ends with zFrom and xCond() is true for the stem
** of the word that preceeds the zFrom ending, then change the 
** ending to zTo.
**
** The input word *pz and zFrom are both in reverse order.  zTo
** is in normal order. 
**
** Return TRUE if zFrom matches.  Return FALSE if zFrom does not
** match.  Not that TRUE is returned even if xCond() fails and
** no substitution occurs.
*/
static int stem(
  char **pz,             /* The word being stemmed (Reversed) */
  const char *zFrom,     /* If the ending matches this... (Reversed) */
  const char *zTo,       /* ... change the ending to this (not reversed) */
  int (*xCond)(const char*)   /* Condition that must be true */
){
  char *z = *pz;
  while( *zFrom && *zFrom==*z ){ z++; zFrom++; }
  if( *zFrom!=0 ) return 0;
  if( xCond && !xCond(z) ) return 1;
  while( *zTo ){
    *(--z) = *(zTo++);
  }
  *pz = z;
  return 1;
}

/*
** This is the fallback stemmer used when the porter stemmer is
** inappropriate.  The input word is copied into the output with
** US-ASCII case folding.  If the input word is too long (more
** than 20 bytes if it contains no digits or more than 6 bytes if
** it contains digits) then word is truncated to 20 or 6 bytes
** by taking 10 or 3 bytes from the beginning and end.
*/
static void copy_stemmer(const char *zIn, int nIn, char *zOut, int *pnOut){
  int i, mx, j;
  int hasDigit = 0;
  for(i=0; i<nIn; i++){
    char c = zIn[i];
    if( c>='A' && c<='Z' ){
      zOut[i] = c - 'A' + 'a';
    }else{
      if( c>='0' && c<='9' ) hasDigit = 1;
      zOut[i] = c;
    }
  }
  mx = hasDigit ? 3 : 10;
  if( nIn>mx*2 ){
    for(j=mx, i=nIn-mx; i<nIn; i++, j++){
      zOut[j] = zOut[i];
    }
    i = j;
  }
  zOut[i] = 0;
  *pnOut = i;
}


/*
** Stem the input word zIn[0..nIn-1].  Store the output in zOut.
** zOut is at least big enough to hold nIn bytes.  Write the actual
** size of the output word (exclusive of the '\0' terminator) into *pnOut.
**
** Any upper-case characters in the US-ASCII character set ([A-Z])
** are converted to lower case.  Upper-case UTF characters are
** unchanged.
**
** Words that are longer than about 20 bytes are stemmed by retaining
** a few bytes from the beginning and the end of the word.  If the
** word contains digits, 3 bytes are taken from the beginning and
** 3 bytes from the end.  For long words without digits, 10 bytes
** are taken from each end.  US-ASCII case folding still applies.
** 
** If the input word contains not digits but does characters not 
** in [a-zA-Z] then no stemming is attempted and this routine just 
** copies the input into the input into the output with US-ASCII
** case folding.
**
** Stemming never increases the length of the word.  So there is
** no chance of overflowing the zOut buffer.
*/
static void porter_stemmer(const char *zIn, int nIn, char *zOut, int *pnOut){
  int i, j;
  char zReverse[28];
  char *z, *z2;
  if( nIn<3 || nIn>=(int)sizeof(zReverse)-7 ){
    /* The word is too big or too small for the porter stemmer.
    ** Fallback to the copy stemmer */
    copy_stemmer(zIn, nIn, zOut, pnOut);
    return;
  }
  for(i=0, j=sizeof(zReverse)-6; i<nIn; i++, j--){
    char c = zIn[i];
    if( c>='A' && c<='Z' ){
      zReverse[j] = c + 'a' - 'A';
    }else if( c>='a' && c<='z' ){
      zReverse[j] = c;
    }else{
      /* The use of a character not in [a-zA-Z] means that we fallback
      ** to the copy stemmer */
      copy_stemmer(zIn, nIn, zOut, pnOut);
      return;
    }
  }
  memset(&zReverse[sizeof(zReverse)-5], 0, 5);
  z = &zReverse[j+1];


  /* Step 1a */
  if( z[0]=='s' ){
    if(
     !stem(&z, "sess", "ss", 0) &&
     !stem(&z, "sei", "i", 0)  &&
     !stem(&z, "ss", "ss", 0)
    ){
      z++;
    }
  }

  /* Step 1b */  
  z2 = z;
  if( stem(&z, "dee", "ee", m_gt_0) ){
    /* Do nothing.  The work was all in the test */
  }else if( 
     (stem(&z, "gni", "", hasVowel) || stem(&z, "de", "", hasVowel))
      && z!=z2
  ){
     if( stem(&z, "ta", "ate", 0) ||
         stem(&z, "lb", "ble", 0) ||
         stem(&z, "zi", "ize", 0) ){
       /* Do nothing.  The work was all in the test */
     }else if( doubleConsonant(z) && (*z!='l' && *z!='s' && *z!='z') ){
       z++;
     }else if( m_eq_1(z) && star_oh(z) ){
       *(--z) = 'e';
     }
  }

  /* Step 1c */
  if( z[0]=='y' && hasVowel(z+1) ){
    z[0] = 'i';
  }

  /* Step 2 */
  switch( z[1] ){
   case 'a':
     if( !stem(&z, "lanoita", "ate", m_gt_0) ){
       stem(&z, "lanoit", "tion", m_gt_0);
     }
     break;
   case 'c':
     if( !stem(&z, "icne", "ence", m_gt_0) ){
       stem(&z, "icna", "ance", m_gt_0);
     }
     break;
   case 'e':
     stem(&z, "rezi", "ize", m_gt_0);
     break;
   case 'g':
     stem(&z, "igol", "log", m_gt_0);
     break;
   case 'l':
     if( !stem(&z, "ilb", "ble", m_gt_0) 
      && !stem(&z, "illa", "al", m_gt_0)
      && !stem(&z, "iltne", "ent", m_gt_0)
      && !stem(&z, "ile", "e", m_gt_0)
     ){
       stem(&z, "ilsuo", "ous", m_gt_0);
     }
     break;
   case 'o':
     if( !stem(&z, "noitazi", "ize", m_gt_0)
      && !stem(&z, "noita", "ate", m_gt_0)
     ){
       stem(&z, "rota", "ate", m_gt_0);
     }
     break;
   case 's':
     if( !stem(&z, "msila", "al", m_gt_0)
      && !stem(&z, "ssenevi", "ive", m_gt_0)
      && !stem(&z, "ssenluf", "ful", m_gt_0)
     ){
       stem(&z, "ssensuo", "ous", m_gt_0);
     }
     break;
   case 't':
     if( !stem(&z, "itila", "al", m_gt_0)
      && !stem(&z, "itivi", "ive", m_gt_0)
     ){
       stem(&z, "itilib", "ble", m_gt_0);
     }
     break;
  }

  /* Step 3 */
  switch( z[0] ){
   case 'e':
     if( !stem(&z, "etaci", "ic", m_gt_0)
      && !stem(&z, "evita", "", m_gt_0)
     ){
       stem(&z, "ezila", "al", m_gt_0);
     }
     break;
   case 'i':
     stem(&z, "itici", "ic", m_gt_0);
     break;
   case 'l':
     if( !stem(&z, "laci", "ic", m_gt_0) ){
       stem(&z, "luf", "", m_gt_0);
     }
     break;
   case 's':
     stem(&z, "ssen", "", m_gt_0);
     break;
  }

  /* Step 4 */
  switch( z[1] ){
   case 'a':
     if( z[0]=='l' && m_gt_1(z+2) ){
       z += 2;
     }
     break;
   case 'c':
     if( z[0]=='e' && z[2]=='n' && (z[3]=='a' || z[3]=='e')  && m_gt_1(z+4)  ){
       z += 4;
     }
     break;
   case 'e':
     if( z[0]=='r' && m_gt_1(z+2) ){
       z += 2;
     }
     break;
   case 'i':
     if( z[0]=='c' && m_gt_1(z+2) ){
       z += 2;
     }
     break;
   case 'l':
     if( z[0]=='e' && z[2]=='b' && (z[3]=='a' || z[3]=='i') && m_gt_1(z+4) ){
       z += 4;
     }
     break;
   case 'n':
     if( z[0]=='t' ){
       if( z[2]=='a' ){
         if( m_gt_1(z+3) ){
           z += 3;
         }
       }else if( z[2]=='e' ){
         if( !stem(&z, "tneme", "", m_gt_1)
          && !stem(&z, "tnem", "", m_gt_1)
         ){
           stem(&z, "tne", "", m_gt_1);
         }
       }
     }
     break;
   case 'o':
     if( z[0]=='u' ){
       if( m_gt_1(z+2) ){
         z += 2;
       }
     }else if( z[3]=='s' || z[3]=='t' ){
       stem(&z, "noi", "", m_gt_1);
     }
     break;
   case 's':
     if( z[0]=='m' && z[2]=='i' && m_gt_1(z+3) ){
       z += 3;
     }
     break;
   case 't':
     if( !stem(&z, "eta", "", m_gt_1) ){
       stem(&z, "iti", "", m_gt_1);
     }
     break;
   case 'u':
     if( z[0]=='s' && z[2]=='o' && m_gt_1(z+3) ){
       z += 3;
     }
     break;
   case 'v':
   case 'z':
     if( z[0]=='e' && z[2]=='i' && m_gt_1(z+3) ){
       z += 3;
     }
     break;
  }

  /* Step 5a */
  if( z[0]=='e' ){
    if( m_gt_1(z+1) ){
      z++;
    }else if( m_eq_1(z+1) && !star_oh(z+1) ){
      z++;
    }
  }

  /* Step 5b */
  if( m_gt_1(z) && z[0]=='l' && z[1]=='l' ){
    z++;
  }

  /* z[] is now the stemmed word in reverse order.  Flip it back
  ** around into forward order and return.
  */
  *pnOut = i = (int)strlen(z);
  zOut[i] = 0;
  while( *z ){
    zOut[--i] = *(z++);
  }
}

/*
** Characters that can be part of a token.  We assume any character
** whose value is greater than 0x80 (any UTF character) can be
** part of a token.  In other words, delimiters all must have
** values of 0x7f or lower.
*/
static const char porterIdChar[] = {
/* x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 3x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 4x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,  /* 5x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 6x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,  /* 7x */
};
#define isDelim(C) (((ch=C)&0x80)==0 && (ch<0x30 || !porterIdChar[ch-0x30]))

/*
** Extract the next token from a tokenization cursor.  The cursor must
** have been opened by a prior call to porterOpen().
*/
static int porterNext(
  sqlite3_tokenizer_cursor *pCursor,  /* Cursor returned by porterOpen */
  const char **pzToken,               /* OUT: *pzToken is the token text */
  int *pnBytes,                       /* OUT: Number of bytes in token */
  int *piStartOffset,                 /* OUT: Starting offset of token */
  int *piEndOffset,                   /* OUT: Ending offset of token */
  int *piPosition                     /* OUT: Position integer of token */
){
  porter_tokenizer_cursor *c = (porter_tokenizer_cursor *) pCursor;
  const char *z = c->zInput;

  while( c->iOffset<c->nInput ){
    int iStartOffset, ch;

    /* Scan past delimiter characters */
    while( c->iOffset<c->nInput && isDelim(z[c->iOffset]) ){
      c->iOffset++;
    }

    /* Count non-delimiter characters. */
    iStartOffset = c->iOffset;
    while( c->iOffset<c->nInput && !isDelim(z[c->iOffset]) ){
      c->iOffset++;
    }

    if( c->iOffset>iStartOffset ){
      int n = c->iOffset-iStartOffset;
      if( n>c->nAllocated ){
        char *pNew;
        c->nAllocated = n+20;
        pNew = sqlite3_realloc(c->zToken, c->nAllocated);
        if( !pNew ) return SQLITE_NOMEM;
        c->zToken = pNew;
      }
      porter_stemmer(&z[iStartOffset], n, c->zToken, pnBytes);
      *pzToken = c->zToken;
      *piStartOffset = iStartOffset;
      *piEndOffset = c->iOffset;
      *piPosition = c->iToken++;
      return SQLITE_OK;
    }
  }
  return SQLITE_DONE;
}

/*
** The set of routines that implement the porter-stemmer tokenizer
*/
static const sqlite3_tokenizer_module porterTokenizerModule = {
  0,
  porterCreate,
  porterDestroy,
  porterOpen,
  porterClose,
  porterNext,
  0
};

/*
** Allocate a new porter tokenizer.  Return a pointer to the new
** tokenizer in *ppModule
*/
SQLITE_PRIVATE void sqlite3Fts3PorterTokenizerModule(
  sqlite3_tokenizer_module const**ppModule
){
  *ppModule = &porterTokenizerModule;
}

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */

/************** End of fts3_porter.c *****************************************/
/************** Begin file fts3_tokenizer.c **********************************/
/*
** 2007 June 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This is part of an SQLite module implementing full-text search.
** This particular file implements the generic tokenizer interface.
*/

/*
** The code in this file is only compiled if:
**
**     * The FTS3 module is being built as an extension
**       (in which case SQLITE_CORE is not defined), or
**
**     * The FTS3 module is being built into the core of
**       SQLite (in which case SQLITE_ENABLE_FTS3 is defined).
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <assert.h> */
/* #include <string.h> */

/*
** Implementation of the SQL scalar function for accessing the underlying 
** hash table. This function may be called as follows:
**
**   SELECT <function-name>(<key-name>);
**   SELECT <function-name>(<key-name>, <pointer>);
**
** where <function-name> is the name passed as the second argument
** to the sqlite3Fts3InitHashTable() function (e.g. 'fts3_tokenizer').
**
** If the <pointer> argument is specified, it must be a blob value
** containing a pointer to be stored as the hash data corresponding
** to the string <key-name>. If <pointer> is not specified, then
** the string <key-name> must already exist in the has table. Otherwise,
** an error is returned.
**
** Whether or not the <pointer> argument is specified, the value returned
** is a blob containing the pointer stored as the hash data corresponding
** to string <key-name> (after the hash-table is updated, if applicable).
*/
static void scalarFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  Fts3Hash *pHash;
  void *pPtr = 0;
  const unsigned char *zName;
  int nName;

  assert( argc==1 || argc==2 );

  pHash = (Fts3Hash *)sqlite3_user_data(context);

  zName = sqlite3_value_text(argv[0]);
  nName = sqlite3_value_bytes(argv[0])+1;

  if( argc==2 ){
    void *pOld;
    int n = sqlite3_value_bytes(argv[1]);
    if( n!=sizeof(pPtr) ){
      sqlite3_result_error(context, "argument type mismatch", -1);
      return;
    }
    pPtr = *(void **)sqlite3_value_blob(argv[1]);
    pOld = sqlite3Fts3HashInsert(pHash, (void *)zName, nName, pPtr);
    if( pOld==pPtr ){
      sqlite3_result_error(context, "out of memory", -1);
      return;
    }
  }else{
    pPtr = sqlite3Fts3HashFind(pHash, zName, nName);
    if( !pPtr ){
      char *zErr = sqlite3_mprintf("unknown tokenizer: %s", zName);
      sqlite3_result_error(context, zErr, -1);
      sqlite3_free(zErr);
      return;
    }
  }

  sqlite3_result_blob(context, (void *)&pPtr, sizeof(pPtr), SQLITE_TRANSIENT);
}

SQLITE_PRIVATE int sqlite3Fts3IsIdChar(char c){
  static const char isFtsIdChar[] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 0x */
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 1x */
      0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 2x */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 3x */
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 4x */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,  /* 5x */
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  /* 6x */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,  /* 7x */
  };
  return (c&0x80 || isFtsIdChar[(int)(c)]);
}

SQLITE_PRIVATE const char *sqlite3Fts3NextToken(const char *zStr, int *pn){
  const char *z1;
  const char *z2 = 0;

  /* Find the start of the next token. */
  z1 = zStr;
  while( z2==0 ){
    char c = *z1;
    switch( c ){
      case '\0': return 0;        /* No more tokens here */
      case '\'':
      case '"':
      case '`': {
        z2 = z1;
        while( *++z2 && (*z2!=c || *++z2==c) );
        break;
      }
      case '[':
        z2 = &z1[1];
        while( *z2 && z2[0]!=']' ) z2++;
        if( *z2 ) z2++;
        break;

      default:
        if( sqlite3Fts3IsIdChar(*z1) ){
          z2 = &z1[1];
          while( sqlite3Fts3IsIdChar(*z2) ) z2++;
        }else{
          z1++;
        }
    }
  }

  *pn = (int)(z2-z1);
  return z1;
}

SQLITE_PRIVATE int sqlite3Fts3InitTokenizer(
  Fts3Hash *pHash,                /* Tokenizer hash table */
  const char *zArg,               /* Tokenizer name */
  sqlite3_tokenizer **ppTok,      /* OUT: Tokenizer (if applicable) */
  char **pzErr                    /* OUT: Set to malloced error message */
){
  int rc;
  char *z = (char *)zArg;
  int n = 0;
  char *zCopy;
  char *zEnd;                     /* Pointer to nul-term of zCopy */
  sqlite3_tokenizer_module *m;

  zCopy = sqlite3_mprintf("%s", zArg);
  if( !zCopy ) return SQLITE_NOMEM;
  zEnd = &zCopy[strlen(zCopy)];

  z = (char *)sqlite3Fts3NextToken(zCopy, &n);
  z[n] = '\0';
  sqlite3Fts3Dequote(z);

  m = (sqlite3_tokenizer_module *)sqlite3Fts3HashFind(pHash,z,(int)strlen(z)+1);
  if( !m ){
    *pzErr = sqlite3_mprintf("unknown tokenizer: %s", z);
    rc = SQLITE_ERROR;
  }else{
    char const **aArg = 0;
    int iArg = 0;
    z = &z[n+1];
    while( z<zEnd && (NULL!=(z = (char *)sqlite3Fts3NextToken(z, &n))) ){
      int nNew = sizeof(char *)*(iArg+1);
      char const **aNew = (const char **)sqlite3_realloc((void *)aArg, nNew);
      if( !aNew ){
        sqlite3_free(zCopy);
        sqlite3_free((void *)aArg);
        return SQLITE_NOMEM;
      }
      aArg = aNew;
      aArg[iArg++] = z;
      z[n] = '\0';
      sqlite3Fts3Dequote(z);
      z = &z[n+1];
    }
    rc = m->xCreate(iArg, aArg, ppTok);
    assert( rc!=SQLITE_OK || *ppTok );
    if( rc!=SQLITE_OK ){
      *pzErr = sqlite3_mprintf("unknown tokenizer");
    }else{
      (*ppTok)->pModule = m; 
    }
    sqlite3_free((void *)aArg);
  }

  sqlite3_free(zCopy);
  return rc;
}


#ifdef SQLITE_TEST

#include <tcl.h>
/* #include <string.h> */

/*
** Implementation of a special SQL scalar function for testing tokenizers 
** designed to be used in concert with the Tcl testing framework. This
** function must be called with two or more arguments:
**
**   SELECT <function-name>(<key-name>, ..., <input-string>);
**
** where <function-name> is the name passed as the second argument
** to the sqlite3Fts3InitHashTable() function (e.g. 'fts3_tokenizer')
** concatenated with the string '_test' (e.g. 'fts3_tokenizer_test').
**
** The return value is a string that may be interpreted as a Tcl
** list. For each token in the <input-string>, three elements are
** added to the returned list. The first is the token position, the 
** second is the token text (folded, stemmed, etc.) and the third is the
** substring of <input-string> associated with the token. For example, 
** using the built-in "simple" tokenizer:
**
**   SELECT fts_tokenizer_test('simple', 'I don't see how');
**
** will return the string:
**
**   "{0 i I 1 dont don't 2 see see 3 how how}"
**   
*/
static void testFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  Fts3Hash *pHash;
  sqlite3_tokenizer_module *p;
  sqlite3_tokenizer *pTokenizer = 0;
  sqlite3_tokenizer_cursor *pCsr = 0;

  const char *zErr = 0;

  const char *zName;
  int nName;
  const char *zInput;
  int nInput;

  const char *azArg[64];

  const char *zToken;
  int nToken = 0;
  int iStart = 0;
  int iEnd = 0;
  int iPos = 0;
  int i;

  Tcl_Obj *pRet;

  if( argc<2 ){
    sqlite3_result_error(context, "insufficient arguments", -1);
    return;
  }

  nName = sqlite3_value_bytes(argv[0]);
  zName = (const char *)sqlite3_value_text(argv[0]);
  nInput = sqlite3_value_bytes(argv[argc-1]);
  zInput = (const char *)sqlite3_value_text(argv[argc-1]);

  pHash = (Fts3Hash *)sqlite3_user_data(context);
  p = (sqlite3_tokenizer_module *)sqlite3Fts3HashFind(pHash, zName, nName+1);

  if( !p ){
    char *zErr = sqlite3_mprintf("unknown tokenizer: %s", zName);
    sqlite3_result_error(context, zErr, -1);
    sqlite3_free(zErr);
    return;
  }

  pRet = Tcl_NewObj();
  Tcl_IncrRefCount(pRet);

  for(i=1; i<argc-1; i++){
    azArg[i-1] = (const char *)sqlite3_value_text(argv[i]);
  }

  if( SQLITE_OK!=p->xCreate(argc-2, azArg, &pTokenizer) ){
    zErr = "error in xCreate()";
    goto finish;
  }
  pTokenizer->pModule = p;
  if( sqlite3Fts3OpenTokenizer(pTokenizer, 0, zInput, nInput, &pCsr) ){
    zErr = "error in xOpen()";
    goto finish;
  }

  while( SQLITE_OK==p->xNext(pCsr, &zToken, &nToken, &iStart, &iEnd, &iPos) ){
    Tcl_ListObjAppendElement(0, pRet, Tcl_NewIntObj(iPos));
    Tcl_ListObjAppendElement(0, pRet, Tcl_NewStringObj(zToken, nToken));
    zToken = &zInput[iStart];
    nToken = iEnd-iStart;
    Tcl_ListObjAppendElement(0, pRet, Tcl_NewStringObj(zToken, nToken));
  }

  if( SQLITE_OK!=p->xClose(pCsr) ){
    zErr = "error in xClose()";
    goto finish;
  }
  if( SQLITE_OK!=p->xDestroy(pTokenizer) ){
    zErr = "error in xDestroy()";
    goto finish;
  }

finish:
  if( zErr ){
    sqlite3_result_error(context, zErr, -1);
  }else{
    sqlite3_result_text(context, Tcl_GetString(pRet), -1, SQLITE_TRANSIENT);
  }
  Tcl_DecrRefCount(pRet);
}

static
int registerTokenizer(
  sqlite3 *db, 
  char *zName, 
  const sqlite3_tokenizer_module *p
){
  int rc;
  sqlite3_stmt *pStmt;
  const char zSql[] = "SELECT fts3_tokenizer(?, ?)";

  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  sqlite3_bind_text(pStmt, 1, zName, -1, SQLITE_STATIC);
  sqlite3_bind_blob(pStmt, 2, &p, sizeof(p), SQLITE_STATIC);
  sqlite3_step(pStmt);

  return sqlite3_finalize(pStmt);
}

static
int queryTokenizer(
  sqlite3 *db, 
  char *zName,  
  const sqlite3_tokenizer_module **pp
){
  int rc;
  sqlite3_stmt *pStmt;
  const char zSql[] = "SELECT fts3_tokenizer(?)";

  *pp = 0;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  sqlite3_bind_text(pStmt, 1, zName, -1, SQLITE_STATIC);
  if( SQLITE_ROW==sqlite3_step(pStmt) ){
    if( sqlite3_column_type(pStmt, 0)==SQLITE_BLOB ){
      memcpy((void *)pp, sqlite3_column_blob(pStmt, 0), sizeof(*pp));
    }
  }

  return sqlite3_finalize(pStmt);
}

SQLITE_PRIVATE void sqlite3Fts3SimpleTokenizerModule(sqlite3_tokenizer_module const**ppModule);

/*
** Implementation of the scalar function fts3_tokenizer_internal_test().
** This function is used for testing only, it is not included in the
** build unless SQLITE_TEST is defined.
**
** The purpose of this is to test that the fts3_tokenizer() function
** can be used as designed by the C-code in the queryTokenizer and
** registerTokenizer() functions above. These two functions are repeated
** in the README.tokenizer file as an example, so it is important to
** test them.
**
** To run the tests, evaluate the fts3_tokenizer_internal_test() scalar
** function with no arguments. An assert() will fail if a problem is
** detected. i.e.:
**
**     SELECT fts3_tokenizer_internal_test();
**
*/
static void intTestFunc(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  int rc;
  const sqlite3_tokenizer_module *p1;
  const sqlite3_tokenizer_module *p2;
  sqlite3 *db = (sqlite3 *)sqlite3_user_data(context);

  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(argv);

  /* Test the query function */
  sqlite3Fts3SimpleTokenizerModule(&p1);
  rc = queryTokenizer(db, "simple", &p2);
  assert( rc==SQLITE_OK );
  assert( p1==p2 );
  rc = queryTokenizer(db, "nosuchtokenizer", &p2);
  assert( rc==SQLITE_ERROR );
  assert( p2==0 );
  assert( 0==strcmp(sqlite3_errmsg(db), "unknown tokenizer: nosuchtokenizer") );

  /* Test the storage function */
  rc = registerTokenizer(db, "nosuchtokenizer", p1);
  assert( rc==SQLITE_OK );
  rc = queryTokenizer(db, "nosuchtokenizer", &p2);
  assert( rc==SQLITE_OK );
  assert( p2==p1 );

  sqlite3_result_text(context, "ok", -1, SQLITE_STATIC);
}

#endif

/*
** Set up SQL objects in database db used to access the contents of
** the hash table pointed to by argument pHash. The hash table must
** been initialized to use string keys, and to take a private copy 
** of the key when a value is inserted. i.e. by a call similar to:
**
**    sqlite3Fts3HashInit(pHash, FTS3_HASH_STRING, 1);
**
** This function adds a scalar function (see header comment above
** scalarFunc() in this file for details) and, if ENABLE_TABLE is
** defined at compilation time, a temporary virtual table (see header 
** comment above struct HashTableVtab) to the database schema. Both 
** provide read/write access to the contents of *pHash.
**
** The third argument to this function, zName, is used as the name
** of both the scalar and, if created, the virtual table.
*/
SQLITE_PRIVATE int sqlite3Fts3InitHashTable(
  sqlite3 *db, 
  Fts3Hash *pHash, 
  const char *zName
){
  int rc = SQLITE_OK;
  void *p = (void *)pHash;
  const int any = SQLITE_ANY;

#ifdef SQLITE_TEST
  char *zTest = 0;
  char *zTest2 = 0;
  void *pdb = (void *)db;
  zTest = sqlite3_mprintf("%s_test", zName);
  zTest2 = sqlite3_mprintf("%s_internal_test", zName);
  if( !zTest || !zTest2 ){
    rc = SQLITE_NOMEM;
  }
#endif

  if( SQLITE_OK==rc ){
    rc = sqlite3_create_function(db, zName, 1, any, p, scalarFunc, 0, 0);
  }
  if( SQLITE_OK==rc ){
    rc = sqlite3_create_function(db, zName, 2, any, p, scalarFunc, 0, 0);
  }
#ifdef SQLITE_TEST
  if( SQLITE_OK==rc ){
    rc = sqlite3_create_function(db, zTest, -1, any, p, testFunc, 0, 0);
  }
  if( SQLITE_OK==rc ){
    rc = sqlite3_create_function(db, zTest2, 0, any, pdb, intTestFunc, 0, 0);
  }
#endif

#ifdef SQLITE_TEST
  sqlite3_free(zTest);
  sqlite3_free(zTest2);
#endif

  return rc;
}

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */

/************** End of fts3_tokenizer.c **************************************/
/************** Begin file fts3_tokenizer1.c *********************************/
/*
** 2006 Oct 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** Implementation of the "simple" full-text-search tokenizer.
*/

/*
** The code in this file is only compiled if:
**
**     * The FTS3 module is being built as an extension
**       (in which case SQLITE_CORE is not defined), or
**
**     * The FTS3 module is being built into the core of
**       SQLite (in which case SQLITE_ENABLE_FTS3 is defined).
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <assert.h> */
/* #include <stdlib.h> */
/* #include <stdio.h> */
/* #include <string.h> */


typedef struct simple_tokenizer {
  sqlite3_tokenizer base;
  char delim[128];             /* flag ASCII delimiters */
} simple_tokenizer;

typedef struct simple_tokenizer_cursor {
  sqlite3_tokenizer_cursor base;
  const char *pInput;          /* input we are tokenizing */
  int nBytes;                  /* size of the input */
  int iOffset;                 /* current position in pInput */
  int iToken;                  /* index of next token to be returned */
  char *pToken;                /* storage for current token */
  int nTokenAllocated;         /* space allocated to zToken buffer */
} simple_tokenizer_cursor;


static int simpleDelim(simple_tokenizer *t, unsigned char c){
  return c<0x80 && t->delim[c];
}
static int fts3_isalnum(int x){
  return (x>='0' && x<='9') || (x>='A' && x<='Z') || (x>='a' && x<='z');
}

/*
** Create a new tokenizer instance.
*/
static int simpleCreate(
  int argc, const char * const *argv,
  sqlite3_tokenizer **ppTokenizer
){
  simple_tokenizer *t;

  t = (simple_tokenizer *) sqlite3_malloc(sizeof(*t));
  if( t==NULL ) return SQLITE_NOMEM;
  memset(t, 0, sizeof(*t));

  /* TODO(shess) Delimiters need to remain the same from run to run,
  ** else we need to reindex.  One solution would be a meta-table to
  ** track such information in the database, then we'd only want this
  ** information on the initial create.
  */
  if( argc>1 ){
    int i, n = (int)strlen(argv[1]);
    for(i=0; i<n; i++){
      unsigned char ch = argv[1][i];
      /* We explicitly don't support UTF-8 delimiters for now. */
      if( ch>=0x80 ){
        sqlite3_free(t);
        return SQLITE_ERROR;
      }
      t->delim[ch] = 1;
    }
  } else {
    /* Mark non-alphanumeric ASCII characters as delimiters */
    int i;
    for(i=1; i<0x80; i++){
      t->delim[i] = !fts3_isalnum(i) ? -1 : 0;
    }
  }

  *ppTokenizer = &t->base;
  return SQLITE_OK;
}

/*
** Destroy a tokenizer
*/
static int simpleDestroy(sqlite3_tokenizer *pTokenizer){
  sqlite3_free(pTokenizer);
  return SQLITE_OK;
}

/*
** Prepare to begin tokenizing a particular string.  The input
** string to be tokenized is pInput[0..nBytes-1].  A cursor
** used to incrementally tokenize this string is returned in 
** *ppCursor.
*/
static int simpleOpen(
  sqlite3_tokenizer *pTokenizer,         /* The tokenizer */
  const char *pInput, int nBytes,        /* String to be tokenized */
  sqlite3_tokenizer_cursor **ppCursor    /* OUT: Tokenization cursor */
){
  simple_tokenizer_cursor *c;

  UNUSED_PARAMETER(pTokenizer);

  c = (simple_tokenizer_cursor *) sqlite3_malloc(sizeof(*c));
  if( c==NULL ) return SQLITE_NOMEM;

  c->pInput = pInput;
  if( pInput==0 ){
    c->nBytes = 0;
  }else if( nBytes<0 ){
    c->nBytes = (int)strlen(pInput);
  }else{
    c->nBytes = nBytes;
  }
  c->iOffset = 0;                 /* start tokenizing at the beginning */
  c->iToken = 0;
  c->pToken = NULL;               /* no space allocated, yet. */
  c->nTokenAllocated = 0;

  *ppCursor = &c->base;
  return SQLITE_OK;
}

/*
** Close a tokenization cursor previously opened by a call to
** simpleOpen() above.
*/
static int simpleClose(sqlite3_tokenizer_cursor *pCursor){
  simple_tokenizer_cursor *c = (simple_tokenizer_cursor *) pCursor;
  sqlite3_free(c->pToken);
  sqlite3_free(c);
  return SQLITE_OK;
}

/*
** Extract the next token from a tokenization cursor.  The cursor must
** have been opened by a prior call to simpleOpen().
*/
static int simpleNext(
  sqlite3_tokenizer_cursor *pCursor,  /* Cursor returned by simpleOpen */
  const char **ppToken,               /* OUT: *ppToken is the token text */
  int *pnBytes,                       /* OUT: Number of bytes in token */
  int *piStartOffset,                 /* OUT: Starting offset of token */
  int *piEndOffset,                   /* OUT: Ending offset of token */
  int *piPosition                     /* OUT: Position integer of token */
){
  simple_tokenizer_cursor *c = (simple_tokenizer_cursor *) pCursor;
  simple_tokenizer *t = (simple_tokenizer *) pCursor->pTokenizer;
  unsigned char *p = (unsigned char *)c->pInput;

  while( c->iOffset<c->nBytes ){
    int iStartOffset;

    /* Scan past delimiter characters */
    while( c->iOffset<c->nBytes && simpleDelim(t, p[c->iOffset]) ){
      c->iOffset++;
    }

    /* Count non-delimiter characters. */
    iStartOffset = c->iOffset;
    while( c->iOffset<c->nBytes && !simpleDelim(t, p[c->iOffset]) ){
      c->iOffset++;
    }

    if( c->iOffset>iStartOffset ){
      int i, n = c->iOffset-iStartOffset;
      if( n>c->nTokenAllocated ){
        char *pNew;
        c->nTokenAllocated = n+20;
        pNew = sqlite3_realloc(c->pToken, c->nTokenAllocated);
        if( !pNew ) return SQLITE_NOMEM;
        c->pToken = pNew;
      }
      for(i=0; i<n; i++){
        /* TODO(shess) This needs expansion to handle UTF-8
        ** case-insensitivity.
        */
        unsigned char ch = p[iStartOffset+i];
        c->pToken[i] = (char)((ch>='A' && ch<='Z') ? ch-'A'+'a' : ch);
      }
      *ppToken = c->pToken;
      *pnBytes = n;
      *piStartOffset = iStartOffset;
      *piEndOffset = c->iOffset;
      *piPosition = c->iToken++;

      return SQLITE_OK;
    }
  }
  return SQLITE_DONE;
}

/*
** The set of routines that implement the simple tokenizer
*/
static const sqlite3_tokenizer_module simpleTokenizerModule = {
  0,
  simpleCreate,
  simpleDestroy,
  simpleOpen,
  simpleClose,
  simpleNext,
  0,
};

/*
** Allocate a new simple tokenizer.  Return a pointer to the new
** tokenizer in *ppModule
*/
SQLITE_PRIVATE void sqlite3Fts3SimpleTokenizerModule(
  sqlite3_tokenizer_module const**ppModule
){
  *ppModule = &simpleTokenizerModule;
}

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */

/************** End of fts3_tokenizer1.c *************************************/
/************** Begin file fts3_tokenize_vtab.c ******************************/
/*
** 2013 Apr 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code for the "fts3tokenize" virtual table module.
** An fts3tokenize virtual table is created as follows:
**
**   CREATE VIRTUAL TABLE <tbl> USING fts3tokenize(
**       <tokenizer-name>, <arg-1>, ...
**   );
**
** The table created has the following schema:
**
**   CREATE TABLE <tbl>(input, token, start, end, position)
**
** When queried, the query must include a WHERE clause of type:
**
**   input = <string>
**
** The virtual table module tokenizes this <string>, using the FTS3 
** tokenizer specified by the arguments to the CREATE VIRTUAL TABLE 
** statement and returns one row for each token in the result. With
** fields set as follows:
**
**   input:   Always set to a copy of <string>
**   token:   A token from the input.
**   start:   Byte offset of the token within the input <string>.
**   end:     Byte offset of the byte immediately following the end of the
**            token within the input string.
**   pos:     Token offset of token within input.
**
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <string.h> */
/* #include <assert.h> */

typedef struct Fts3tokTable Fts3tokTable;
typedef struct Fts3tokCursor Fts3tokCursor;

/*
** Virtual table structure.
*/
struct Fts3tokTable {
  sqlite3_vtab base;              /* Base class used by SQLite core */
  const sqlite3_tokenizer_module *pMod;
  sqlite3_tokenizer *pTok;
};

/*
** Virtual table cursor structure.
*/
struct Fts3tokCursor {
  sqlite3_vtab_cursor base;       /* Base class used by SQLite core */
  char *zInput;                   /* Input string */
  sqlite3_tokenizer_cursor *pCsr; /* Cursor to iterate through zInput */
  int iRowid;                     /* Current 'rowid' value */
  const char *zToken;             /* Current 'token' value */
  int nToken;                     /* Size of zToken in bytes */
  int iStart;                     /* Current 'start' value */
  int iEnd;                       /* Current 'end' value */
  int iPos;                       /* Current 'pos' value */
};

/*
** Query FTS for the tokenizer implementation named zName.
*/
static int fts3tokQueryTokenizer(
  Fts3Hash *pHash,
  const char *zName,
  const sqlite3_tokenizer_module **pp,
  char **pzErr
){
  sqlite3_tokenizer_module *p;
  int nName = (int)strlen(zName);

  p = (sqlite3_tokenizer_module *)sqlite3Fts3HashFind(pHash, zName, nName+1);
  if( !p ){
    *pzErr = sqlite3_mprintf("unknown tokenizer: %s", zName);
    return SQLITE_ERROR;
  }

  *pp = p;
  return SQLITE_OK;
}

/*
** The second argument, argv[], is an array of pointers to nul-terminated
** strings. This function makes a copy of the array and strings into a 
** single block of memory. It then dequotes any of the strings that appear
** to be quoted.
**
** If successful, output parameter *pazDequote is set to point at the
** array of dequoted strings and SQLITE_OK is returned. The caller is
** responsible for eventually calling sqlite3_free() to free the array
** in this case. Or, if an error occurs, an SQLite error code is returned.
** The final value of *pazDequote is undefined in this case.
*/
static int fts3tokDequoteArray(
  int argc,                       /* Number of elements in argv[] */
  const char * const *argv,       /* Input array */
  char ***pazDequote              /* Output array */
){
  int rc = SQLITE_OK;             /* Return code */
  if( argc==0 ){
    *pazDequote = 0;
  }else{
    int i;
    int nByte = 0;
    char **azDequote;

    for(i=0; i<argc; i++){
      nByte += (int)(strlen(argv[i]) + 1);
    }

    *pazDequote = azDequote = sqlite3_malloc(sizeof(char *)*argc + nByte);
    if( azDequote==0 ){
      rc = SQLITE_NOMEM;
    }else{
      char *pSpace = (char *)&azDequote[argc];
      for(i=0; i<argc; i++){
        int n = (int)strlen(argv[i]);
        azDequote[i] = pSpace;
        memcpy(pSpace, argv[i], n+1);
        sqlite3Fts3Dequote(pSpace);
        pSpace += (n+1);
      }
    }
  }

  return rc;
}

/*
** Schema of the tokenizer table.
*/
#define FTS3_TOK_SCHEMA "CREATE TABLE x(input, token, start, end, position)"

/*
** This function does all the work for both the xConnect and xCreate methods.
** These tables have no persistent representation of their own, so xConnect
** and xCreate are identical operations.
**
**   argv[0]: module name
**   argv[1]: database name 
**   argv[2]: table name
**   argv[3]: first argument (tokenizer name)
*/
static int fts3tokConnectMethod(
  sqlite3 *db,                    /* Database connection */
  void *pHash,                    /* Hash table of tokenizers */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVtab,          /* OUT: New sqlite3_vtab object */
  char **pzErr                    /* OUT: sqlite3_malloc'd error message */
){
  Fts3tokTable *pTab;
  const sqlite3_tokenizer_module *pMod = 0;
  sqlite3_tokenizer *pTok = 0;
  int rc;
  char **azDequote = 0;
  int nDequote;

  rc = sqlite3_declare_vtab(db, FTS3_TOK_SCHEMA);
  if( rc!=SQLITE_OK ) return rc;

  nDequote = argc-3;
  rc = fts3tokDequoteArray(nDequote, &argv[3], &azDequote);

  if( rc==SQLITE_OK ){
    const char *zModule;
    if( nDequote<1 ){
      zModule = "simple";
    }else{
      zModule = azDequote[0];
    }
    rc = fts3tokQueryTokenizer((Fts3Hash*)pHash, zModule, &pMod, pzErr);
  }

  assert( (rc==SQLITE_OK)==(pMod!=0) );
  if( rc==SQLITE_OK ){
    const char * const *azArg = (const char * const *)&azDequote[1];
    rc = pMod->xCreate((nDequote>1 ? nDequote-1 : 0), azArg, &pTok);
  }

  if( rc==SQLITE_OK ){
    pTab = (Fts3tokTable *)sqlite3_malloc(sizeof(Fts3tokTable));
    if( pTab==0 ){
      rc = SQLITE_NOMEM;
    }
  }

  if( rc==SQLITE_OK ){
    memset(pTab, 0, sizeof(Fts3tokTable));
    pTab->pMod = pMod;
    pTab->pTok = pTok;
    *ppVtab = &pTab->base;
  }else{
    if( pTok ){
      pMod->xDestroy(pTok);
    }
  }

  sqlite3_free(azDequote);
  return rc;
}

/*
** This function does the work for both the xDisconnect and xDestroy methods.
** These tables have no persistent representation of their own, so xDisconnect
** and xDestroy are identical operations.
*/
static int fts3tokDisconnectMethod(sqlite3_vtab *pVtab){
  Fts3tokTable *pTab = (Fts3tokTable *)pVtab;

  pTab->pMod->xDestroy(pTab->pTok);
  sqlite3_free(pTab);
  return SQLITE_OK;
}

/*
** xBestIndex - Analyze a WHERE and ORDER BY clause.
*/
static int fts3tokBestIndexMethod(
  sqlite3_vtab *pVTab, 
  sqlite3_index_info *pInfo
){
  int i;
  UNUSED_PARAMETER(pVTab);

  for(i=0; i<pInfo->nConstraint; i++){
    if( pInfo->aConstraint[i].usable 
     && pInfo->aConstraint[i].iColumn==0 
     && pInfo->aConstraint[i].op==SQLITE_INDEX_CONSTRAINT_EQ 
    ){
      pInfo->idxNum = 1;
      pInfo->aConstraintUsage[i].argvIndex = 1;
      pInfo->aConstraintUsage[i].omit = 1;
      pInfo->estimatedCost = 1;
      return SQLITE_OK;
    }
  }

  pInfo->idxNum = 0;
  assert( pInfo->estimatedCost>1000000.0 );

  return SQLITE_OK;
}

/*
** xOpen - Open a cursor.
*/
static int fts3tokOpenMethod(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCsr){
  Fts3tokCursor *pCsr;
  UNUSED_PARAMETER(pVTab);

  pCsr = (Fts3tokCursor *)sqlite3_malloc(sizeof(Fts3tokCursor));
  if( pCsr==0 ){
    return SQLITE_NOMEM;
  }
  memset(pCsr, 0, sizeof(Fts3tokCursor));

  *ppCsr = (sqlite3_vtab_cursor *)pCsr;
  return SQLITE_OK;
}

/*
** Reset the tokenizer cursor passed as the only argument. As if it had
** just been returned by fts3tokOpenMethod().
*/
static void fts3tokResetCursor(Fts3tokCursor *pCsr){
  if( pCsr->pCsr ){
    Fts3tokTable *pTab = (Fts3tokTable *)(pCsr->base.pVtab);
    pTab->pMod->xClose(pCsr->pCsr);
    pCsr->pCsr = 0;
  }
  sqlite3_free(pCsr->zInput);
  pCsr->zInput = 0;
  pCsr->zToken = 0;
  pCsr->nToken = 0;
  pCsr->iStart = 0;
  pCsr->iEnd = 0;
  pCsr->iPos = 0;
  pCsr->iRowid = 0;
}

/*
** xClose - Close a cursor.
*/
static int fts3tokCloseMethod(sqlite3_vtab_cursor *pCursor){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;

  fts3tokResetCursor(pCsr);
  sqlite3_free(pCsr);
  return SQLITE_OK;
}

/*
** xNext - Advance the cursor to the next row, if any.
*/
static int fts3tokNextMethod(sqlite3_vtab_cursor *pCursor){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;
  Fts3tokTable *pTab = (Fts3tokTable *)(pCursor->pVtab);
  int rc;                         /* Return code */

  pCsr->iRowid++;
  rc = pTab->pMod->xNext(pCsr->pCsr,
      &pCsr->zToken, &pCsr->nToken,
      &pCsr->iStart, &pCsr->iEnd, &pCsr->iPos
  );

  if( rc!=SQLITE_OK ){
    fts3tokResetCursor(pCsr);
    if( rc==SQLITE_DONE ) rc = SQLITE_OK;
  }

  return rc;
}

/*
** xFilter - Initialize a cursor to point at the start of its data.
*/
static int fts3tokFilterMethod(
  sqlite3_vtab_cursor *pCursor,   /* The cursor used for this query */
  int idxNum,                     /* Strategy index */
  const char *idxStr,             /* Unused */
  int nVal,                       /* Number of elements in apVal */
  sqlite3_value **apVal           /* Arguments for the indexing scheme */
){
  int rc = SQLITE_ERROR;
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;
  Fts3tokTable *pTab = (Fts3tokTable *)(pCursor->pVtab);
  UNUSED_PARAMETER(idxStr);
  UNUSED_PARAMETER(nVal);

  fts3tokResetCursor(pCsr);
  if( idxNum==1 ){
    const char *zByte = (const char *)sqlite3_value_text(apVal[0]);
    int nByte = sqlite3_value_bytes(apVal[0]);
    pCsr->zInput = sqlite3_malloc(nByte+1);
    if( pCsr->zInput==0 ){
      rc = SQLITE_NOMEM;
    }else{
      memcpy(pCsr->zInput, zByte, nByte);
      pCsr->zInput[nByte] = 0;
      rc = pTab->pMod->xOpen(pTab->pTok, pCsr->zInput, nByte, &pCsr->pCsr);
      if( rc==SQLITE_OK ){
        pCsr->pCsr->pTokenizer = pTab->pTok;
      }
    }
  }

  if( rc!=SQLITE_OK ) return rc;
  return fts3tokNextMethod(pCursor);
}

/*
** xEof - Return true if the cursor is at EOF, or false otherwise.
*/
static int fts3tokEofMethod(sqlite3_vtab_cursor *pCursor){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;
  return (pCsr->zToken==0);
}

/*
** xColumn - Return a column value.
*/
static int fts3tokColumnMethod(
  sqlite3_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite3_context *pCtx,          /* Context for sqlite3_result_xxx() calls */
  int iCol                        /* Index of column to read value from */
){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;

  /* CREATE TABLE x(input, token, start, end, position) */
  switch( iCol ){
    case 0:
      sqlite3_result_text(pCtx, pCsr->zInput, -1, SQLITE_TRANSIENT);
      break;
    case 1:
      sqlite3_result_text(pCtx, pCsr->zToken, pCsr->nToken, SQLITE_TRANSIENT);
      break;
    case 2:
      sqlite3_result_int(pCtx, pCsr->iStart);
      break;
    case 3:
      sqlite3_result_int(pCtx, pCsr->iEnd);
      break;
    default:
      assert( iCol==4 );
      sqlite3_result_int(pCtx, pCsr->iPos);
      break;
  }
  return SQLITE_OK;
}

/*
** xRowid - Return the current rowid for the cursor.
*/
static int fts3tokRowidMethod(
  sqlite3_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite_int64 *pRowid            /* OUT: Rowid value */
){
  Fts3tokCursor *pCsr = (Fts3tokCursor *)pCursor;
  *pRowid = (sqlite3_int64)pCsr->iRowid;
  return SQLITE_OK;
}

/*
** Register the fts3tok module with database connection db. Return SQLITE_OK
** if successful or an error code if sqlite3_create_module() fails.
*/
SQLITE_PRIVATE int sqlite3Fts3InitTok(sqlite3 *db, Fts3Hash *pHash){
  static const sqlite3_module fts3tok_module = {
     0,                           /* iVersion      */
     fts3tokConnectMethod,        /* xCreate       */
     fts3tokConnectMethod,        /* xConnect      */
     fts3tokBestIndexMethod,      /* xBestIndex    */
     fts3tokDisconnectMethod,     /* xDisconnect   */
     fts3tokDisconnectMethod,     /* xDestroy      */
     fts3tokOpenMethod,           /* xOpen         */
     fts3tokCloseMethod,          /* xClose        */
     fts3tokFilterMethod,         /* xFilter       */
     fts3tokNextMethod,           /* xNext         */
     fts3tokEofMethod,            /* xEof          */
     fts3tokColumnMethod,         /* xColumn       */
     fts3tokRowidMethod,          /* xRowid        */
     0,                           /* xUpdate       */
     0,                           /* xBegin        */
     0,                           /* xSync         */
     0,                           /* xCommit       */
     0,                           /* xRollback     */
     0,                           /* xFindFunction */
     0,                           /* xRename       */
     0,                           /* xSavepoint    */
     0,                           /* xRelease      */
     0                            /* xRollbackTo   */
  };
  int rc;                         /* Return code */

  rc = sqlite3_create_module(db, "fts3tokenize", &fts3tok_module, (void*)pHash);
  return rc;
}

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */

/************** End of fts3_tokenize_vtab.c **********************************/
/************** Begin file fts3_write.c **************************************/
/*
** 2009 Oct 23
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file is part of the SQLite FTS3 extension module. Specifically,
** this file contains code to insert, update and delete rows from FTS3
** tables. It also contains code to merge FTS3 b-tree segments. Some
** of the sub-routines used to merge segments are also used by the query 
** code in fts3.c.
*/

#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <string.h> */
/* #include <assert.h> */
/* #include <stdlib.h> */


#define FTS_MAX_APPENDABLE_HEIGHT 16

/*
** When full-text index nodes are loaded from disk, the buffer that they
** are loaded into has the following number of bytes of padding at the end 
** of it. i.e. if a full-text index node is 900 bytes in size, then a buffer
** of 920 bytes is allocated for it.
**
** This means that if we have a pointer into a buffer containing node data,
** it is always safe to read up to two varints from it without risking an
** overread, even if the node data is corrupted.
*/
#define FTS3_NODE_PADDING (FTS3_VARINT_MAX*2)

/*
** Under certain circumstances, b-tree nodes (doclists) can be loaded into
** memory incrementally instead of all at once. This can be a big performance
** win (reduced IO and CPU) if SQLite stops calling the virtual table xNext()
** method before retrieving all query results (as may happen, for example,
** if a query has a LIMIT clause).
**
** Incremental loading is used for b-tree nodes FTS3_NODE_CHUNK_THRESHOLD 
** bytes and larger. Nodes are loaded in chunks of FTS3_NODE_CHUNKSIZE bytes.
** The code is written so that the hard lower-limit for each of these values 
** is 1. Clearly such small values would be inefficient, but can be useful 
** for testing purposes.
**
** If this module is built with SQLITE_TEST defined, these constants may
** be overridden at runtime for testing purposes. File fts3_test.c contains
** a Tcl interface to read and write the values.
*/
#ifdef SQLITE_TEST
int test_fts3_node_chunksize = (4*1024);
int test_fts3_node_chunk_threshold = (4*1024)*4;
# define FTS3_NODE_CHUNKSIZE       test_fts3_node_chunksize
# define FTS3_NODE_CHUNK_THRESHOLD test_fts3_node_chunk_threshold
#else
# define FTS3_NODE_CHUNKSIZE (4*1024) 
# define FTS3_NODE_CHUNK_THRESHOLD (FTS3_NODE_CHUNKSIZE*4)
#endif

/*
** The two values that may be meaningfully bound to the :1 parameter in
** statements SQL_REPLACE_STAT and SQL_SELECT_STAT.
*/
#define FTS_STAT_DOCTOTAL      0
#define FTS_STAT_INCRMERGEHINT 1
#define FTS_STAT_AUTOINCRMERGE 2

/*
** If FTS_LOG_MERGES is defined, call sqlite3_log() to report each automatic
** and incremental merge operation that takes place. This is used for 
** debugging FTS only, it should not usually be turned on in production
** systems.
*/
#ifdef FTS3_LOG_MERGES
static void fts3LogMerge(int nMerge, sqlite3_int64 iAbsLevel){
  sqlite3_log(SQLITE_OK, "%d-way merge from level %d", nMerge, (int)iAbsLevel);
}
#else
#define fts3LogMerge(x, y)
#endif


typedef struct PendingList PendingList;
typedef struct SegmentNode SegmentNode;
typedef struct SegmentWriter SegmentWriter;

/*
** An instance of the following data structure is used to build doclists
** incrementally. See function fts3PendingListAppend() for details.
*/
struct PendingList {
  int nData;
  char *aData;
  int nSpace;
  sqlite3_int64 iLastDocid;
  sqlite3_int64 iLastCol;
  sqlite3_int64 iLastPos;
};


/*
** Each cursor has a (possibly empty) linked list of the following objects.
*/
struct Fts3DeferredToken {
  Fts3PhraseToken *pToken;        /* Pointer to corresponding expr token */
  int iCol;                       /* Column token must occur in */
  Fts3DeferredToken *pNext;       /* Next in list of deferred tokens */
  PendingList *pList;             /* Doclist is assembled here */
};

/*
** An instance of this structure is used to iterate through the terms on
** a contiguous set of segment b-tree leaf nodes. Although the details of
** this structure are only manipulated by code in this file, opaque handles
** of type Fts3SegReader* are also used by code in fts3.c to iterate through
** terms when querying the full-text index. See functions:
**
**   sqlite3Fts3SegReaderNew()
**   sqlite3Fts3SegReaderFree()
**   sqlite3Fts3SegReaderIterate()
**
** Methods used to manipulate Fts3SegReader structures:
**
**   fts3SegReaderNext()
**   fts3SegReaderFirstDocid()
**   fts3SegReaderNextDocid()
*/
struct Fts3SegReader {
  int iIdx;                       /* Index within level, or 0x7FFFFFFF for PT */
  u8 bLookup;                     /* True for a lookup only */
  u8 rootOnly;                    /* True for a root-only reader */

  sqlite3_int64 iStartBlock;      /* Rowid of first leaf block to traverse */
  sqlite3_int64 iLeafEndBlock;    /* Rowid of final leaf block to traverse */
  sqlite3_int64 iEndBlock;        /* Rowid of final block in segment (or 0) */
  sqlite3_int64 iCurrentBlock;    /* Current leaf block (or 0) */

  char *aNode;                    /* Pointer to node data (or NULL) */
  int nNode;                      /* Size of buffer at aNode (or 0) */
  int nPopulate;                  /* If >0, bytes of buffer aNode[] loaded */
  sqlite3_blob *pBlob;            /* If not NULL, blob handle to read node */

  Fts3HashElem **ppNextElem;

  /* Variables set by fts3SegReaderNext(). These may be read directly
  ** by the caller. They are valid from the time SegmentReaderNew() returns
  ** until SegmentReaderNext() returns something other than SQLITE_OK
  ** (i.e. SQLITE_DONE).
  */
  int nTerm;                      /* Number of bytes in current term */
  char *zTerm;                    /* Pointer to current term */
  int nTermAlloc;                 /* Allocated size of zTerm buffer */
  char *aDoclist;                 /* Pointer to doclist of current entry */
  int nDoclist;                   /* Size of doclist in current entry */

  /* The following variables are used by fts3SegReaderNextDocid() to iterate 
  ** through the current doclist (aDoclist/nDoclist).
  */
  char *pOffsetList;
  int nOffsetList;                /* For descending pending seg-readers only */
  sqlite3_int64 iDocid;
};

#define fts3SegReaderIsPending(p) ((p)->ppNextElem!=0)
#define fts3SegReaderIsRootOnly(p) ((p)->rootOnly!=0)

/*
** An instance of this structure is used to create a segment b-tree in the
** database. The internal details of this type are only accessed by the
** following functions:
**
**   fts3SegWriterAdd()
**   fts3SegWriterFlush()
**   fts3SegWriterFree()
*/
struct SegmentWriter {
  SegmentNode *pTree;             /* Pointer to interior tree structure */
  sqlite3_int64 iFirst;           /* First slot in %_segments written */
  sqlite3_int64 iFree;            /* Next free slot in %_segments */
  char *zTerm;                    /* Pointer to previous term buffer */
  int nTerm;                      /* Number of bytes in zTerm */
  int nMalloc;                    /* Size of malloc'd buffer at zMalloc */
  char *zMalloc;                  /* Malloc'd space (possibly) used for zTerm */
  int nSize;                      /* Size of allocation at aData */
  int nData;                      /* Bytes of data in aData */
  char *aData;                    /* Pointer to block from malloc() */
  i64 nLeafData;                  /* Number of bytes of leaf data written */
};

/*
** Type SegmentNode is used by the following three functions to create
** the interior part of the segment b+-tree structures (everything except
** the leaf nodes). These functions and type are only ever used by code
** within the fts3SegWriterXXX() family of functions described above.
**
**   fts3NodeAddTerm()
**   fts3NodeWrite()
**   fts3NodeFree()
**
** When a b+tree is written to the database (either as a result of a merge
** or the pending-terms table being flushed), leaves are written into the 
** database file as soon as they are completely populated. The interior of
** the tree is assembled in memory and written out only once all leaves have
** been populated and stored. This is Ok, as the b+-tree fanout is usually
** very large, meaning that the interior of the tree consumes relatively 
** little memory.
*/
struct SegmentNode {
  SegmentNode *pParent;           /* Parent node (or NULL for root node) */
  SegmentNode *pRight;            /* Pointer to right-sibling */
  SegmentNode *pLeftmost;         /* Pointer to left-most node of this depth */
  int nEntry;                     /* Number of terms written to node so far */
  char *zTerm;                    /* Pointer to previous term buffer */
  int nTerm;                      /* Number of bytes in zTerm */
  int nMalloc;                    /* Size of malloc'd buffer at zMalloc */
  char *zMalloc;                  /* Malloc'd space (possibly) used for zTerm */
  int nData;                      /* Bytes of valid data so far */
  char *aData;                    /* Node data */
};

/*
** Valid values for the second argument to fts3SqlStmt().
*/
#define SQL_DELETE_CONTENT             0
#define SQL_IS_EMPTY                   1
#define SQL_DELETE_ALL_CONTENT         2 
#define SQL_DELETE_ALL_SEGMENTS        3
#define SQL_DELETE_ALL_SEGDIR          4
#define SQL_DELETE_ALL_DOCSIZE         5
#define SQL_DELETE_ALL_STAT            6
#define SQL_SELECT_CONTENT_BY_ROWID    7
#define SQL_NEXT_SEGMENT_INDEX         8
#define SQL_INSERT_SEGMENTS            9
#define SQL_NEXT_SEGMENTS_ID          10
#define SQL_INSERT_SEGDIR             11
#define SQL_SELECT_LEVEL              12
#define SQL_SELECT_LEVEL_RANGE        13
#define SQL_SELECT_LEVEL_COUNT        14
#define SQL_SELECT_SEGDIR_MAX_LEVEL   15
#define SQL_DELETE_SEGDIR_LEVEL       16
#define SQL_DELETE_SEGMENTS_RANGE     17
#define SQL_CONTENT_INSERT            18
#define SQL_DELETE_DOCSIZE            19
#define SQL_REPLACE_DOCSIZE           20
#define SQL_SELECT_DOCSIZE            21
#define SQL_SELECT_STAT               22
#define SQL_REPLACE_STAT              23

#define SQL_SELECT_ALL_PREFIX_LEVEL   24
#define SQL_DELETE_ALL_TERMS_SEGDIR   25
#define SQL_DELETE_SEGDIR_RANGE       26
#define SQL_SELECT_ALL_LANGID         27
#define SQL_FIND_MERGE_LEVEL          28
#define SQL_MAX_LEAF_NODE_ESTIMATE    29
#define SQL_DELETE_SEGDIR_ENTRY       30
#define SQL_SHIFT_SEGDIR_ENTRY        31
#define SQL_SELECT_SEGDIR             32
#define SQL_CHOMP_SEGDIR              33
#define SQL_SEGMENT_IS_APPENDABLE     34
#define SQL_SELECT_INDEXES            35
#define SQL_SELECT_MXLEVEL            36

#define SQL_SELECT_LEVEL_RANGE2       37
#define SQL_UPDATE_LEVEL_IDX          38
#define SQL_UPDATE_LEVEL              39

/*
** This function is used to obtain an SQLite prepared statement handle
** for the statement identified by the second argument. If successful,
** *pp is set to the requested statement handle and SQLITE_OK returned.
** Otherwise, an SQLite error code is returned and *pp is set to 0.
**
** If argument apVal is not NULL, then it must point to an array with
** at least as many entries as the requested statement has bound 
** parameters. The values are bound to the statements parameters before
** returning.
*/
static int fts3SqlStmt(
  Fts3Table *p,                   /* Virtual table handle */
  int eStmt,                      /* One of the SQL_XXX constants above */
  sqlite3_stmt **pp,              /* OUT: Statement handle */
  sqlite3_value **apVal           /* Values to bind to statement */
){
  const char *azSql[] = {
/* 0  */  "DELETE FROM %Q.'%q_content' WHERE rowid = ?",
/* 1  */  "SELECT NOT EXISTS(SELECT docid FROM %Q.'%q_content' WHERE rowid!=?)",
/* 2  */  "DELETE FROM %Q.'%q_content'",
/* 3  */  "DELETE FROM %Q.'%q_segments'",
/* 4  */  "DELETE FROM %Q.'%q_segdir'",
/* 5  */  "DELETE FROM %Q.'%q_docsize'",
/* 6  */  "DELETE FROM %Q.'%q_stat'",
/* 7  */  "SELECT %s WHERE rowid=?",
/* 8  */  "SELECT (SELECT max(idx) FROM %Q.'%q_segdir' WHERE level = ?) + 1",
/* 9  */  "REPLACE INTO %Q.'%q_segments'(blockid, block) VALUES(?, ?)",
/* 10 */  "SELECT coalesce((SELECT max(blockid) FROM %Q.'%q_segments') + 1, 1)",
/* 11 */  "REPLACE INTO %Q.'%q_segdir' VALUES(?,?,?,?,?,?)",

          /* Return segments in order from oldest to newest.*/ 
/* 12 */  "SELECT idx, start_block, leaves_end_block, end_block, root "
            "FROM %Q.'%q_segdir' WHERE level = ? ORDER BY idx ASC",
/* 13 */  "SELECT idx, start_block, leaves_end_block, end_block, root "
            "FROM %Q.'%q_segdir' WHERE level BETWEEN ? AND ?"
            "ORDER BY level DESC, idx ASC",

/* 14 */  "SELECT count(*) FROM %Q.'%q_segdir' WHERE level = ?",
/* 15 */  "SELECT max(level) FROM %Q.'%q_segdir' WHERE level BETWEEN ? AND ?",

/* 16 */  "DELETE FROM %Q.'%q_segdir' WHERE level = ?",
/* 17 */  "DELETE FROM %Q.'%q_segments' WHERE blockid BETWEEN ? AND ?",
/* 18 */  "INSERT INTO %Q.'%q_content' VALUES(%s)",
/* 19 */  "DELETE FROM %Q.'%q_docsize' WHERE docid = ?",
/* 20 */  "REPLACE INTO %Q.'%q_docsize' VALUES(?,?)",
/* 21 */  "SELECT size FROM %Q.'%q_docsize' WHERE docid=?",
/* 22 */  "SELECT value FROM %Q.'%q_stat' WHERE id=?",
/* 23 */  "REPLACE INTO %Q.'%q_stat' VALUES(?,?)",
/* 24 */  "",
/* 25 */  "",

/* 26 */ "DELETE FROM %Q.'%q_segdir' WHERE level BETWEEN ? AND ?",
/* 27 */ "SELECT DISTINCT level / (1024 * ?) FROM %Q.'%q_segdir'",

/* This statement is used to determine which level to read the input from
** when performing an incremental merge. It returns the absolute level number
** of the oldest level in the db that contains at least ? segments. Or,
** if no level in the FTS index contains more than ? segments, the statement
** returns zero rows.  */
/* 28 */ "SELECT level FROM %Q.'%q_segdir' GROUP BY level HAVING count(*)>=?"
         "  ORDER BY (level %% 1024) ASC LIMIT 1",

/* Estimate the upper limit on the number of leaf nodes in a new segment
** created by merging the oldest :2 segments from absolute level :1. See 
** function sqlite3Fts3Incrmerge() for details.  */
/* 29 */ "SELECT 2 * total(1 + leaves_end_block - start_block) "
         "  FROM %Q.'%q_segdir' WHERE level = ? AND idx < ?",

/* SQL_DELETE_SEGDIR_ENTRY
**   Delete the %_segdir entry on absolute level :1 with index :2.  */
/* 30 */ "DELETE FROM %Q.'%q_segdir' WHERE level = ? AND idx = ?",

/* SQL_SHIFT_SEGDIR_ENTRY
**   Modify the idx value for the segment with idx=:3 on absolute level :2
**   to :1.  */
/* 31 */ "UPDATE %Q.'%q_segdir' SET idx = ? WHERE level=? AND idx=?",

/* SQL_SELECT_SEGDIR
**   Read a single entry from the %_segdir table. The entry from absolute 
**   level :1 with index value :2.  */
/* 32 */  "SELECT idx, start_block, leaves_end_block, end_block, root "
            "FROM %Q.'%q_segdir' WHERE level = ? AND idx = ?",

/* SQL_CHOMP_SEGDIR
**   Update the start_block (:1) and root (:2) fields of the %_segdir
**   entry located on absolute level :3 with index :4.  */
/* 33 */  "UPDATE %Q.'%q_segdir' SET start_block = ?, root = ?"
            "WHERE level = ? AND idx = ?",

/* SQL_SEGMENT_IS_APPENDABLE
**   Return a single row if the segment with end_block=? is appendable. Or
**   no rows otherwise.  */
/* 34 */  "SELECT 1 FROM %Q.'%q_segments' WHERE blockid=? AND block IS NULL",

/* SQL_SELECT_INDEXES
**   Return the list of valid segment indexes for absolute level ?  */
/* 35 */  "SELECT idx FROM %Q.'%q_segdir' WHERE level=? ORDER BY 1 ASC",

/* SQL_SELECT_MXLEVEL
**   Return the largest relative level in the FTS index or indexes.  */
/* 36 */  "SELECT max( level %% 1024 ) FROM %Q.'%q_segdir'",

          /* Return segments in order from oldest to newest.*/ 
/* 37 */  "SELECT level, idx, end_block "
            "FROM %Q.'%q_segdir' WHERE level BETWEEN ? AND ? "
            "ORDER BY level DESC, idx ASC",

          /* Update statements used while promoting segments */
/* 38 */  "UPDATE OR FAIL %Q.'%q_segdir' SET level=-1,idx=? "
            "WHERE level=? AND idx=?",
/* 39 */  "UPDATE OR FAIL %Q.'%q_segdir' SET level=? WHERE level=-1"

  };
  int rc = SQLITE_OK;
  sqlite3_stmt *pStmt;

  assert( SizeofArray(azSql)==SizeofArray(p->aStmt) );
  assert( eStmt<SizeofArray(azSql) && eStmt>=0 );
  
  pStmt = p->aStmt[eStmt];
  if( !pStmt ){
    char *zSql;
    if( eStmt==SQL_CONTENT_INSERT ){
      zSql = sqlite3_mprintf(azSql[eStmt], p->zDb, p->zName, p->zWriteExprlist);
    }else if( eStmt==SQL_SELECT_CONTENT_BY_ROWID ){
      zSql = sqlite3_mprintf(azSql[eStmt], p->zReadExprlist);
    }else{
      zSql = sqlite3_mprintf(azSql[eStmt], p->zDb, p->zName);
    }
    if( !zSql ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlite3_prepare_v2(p->db, zSql, -1, &pStmt, NULL);
      sqlite3_free(zSql);
      assert( rc==SQLITE_OK || pStmt==0 );
      p->aStmt[eStmt] = pStmt;
    }
  }
  if( apVal ){
    int i;
    int nParam = sqlite3_bind_parameter_count(pStmt);
    for(i=0; rc==SQLITE_OK && i<nParam; i++){
      rc = sqlite3_bind_value(pStmt, i+1, apVal[i]);
    }
  }
  *pp = pStmt;
  return rc;
}


static int fts3SelectDocsize(
  Fts3Table *pTab,                /* FTS3 table handle */
  sqlite3_int64 iDocid,           /* Docid to bind for SQL_SELECT_DOCSIZE */
  sqlite3_stmt **ppStmt           /* OUT: Statement handle */
){
  sqlite3_stmt *pStmt = 0;        /* Statement requested from fts3SqlStmt() */
  int rc;                         /* Return code */

  rc = fts3SqlStmt(pTab, SQL_SELECT_DOCSIZE, &pStmt, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pStmt, 1, iDocid);
    rc = sqlite3_step(pStmt);
    if( rc!=SQLITE_ROW || sqlite3_column_type(pStmt, 0)!=SQLITE_BLOB ){
      rc = sqlite3_reset(pStmt);
      if( rc==SQLITE_OK ) rc = FTS_CORRUPT_VTAB;
      pStmt = 0;
    }else{
      rc = SQLITE_OK;
    }
  }

  *ppStmt = pStmt;
  return rc;
}

SQLITE_PRIVATE int sqlite3Fts3SelectDoctotal(
  Fts3Table *pTab,                /* Fts3 table handle */
  sqlite3_stmt **ppStmt           /* OUT: Statement handle */
){
  sqlite3_stmt *pStmt = 0;
  int rc;
  rc = fts3SqlStmt(pTab, SQL_SELECT_STAT, &pStmt, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int(pStmt, 1, FTS_STAT_DOCTOTAL);
    if( sqlite3_step(pStmt)!=SQLITE_ROW
     || sqlite3_column_type(pStmt, 0)!=SQLITE_BLOB
    ){
      rc = sqlite3_reset(pStmt);
      if( rc==SQLITE_OK ) rc = FTS_CORRUPT_VTAB;
      pStmt = 0;
    }
  }
  *ppStmt = pStmt;
  return rc;
}

SQLITE_PRIVATE int sqlite3Fts3SelectDocsize(
  Fts3Table *pTab,                /* Fts3 table handle */
  sqlite3_int64 iDocid,           /* Docid to read size data for */
  sqlite3_stmt **ppStmt           /* OUT: Statement handle */
){
  return fts3SelectDocsize(pTab, iDocid, ppStmt);
}

/*
** Similar to fts3SqlStmt(). Except, after binding the parameters in
** array apVal[] to the SQL statement identified by eStmt, the statement
** is executed.
**
** Returns SQLITE_OK if the statement is successfully executed, or an
** SQLite error code otherwise.
*/
static void fts3SqlExec(
  int *pRC,                /* Result code */
  Fts3Table *p,            /* The FTS3 table */
  int eStmt,               /* Index of statement to evaluate */
  sqlite3_value **apVal    /* Parameters to bind */
){
  sqlite3_stmt *pStmt;
  int rc;
  if( *pRC ) return;
  rc = fts3SqlStmt(p, eStmt, &pStmt, apVal); 
  if( rc==SQLITE_OK ){
    sqlite3_step(pStmt);
    rc = sqlite3_reset(pStmt);
  }
  *pRC = rc;
}


/*
** This function ensures that the caller has obtained an exclusive 
** shared-cache table-lock on the %_segdir table. This is required before 
** writing data to the fts3 table. If this lock is not acquired first, then
** the caller may end up attempting to take this lock as part of committing
** a transaction, causing SQLite to return SQLITE_LOCKED or 
** LOCKED_SHAREDCACHEto a COMMIT command.
**
** It is best to avoid this because if FTS3 returns any error when 
** committing a transaction, the whole transaction will be rolled back. 
** And this is not what users expect when they get SQLITE_LOCKED_SHAREDCACHE. 
** It can still happen if the user locks the underlying tables directly 
** instead of accessing them via FTS.
*/
static int fts3Writelock(Fts3Table *p){
  int rc = SQLITE_OK;
  
  if( p->nPendingData==0 ){
    sqlite3_stmt *pStmt;
    rc = fts3SqlStmt(p, SQL_DELETE_SEGDIR_LEVEL, &pStmt, 0);
    if( rc==SQLITE_OK ){
      sqlite3_bind_null(pStmt, 1);
      sqlite3_step(pStmt);
      rc = sqlite3_reset(pStmt);
    }
  }

  return rc;
}

/*
** FTS maintains a separate indexes for each language-id (a 32-bit integer).
** Within each language id, a separate index is maintained to store the
** document terms, and each configured prefix size (configured the FTS 
** "prefix=" option). And each index consists of multiple levels ("relative
** levels").
**
** All three of these values (the language id, the specific index and the
** level within the index) are encoded in 64-bit integer values stored
** in the %_segdir table on disk. This function is used to convert three
** separate component values into the single 64-bit integer value that
** can be used to query the %_segdir table.
**
** Specifically, each language-id/index combination is allocated 1024 
** 64-bit integer level values ("absolute levels"). The main terms index
** for language-id 0 is allocate values 0-1023. The first prefix index
** (if any) for language-id 0 is allocated values 1024-2047. And so on.
** Language 1 indexes are allocated immediately following language 0.
**
** So, for a system with nPrefix prefix indexes configured, the block of
** absolute levels that corresponds to language-id iLangid and index 
** iIndex starts at absolute level ((iLangid * (nPrefix+1) + iIndex) * 1024).
*/
static sqlite3_int64 getAbsoluteLevel(
  Fts3Table *p,                   /* FTS3 table handle */
  int iLangid,                    /* Language id */
  int iIndex,                     /* Index in p->aIndex[] */
  int iLevel                      /* Level of segments */
){
  sqlite3_int64 iBase;            /* First absolute level for iLangid/iIndex */
  assert( iLangid>=0 );
  assert( p->nIndex>0 );
  assert( iIndex>=0 && iIndex<p->nIndex );

  iBase = ((sqlite3_int64)iLangid * p->nIndex + iIndex) * FTS3_SEGDIR_MAXLEVEL;
  return iBase + iLevel;
}

/*
** Set *ppStmt to a statement handle that may be used to iterate through
** all rows in the %_segdir table, from oldest to newest. If successful,
** return SQLITE_OK. If an error occurs while preparing the statement, 
** return an SQLite error code.
**
** There is only ever one instance of this SQL statement compiled for
** each FTS3 table.
**
** The statement returns the following columns from the %_segdir table:
**
**   0: idx
**   1: start_block
**   2: leaves_end_block
**   3: end_block
**   4: root
*/
SQLITE_PRIVATE int sqlite3Fts3AllSegdirs(
  Fts3Table *p,                   /* FTS3 table */
  int iLangid,                    /* Language being queried */
  int iIndex,                     /* Index for p->aIndex[] */
  int iLevel,                     /* Level to select (relative level) */
  sqlite3_stmt **ppStmt           /* OUT: Compiled statement */
){
  int rc;
  sqlite3_stmt *pStmt = 0;

  assert( iLevel==FTS3_SEGCURSOR_ALL || iLevel>=0 );
  assert( iLevel<FTS3_SEGDIR_MAXLEVEL );
  assert( iIndex>=0 && iIndex<p->nIndex );

  if( iLevel<0 ){
    /* "SELECT * FROM %_segdir WHERE level BETWEEN ? AND ? ORDER BY ..." */
    rc = fts3SqlStmt(p, SQL_SELECT_LEVEL_RANGE, &pStmt, 0);
    if( rc==SQLITE_OK ){ 
      sqlite3_bind_int64(pStmt, 1, getAbsoluteLevel(p, iLangid, iIndex, 0));
      sqlite3_bind_int64(pStmt, 2, 
          getAbsoluteLevel(p, iLangid, iIndex, FTS3_SEGDIR_MAXLEVEL-1)
      );
    }
  }else{
    /* "SELECT * FROM %_segdir WHERE level = ? ORDER BY ..." */
    rc = fts3SqlStmt(p, SQL_SELECT_LEVEL, &pStmt, 0);
    if( rc==SQLITE_OK ){ 
      sqlite3_bind_int64(pStmt, 1, getAbsoluteLevel(p, iLangid, iIndex,iLevel));
    }
  }
  *ppStmt = pStmt;
  return rc;
}


/*
** Append a single varint to a PendingList buffer. SQLITE_OK is returned
** if successful, or an SQLite error code otherwise.
**
** This function also serves to allocate the PendingList structure itself.
** For example, to create a new PendingList structure containing two
** varints:
**
**   PendingList *p = 0;
**   fts3PendingListAppendVarint(&p, 1);
**   fts3PendingListAppendVarint(&p, 2);
*/
static int fts3PendingListAppendVarint(
  PendingList **pp,               /* IN/OUT: Pointer to PendingList struct */
  sqlite3_int64 i                 /* Value to append to data */
){
  PendingList *p = *pp;

  /* Allocate or grow the PendingList as required. */
  if( !p ){
    p = sqlite3_malloc(sizeof(*p) + 100);
    if( !p ){
      return SQLITE_NOMEM;
    }
    p->nSpace = 100;
    p->aData = (char *)&p[1];
    p->nData = 0;
  }
  else if( p->nData+FTS3_VARINT_MAX+1>p->nSpace ){
    int nNew = p->nSpace * 2;
    p = sqlite3_realloc(p, sizeof(*p) + nNew);
    if( !p ){
      sqlite3_free(*pp);
      *pp = 0;
      return SQLITE_NOMEM;
    }
    p->nSpace = nNew;
    p->aData = (char *)&p[1];
  }

  /* Append the new serialized varint to the end of the list. */
  p->nData += sqlite3Fts3PutVarint(&p->aData[p->nData], i);
  p->aData[p->nData] = '\0';
  *pp = p;
  return SQLITE_OK;
}

/*
** Add a docid/column/position entry to a PendingList structure. Non-zero
** is returned if the structure is sqlite3_realloced as part of adding
** the entry. Otherwise, zero.
**
** If an OOM error occurs, *pRc is set to SQLITE_NOMEM before returning.
** Zero is always returned in this case. Otherwise, if no OOM error occurs,
** it is set to SQLITE_OK.
*/
static int fts3PendingListAppend(
  PendingList **pp,               /* IN/OUT: PendingList structure */
  sqlite3_int64 iDocid,           /* Docid for entry to add */
  sqlite3_int64 iCol,             /* Column for entry to add */
  sqlite3_int64 iPos,             /* Position of term for entry to add */
  int *pRc                        /* OUT: Return code */
){
  PendingList *p = *pp;
  int rc = SQLITE_OK;

  assert( !p || p->iLastDocid<=iDocid );

  if( !p || p->iLastDocid!=iDocid ){
    sqlite3_int64 iDelta = iDocid - (p ? p->iLastDocid : 0);
    if( p ){
      assert( p->nData<p->nSpace );
      assert( p->aData[p->nData]==0 );
      p->nData++;
    }
    if( SQLITE_OK!=(rc = fts3PendingListAppendVarint(&p, iDelta)) ){
      goto pendinglistappend_out;
    }
    p->iLastCol = -1;
    p->iLastPos = 0;
    p->iLastDocid = iDocid;
  }
  if( iCol>0 && p->iLastCol!=iCol ){
    if( SQLITE_OK!=(rc = fts3PendingListAppendVarint(&p, 1))
     || SQLITE_OK!=(rc = fts3PendingListAppendVarint(&p, iCol))
    ){
      goto pendinglistappend_out;
    }
    p->iLastCol = iCol;
    p->iLastPos = 0;
  }
  if( iCol>=0 ){
    assert( iPos>p->iLastPos || (iPos==0 && p->iLastPos==0) );
    rc = fts3PendingListAppendVarint(&p, 2+iPos-p->iLastPos);
    if( rc==SQLITE_OK ){
      p->iLastPos = iPos;
    }
  }

 pendinglistappend_out:
  *pRc = rc;
  if( p!=*pp ){
    *pp = p;
    return 1;
  }
  return 0;
}

/*
** Free a PendingList object allocated by fts3PendingListAppend().
*/
static void fts3PendingListDelete(PendingList *pList){
  sqlite3_free(pList);
}

/*
** Add an entry to one of the pending-terms hash tables.
*/
static int fts3PendingTermsAddOne(
  Fts3Table *p,
  int iCol,
  int iPos,
  Fts3Hash *pHash,                /* Pending terms hash table to add entry to */
  const char *zToken,
  int nToken
){
  PendingList *pList;
  int rc = SQLITE_OK;

  pList = (PendingList *)fts3HashFind(pHash, zToken, nToken);
  if( pList ){
    p->nPendingData -= (pList->nData + nToken + sizeof(Fts3HashElem));
  }
  if( fts3PendingListAppend(&pList, p->iPrevDocid, iCol, iPos, &rc) ){
    if( pList==fts3HashInsert(pHash, zToken, nToken, pList) ){
      /* Malloc failed while inserting the new entry. This can only 
      ** happen if there was no previous entry for this token.
      */
      assert( 0==fts3HashFind(pHash, zToken, nToken) );
      sqlite3_free(pList);
      rc = SQLITE_NOMEM;
    }
  }
  if( rc==SQLITE_OK ){
    p->nPendingData += (pList->nData + nToken + sizeof(Fts3HashElem));
  }
  return rc;
}

/*
** Tokenize the nul-terminated string zText and add all tokens to the
** pending-terms hash-table. The docid used is that currently stored in
** p->iPrevDocid, and the column is specified by argument iCol.
**
** If successful, SQLITE_OK is returned. Otherwise, an SQLite error code.
*/
static int fts3PendingTermsAdd(
  Fts3Table *p,                   /* Table into which text will be inserted */
  int iLangid,                    /* Language id to use */
  const char *zText,              /* Text of document to be inserted */
  int iCol,                       /* Column into which text is being inserted */
  u32 *pnWord                     /* IN/OUT: Incr. by number tokens inserted */
){
  int rc;
  int iStart = 0;
  int iEnd = 0;
  int iPos = 0;
  int nWord = 0;

  char const *zToken;
  int nToken = 0;

  sqlite3_tokenizer *pTokenizer = p->pTokenizer;
  sqlite3_tokenizer_module const *pModule = pTokenizer->pModule;
  sqlite3_tokenizer_cursor *pCsr;
  int (*xNext)(sqlite3_tokenizer_cursor *pCursor,
      const char**,int*,int*,int*,int*);

  assert( pTokenizer && pModule );

  /* If the user has inserted a NULL value, this function may be called with
  ** zText==0. In this case, add zero token entries to the hash table and 
  ** return early. */
  if( zText==0 ){
    *pnWord = 0;
    return SQLITE_OK;
  }

  rc = sqlite3Fts3OpenTokenizer(pTokenizer, iLangid, zText, -1, &pCsr);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  xNext = pModule->xNext;
  while( SQLITE_OK==rc
      && SQLITE_OK==(rc = xNext(pCsr, &zToken, &nToken, &iStart, &iEnd, &iPos))
  ){
    int i;
    if( iPos>=nWord ) nWord = iPos+1;

    /* Positions cannot be negative; we use -1 as a terminator internally.
    ** Tokens must have a non-zero length.
    */
    if( iPos<0 || !zToken || nToken<=0 ){
      rc = SQLITE_ERROR;
      break;
    }

    /* Add the term to the terms index */
    rc = fts3PendingTermsAddOne(
        p, iCol, iPos, &p->aIndex[0].hPending, zToken, nToken
    );
    
    /* Add the term to each of the prefix indexes that it is not too 
    ** short for. */
    for(i=1; rc==SQLITE_OK && i<p->nIndex; i++){
      struct Fts3Index *pIndex = &p->aIndex[i];
      if( nToken<pIndex->nPrefix ) continue;
      rc = fts3PendingTermsAddOne(
          p, iCol, iPos, &pIndex->hPending, zToken, pIndex->nPrefix
      );
    }
  }

  pModule->xClose(pCsr);
  *pnWord += nWord;
  return (rc==SQLITE_DONE ? SQLITE_OK : rc);
}

/* 
** Calling this function indicates that subsequent calls to 
** fts3PendingTermsAdd() are to add term/position-list pairs for the
** contents of the document with docid iDocid.
*/
static int fts3PendingTermsDocid(
  Fts3Table *p,                   /* Full-text table handle */
  int iLangid,                    /* Language id of row being written */
  sqlite_int64 iDocid             /* Docid of row being written */
){
  assert( iLangid>=0 );

  /* TODO(shess) Explore whether partially flushing the buffer on
  ** forced-flush would provide better performance.  I suspect that if
  ** we ordered the doclists by size and flushed the largest until the
  ** buffer was half empty, that would let the less frequent terms
  ** generate longer doclists.
  */
  if( iDocid<=p->iPrevDocid 
   || p->iPrevLangid!=iLangid
   || p->nPendingData>p->nMaxPendingData 
  ){
    int rc = sqlite3Fts3PendingTermsFlush(p);
    if( rc!=SQLITE_OK ) return rc;
  }
  p->iPrevDocid = iDocid;
  p->iPrevLangid = iLangid;
  return SQLITE_OK;
}

/*
** Discard the contents of the pending-terms hash tables. 
*/
SQLITE_PRIVATE void sqlite3Fts3PendingTermsClear(Fts3Table *p){
  int i;
  for(i=0; i<p->nIndex; i++){
    Fts3HashElem *pElem;
    Fts3Hash *pHash = &p->aIndex[i].hPending;
    for(pElem=fts3HashFirst(pHash); pElem; pElem=fts3HashNext(pElem)){
      PendingList *pList = (PendingList *)fts3HashData(pElem);
      fts3PendingListDelete(pList);
    }
    fts3HashClear(pHash);
  }
  p->nPendingData = 0;
}

/*
** This function is called by the xUpdate() method as part of an INSERT
** operation. It adds entries for each term in the new record to the
** pendingTerms hash table.
**
** Argument apVal is the same as the similarly named argument passed to
** fts3InsertData(). Parameter iDocid is the docid of the new row.
*/
static int fts3InsertTerms(
  Fts3Table *p, 
  int iLangid, 
  sqlite3_value **apVal, 
  u32 *aSz
){
  int i;                          /* Iterator variable */
  for(i=2; i<p->nColumn+2; i++){
    int iCol = i-2;
    if( p->abNotindexed[iCol]==0 ){
      const char *zText = (const char *)sqlite3_value_text(apVal[i]);
      int rc = fts3PendingTermsAdd(p, iLangid, zText, iCol, &aSz[iCol]);
      if( rc!=SQLITE_OK ){
        return rc;
      }
      aSz[p->nColumn] += sqlite3_value_bytes(apVal[i]);
    }
  }
  return SQLITE_OK;
}

/*
** This function is called by the xUpdate() method for an INSERT operation.
** The apVal parameter is passed a copy of the apVal argument passed by
** SQLite to the xUpdate() method. i.e:
**
**   apVal[0]                Not used for INSERT.
**   apVal[1]                rowid
**   apVal[2]                Left-most user-defined column
**   ...
**   apVal[p->nColumn+1]     Right-most user-defined column
**   apVal[p->nColumn+2]     Hidden column with same name as table
**   apVal[p->nColumn+3]     Hidden "docid" column (alias for rowid)
**   apVal[p->nColumn+4]     Hidden languageid column
*/
static int fts3InsertData(
  Fts3Table *p,                   /* Full-text table */
  sqlite3_value **apVal,          /* Array of values to insert */
  sqlite3_int64 *piDocid          /* OUT: Docid for row just inserted */
){
  int rc;                         /* Return code */
  sqlite3_stmt *pContentInsert;   /* INSERT INTO %_content VALUES(...) */

  if( p->zContentTbl ){
    sqlite3_value *pRowid = apVal[p->nColumn+3];
    if( sqlite3_value_type(pRowid)==SQLITE_NULL ){
      pRowid = apVal[1];
    }
    if( sqlite3_value_type(pRowid)!=SQLITE_INTEGER ){
      return SQLITE_CONSTRAINT;
    }
    *piDocid = sqlite3_value_int64(pRowid);
    return SQLITE_OK;
  }

  /* Locate the statement handle used to insert data into the %_content
  ** table. The SQL for this statement is:
  **
  **   INSERT INTO %_content VALUES(?, ?, ?, ...)
  **
  ** The statement features N '?' variables, where N is the number of user
  ** defined columns in the FTS3 table, plus one for the docid field.
  */
  rc = fts3SqlStmt(p, SQL_CONTENT_INSERT, &pContentInsert, &apVal[1]);
  if( rc==SQLITE_OK && p->zLanguageid ){
    rc = sqlite3_bind_int(
        pContentInsert, p->nColumn+2, 
        sqlite3_value_int(apVal[p->nColumn+4])
    );
  }
  if( rc!=SQLITE_OK ) return rc;

  /* There is a quirk here. The users INSERT statement may have specified
  ** a value for the "rowid" field, for the "docid" field, or for both.
  ** Which is a problem, since "rowid" and "docid" are aliases for the
  ** same value. For example:
  **
  **   INSERT INTO fts3tbl(rowid, docid) VALUES(1, 2);
  **
  ** In FTS3, this is an error. It is an error to specify non-NULL values
  ** for both docid and some other rowid alias.
  */
  if( SQLITE_NULL!=sqlite3_value_type(apVal[3+p->nColumn]) ){
    if( SQLITE_NULL==sqlite3_value_type(apVal[0])
     && SQLITE_NULL!=sqlite3_value_type(apVal[1])
    ){
      /* A rowid/docid conflict. */
      return SQLITE_ERROR;
    }
    rc = sqlite3_bind_value(pContentInsert, 1, apVal[3+p->nColumn]);
    if( rc!=SQLITE_OK ) return rc;
  }

  /* Execute the statement to insert the record. Set *piDocid to the 
  ** new docid value. 
  */
  sqlite3_step(pContentInsert);
  rc = sqlite3_reset(pContentInsert);

  *piDocid = sqlite3_last_insert_rowid(p->db);
  return rc;
}



/*
** Remove all data from the FTS3 table. Clear the hash table containing
** pending terms.
*/
static int fts3DeleteAll(Fts3Table *p, int bContent){
  int rc = SQLITE_OK;             /* Return code */

  /* Discard the contents of the pending-terms hash table. */
  sqlite3Fts3PendingTermsClear(p);

  /* Delete everything from the shadow tables. Except, leave %_content as
  ** is if bContent is false.  */
  assert( p->zContentTbl==0 || bContent==0 );
  if( bContent ) fts3SqlExec(&rc, p, SQL_DELETE_ALL_CONTENT, 0);
  fts3SqlExec(&rc, p, SQL_DELETE_ALL_SEGMENTS, 0);
  fts3SqlExec(&rc, p, SQL_DELETE_ALL_SEGDIR, 0);
  if( p->bHasDocsize ){
    fts3SqlExec(&rc, p, SQL_DELETE_ALL_DOCSIZE, 0);
  }
  if( p->bHasStat ){
    fts3SqlExec(&rc, p, SQL_DELETE_ALL_STAT, 0);
  }
  return rc;
}

/*
**
*/
static int langidFromSelect(Fts3Table *p, sqlite3_stmt *pSelect){
  int iLangid = 0;
  if( p->zLanguageid ) iLangid = sqlite3_column_int(pSelect, p->nColumn+1);
  return iLangid;
}

/*
** The first element in the apVal[] array is assumed to contain the docid
** (an integer) of a row about to be deleted. Remove all terms from the
** full-text index.
*/
static void fts3DeleteTerms( 
  int *pRC,               /* Result code */
  Fts3Table *p,           /* The FTS table to delete from */
  sqlite3_value *pRowid,  /* The docid to be deleted */
  u32 *aSz,               /* Sizes of deleted document written here */
  int *pbFound            /* OUT: Set to true if row really does exist */
){
  int rc;
  sqlite3_stmt *pSelect;

  assert( *pbFound==0 );
  if( *pRC ) return;
  rc = fts3SqlStmt(p, SQL_SELECT_CONTENT_BY_ROWID, &pSelect, &pRowid);
  if( rc==SQLITE_OK ){
    if( SQLITE_ROW==sqlite3_step(pSelect) ){
      int i;
      int iLangid = langidFromSelect(p, pSelect);
      rc = fts3PendingTermsDocid(p, iLangid, sqlite3_column_int64(pSelect, 0));
      for(i=1; rc==SQLITE_OK && i<=p->nColumn; i++){
        int iCol = i-1;
        if( p->abNotindexed[iCol]==0 ){
          const char *zText = (const char *)sqlite3_column_text(pSelect, i);
          rc = fts3PendingTermsAdd(p, iLangid, zText, -1, &aSz[iCol]);
          aSz[p->nColumn] += sqlite3_column_bytes(pSelect, i);
        }
      }
      if( rc!=SQLITE_OK ){
        sqlite3_reset(pSelect);
        *pRC = rc;
        return;
      }
      *pbFound = 1;
    }
    rc = sqlite3_reset(pSelect);
  }else{
    sqlite3_reset(pSelect);
  }
  *pRC = rc;
}

/*
** Forward declaration to account for the circular dependency between
** functions fts3SegmentMerge() and fts3AllocateSegdirIdx().
*/
static int fts3SegmentMerge(Fts3Table *, int, int, int);

/* 
** This function allocates a new level iLevel index in the segdir table.
** Usually, indexes are allocated within a level sequentially starting
** with 0, so the allocated index is one greater than the value returned
** by:
**
**   SELECT max(idx) FROM %_segdir WHERE level = :iLevel
**
** However, if there are already FTS3_MERGE_COUNT indexes at the requested
** level, they are merged into a single level (iLevel+1) segment and the 
** allocated index is 0.
**
** If successful, *piIdx is set to the allocated index slot and SQLITE_OK
** returned. Otherwise, an SQLite error code is returned.
*/
static int fts3AllocateSegdirIdx(
  Fts3Table *p, 
  int iLangid,                    /* Language id */
  int iIndex,                     /* Index for p->aIndex */
  int iLevel, 
  int *piIdx
){
  int rc;                         /* Return Code */
  sqlite3_stmt *pNextIdx;         /* Query for next idx at level iLevel */
  int iNext = 0;                  /* Result of query pNextIdx */

  assert( iLangid>=0 );
  assert( p->nIndex>=1 );

  /* Set variable iNext to the next available segdir index at level iLevel. */
  rc = fts3SqlStmt(p, SQL_NEXT_SEGMENT_INDEX, &pNextIdx, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(
        pNextIdx, 1, getAbsoluteLevel(p, iLangid, iIndex, iLevel)
    );
    if( SQLITE_ROW==sqlite3_step(pNextIdx) ){
      iNext = sqlite3_column_int(pNextIdx, 0);
    }
    rc = sqlite3_reset(pNextIdx);
  }

  if( rc==SQLITE_OK ){
    /* If iNext is FTS3_MERGE_COUNT, indicating that level iLevel is already
    ** full, merge all segments in level iLevel into a single iLevel+1
    ** segment and allocate (newly freed) index 0 at level iLevel. Otherwise,
    ** if iNext is less than FTS3_MERGE_COUNT, allocate index iNext.
    */
    if( iNext>=FTS3_MERGE_COUNT ){
      fts3LogMerge(16, getAbsoluteLevel(p, iLangid, iIndex, iLevel));
      rc = fts3SegmentMerge(p, iLangid, iIndex, iLevel);
      *piIdx = 0;
    }else{
      *piIdx = iNext;
    }
  }

  return rc;
}

/*
** The %_segments table is declared as follows:
**
**   CREATE TABLE %_segments(blockid INTEGER PRIMARY KEY, block BLOB)
**
** This function reads data from a single row of the %_segments table. The
** specific row is identified by the iBlockid parameter. If paBlob is not
** NULL, then a buffer is allocated using sqlite3_malloc() and populated
** with the contents of the blob stored in the "block" column of the 
** identified table row is. Whether or not paBlob is NULL, *pnBlob is set
** to the size of the blob in bytes before returning.
**
** If an error occurs, or the table does not contain the specified row,
** an SQLite error code is returned. Otherwise, SQLITE_OK is returned. If
** paBlob is non-NULL, then it is the responsibility of the caller to
** eventually free the returned buffer.
**
** This function may leave an open sqlite3_blob* handle in the
** Fts3Table.pSegments variable. This handle is reused by subsequent calls
** to this function. The handle may be closed by calling the
** sqlite3Fts3SegmentsClose() function. Reusing a blob handle is a handy
** performance improvement, but the blob handle should always be closed
** before control is returned to the user (to prevent a lock being held
** on the database file for longer than necessary). Thus, any virtual table
** method (xFilter etc.) that may directly or indirectly call this function
** must call sqlite3Fts3SegmentsClose() before returning.
*/
SQLITE_PRIVATE int sqlite3Fts3ReadBlock(
  Fts3Table *p,                   /* FTS3 table handle */
  sqlite3_int64 iBlockid,         /* Access the row with blockid=$iBlockid */
  char **paBlob,                  /* OUT: Blob data in malloc'd buffer */
  int *pnBlob,                    /* OUT: Size of blob data */
  int *pnLoad                     /* OUT: Bytes actually loaded */
){
  int rc;                         /* Return code */

  /* pnBlob must be non-NULL. paBlob may be NULL or non-NULL. */
  assert( pnBlob );

  if( p->pSegments ){
    rc = sqlite3_blob_reopen(p->pSegments, iBlockid);
  }else{
    if( 0==p->zSegmentsTbl ){
      p->zSegmentsTbl = sqlite3_mprintf("%s_segments", p->zName);
      if( 0==p->zSegmentsTbl ) return SQLITE_NOMEM;
    }
    rc = sqlite3_blob_open(
       p->db, p->zDb, p->zSegmentsTbl, "block", iBlockid, 0, &p->pSegments
    );
  }

  if( rc==SQLITE_OK ){
    int nByte = sqlite3_blob_bytes(p->pSegments);
    *pnBlob = nByte;
    if( paBlob ){
      char *aByte = sqlite3_malloc(nByte + FTS3_NODE_PADDING);
      if( !aByte ){
        rc = SQLITE_NOMEM;
      }else{
        if( pnLoad && nByte>(FTS3_NODE_CHUNK_THRESHOLD) ){
          nByte = FTS3_NODE_CHUNKSIZE;
          *pnLoad = nByte;
        }
        rc = sqlite3_blob_read(p->pSegments, aByte, nByte, 0);
        memset(&aByte[nByte], 0, FTS3_NODE_PADDING);
        if( rc!=SQLITE_OK ){
          sqlite3_free(aByte);
          aByte = 0;
        }
      }
      *paBlob = aByte;
    }
  }

  return rc;
}

/*
** Close the blob handle at p->pSegments, if it is open. See comments above
** the sqlite3Fts3ReadBlock() function for details.
*/
SQLITE_PRIVATE void sqlite3Fts3SegmentsClose(Fts3Table *p){
  sqlite3_blob_close(p->pSegments);
  p->pSegments = 0;
}
    
static int fts3SegReaderIncrRead(Fts3SegReader *pReader){
  int nRead;                      /* Number of bytes to read */
  int rc;                         /* Return code */

  nRead = MIN(pReader->nNode - pReader->nPopulate, FTS3_NODE_CHUNKSIZE);
  rc = sqlite3_blob_read(
      pReader->pBlob, 
      &pReader->aNode[pReader->nPopulate],
      nRead,
      pReader->nPopulate
  );

  if( rc==SQLITE_OK ){
    pReader->nPopulate += nRead;
    memset(&pReader->aNode[pReader->nPopulate], 0, FTS3_NODE_PADDING);
    if( pReader->nPopulate==pReader->nNode ){
      sqlite3_blob_close(pReader->pBlob);
      pReader->pBlob = 0;
      pReader->nPopulate = 0;
    }
  }
  return rc;
}

static int fts3SegReaderRequire(Fts3SegReader *pReader, char *pFrom, int nByte){
  int rc = SQLITE_OK;
  assert( !pReader->pBlob 
       || (pFrom>=pReader->aNode && pFrom<&pReader->aNode[pReader->nNode])
  );
  while( pReader->pBlob && rc==SQLITE_OK 
     &&  (pFrom - pReader->aNode + nByte)>pReader->nPopulate
  ){
    rc = fts3SegReaderIncrRead(pReader);
  }
  return rc;
}

/*
** Set an Fts3SegReader cursor to point at EOF.
*/
static void fts3SegReaderSetEof(Fts3SegReader *pSeg){
  if( !fts3SegReaderIsRootOnly(pSeg) ){
    sqlite3_free(pSeg->aNode);
    sqlite3_blob_close(pSeg->pBlob);
    pSeg->pBlob = 0;
  }
  pSeg->aNode = 0;
}

/*
** Move the iterator passed as the first argument to the next term in the
** segment. If successful, SQLITE_OK is returned. If there is no next term,
** SQLITE_DONE. Otherwise, an SQLite error code.
*/
static int fts3SegReaderNext(
  Fts3Table *p, 
  Fts3SegReader *pReader,
  int bIncr
){
  int rc;                         /* Return code of various sub-routines */
  char *pNext;                    /* Cursor variable */
  int nPrefix;                    /* Number of bytes in term prefix */
  int nSuffix;                    /* Number of bytes in term suffix */

  if( !pReader->aDoclist ){
    pNext = pReader->aNode;
  }else{
    pNext = &pReader->aDoclist[pReader->nDoclist];
  }

  if( !pNext || pNext>=&pReader->aNode[pReader->nNode] ){

    if( fts3SegReaderIsPending(pReader) ){
      Fts3HashElem *pElem = *(pReader->ppNextElem);
      if( pElem==0 ){
        pReader->aNode = 0;
      }else{
        PendingList *pList = (PendingList *)fts3HashData(pElem);
        pReader->zTerm = (char *)fts3HashKey(pElem);
        pReader->nTerm = fts3HashKeysize(pElem);
        pReader->nNode = pReader->nDoclist = pList->nData + 1;
        pReader->aNode = pReader->aDoclist = pList->aData;
        pReader->ppNextElem++;
        assert( pReader->aNode );
      }
      return SQLITE_OK;
    }

    fts3SegReaderSetEof(pReader);

    /* If iCurrentBlock>=iLeafEndBlock, this is an EOF condition. All leaf 
    ** blocks have already been traversed.  */
    assert( pReader->iCurrentBlock<=pReader->iLeafEndBlock );
    if( pReader->iCurrentBlock>=pReader->iLeafEndBlock ){
      return SQLITE_OK;
    }

    rc = sqlite3Fts3ReadBlock(
        p, ++pReader->iCurrentBlock, &pReader->aNode, &pReader->nNode, 
        (bIncr ? &pReader->nPopulate : 0)
    );
    if( rc!=SQLITE_OK ) return rc;
    assert( pReader->pBlob==0 );
    if( bIncr && pReader->nPopulate<pReader->nNode ){
      pReader->pBlob = p->pSegments;
      p->pSegments = 0;
    }
    pNext = pReader->aNode;
  }

  assert( !fts3SegReaderIsPending(pReader) );

  rc = fts3SegReaderRequire(pReader, pNext, FTS3_VARINT_MAX*2);
  if( rc!=SQLITE_OK ) return rc;
  
  /* Because of the FTS3_NODE_PADDING bytes of padding, the following is 
  ** safe (no risk of overread) even if the node data is corrupted. */
  pNext += fts3GetVarint32(pNext, &nPrefix);
  pNext += fts3GetVarint32(pNext, &nSuffix);
  if( nPrefix<0 || nSuffix<=0 
   || &pNext[nSuffix]>&pReader->aNode[pReader->nNode] 
  ){
    return FTS_CORRUPT_VTAB;
  }

  if( nPrefix+nSuffix>pReader->nTermAlloc ){
    int nNew = (nPrefix+nSuffix)*2;
    char *zNew = sqlite3_realloc(pReader->zTerm, nNew);
    if( !zNew ){
      return SQLITE_NOMEM;
    }
    pReader->zTerm = zNew;
    pReader->nTermAlloc = nNew;
  }

  rc = fts3SegReaderRequire(pReader, pNext, nSuffix+FTS3_VARINT_MAX);
  if( rc!=SQLITE_OK ) return rc;

  memcpy(&pReader->zTerm[nPrefix], pNext, nSuffix);
  pReader->nTerm = nPrefix+nSuffix;
  pNext += nSuffix;
  pNext += fts3GetVarint32(pNext, &pReader->nDoclist);
  pReader->aDoclist = pNext;
  pReader->pOffsetList = 0;

  /* Check that the doclist does not appear to extend past the end of the
  ** b-tree node. And that the final byte of the doclist is 0x00. If either 
  ** of these statements is untrue, then the data structure is corrupt.
  */
  if( &pReader->aDoclist[pReader->nDoclist]>&pReader->aNode[pReader->nNode] 
   || (pReader->nPopulate==0 && pReader->aDoclist[pReader->nDoclist-1])
  ){
    return FTS_CORRUPT_VTAB;
  }
  return SQLITE_OK;
}

/*
** Set the SegReader to point to the first docid in the doclist associated
** with the current term.
*/
static int fts3SegReaderFirstDocid(Fts3Table *pTab, Fts3SegReader *pReader){
  int rc = SQLITE_OK;
  assert( pReader->aDoclist );
  assert( !pReader->pOffsetList );
  if( pTab->bDescIdx && fts3SegReaderIsPending(pReader) ){
    u8 bEof = 0;
    pReader->iDocid = 0;
    pReader->nOffsetList = 0;
    sqlite3Fts3DoclistPrev(0,
        pReader->aDoclist, pReader->nDoclist, &pReader->pOffsetList, 
        &pReader->iDocid, &pReader->nOffsetList, &bEof
    );
  }else{
    rc = fts3SegReaderRequire(pReader, pReader->aDoclist, FTS3_VARINT_MAX);
    if( rc==SQLITE_OK ){
      int n = sqlite3Fts3GetVarint(pReader->aDoclist, &pReader->iDocid);
      pReader->pOffsetList = &pReader->aDoclist[n];
    }
  }
  return rc;
}

/*
** Advance the SegReader to point to the next docid in the doclist
** associated with the current term.
** 
** If arguments ppOffsetList and pnOffsetList are not NULL, then 
** *ppOffsetList is set to point to the first column-offset list
** in the doclist entry (i.e. immediately past the docid varint).
** *pnOffsetList is set to the length of the set of column-offset
** lists, not including the nul-terminator byte. For example:
*/
static int fts3SegReaderNextDocid(
  Fts3Table *pTab,
  Fts3SegReader *pReader,         /* Reader to advance to next docid */
  char **ppOffsetList,            /* OUT: Pointer to current position-list */
  int *pnOffsetList               /* OUT: Length of *ppOffsetList in bytes */
){
  int rc = SQLITE_OK;
  char *p = pReader->pOffsetList;
  char c = 0;

  assert( p );

  if( pTab->bDescIdx && fts3SegReaderIsPending(pReader) ){
    /* A pending-terms seg-reader for an FTS4 table that uses order=desc.
    ** Pending-terms doclists are always built up in ascending order, so
    ** we have to iterate through them backwards here. */
    u8 bEof = 0;
    if( ppOffsetList ){
      *ppOffsetList = pReader->pOffsetList;
      *pnOffsetList = pReader->nOffsetList - 1;
    }
    sqlite3Fts3DoclistPrev(0,
        pReader->aDoclist, pReader->nDoclist, &p, &pReader->iDocid,
        &pReader->nOffsetList, &bEof
    );
    if( bEof ){
      pReader->pOffsetList = 0;
    }else{
      pReader->pOffsetList = p;
    }
  }else{
    char *pEnd = &pReader->aDoclist[pReader->nDoclist];

    /* Pointer p currently points at the first byte of an offset list. The
    ** following block advances it to point one byte past the end of
    ** the same offset list. */
    while( 1 ){
  
      /* The following line of code (and the "p++" below the while() loop) is
      ** normally all that is required to move pointer p to the desired 
      ** position. The exception is if this node is being loaded from disk
      ** incrementally and pointer "p" now points to the first byte past
      ** the populated part of pReader->aNode[].
      */
      while( *p | c ) c = *p++ & 0x80;
      assert( *p==0 );
  
      if( pReader->pBlob==0 || p<&pReader->aNode[pReader->nPopulate] ) break;
      rc = fts3SegReaderIncrRead(pReader);
      if( rc!=SQLITE_OK ) return rc;
    }
    p++;
  
    /* If required, populate the output variables with a pointer to and the
    ** size of the previous offset-list.
    */
    if( ppOffsetList ){
      *ppOffsetList = pReader->pOffsetList;
      *pnOffsetList = (int)(p - pReader->pOffsetList - 1);
    }

    /* List may have been edited in place by fts3EvalNearTrim() */
    while( p<pEnd && *p==0 ) p++;
  
    /* If there are no more entries in the doclist, set pOffsetList to
    ** NULL. Otherwise, set Fts3SegReader.iDocid to the next docid and
    ** Fts3SegReader.pOffsetList to point to the next offset list before
    ** returning.
    */
    if( p>=pEnd ){
      pReader->pOffsetList = 0;
    }else{
      rc = fts3SegReaderRequire(pReader, p, FTS3_VARINT_MAX);
      if( rc==SQLITE_OK ){
        sqlite3_int64 iDelta;
        pReader->pOffsetList = p + sqlite3Fts3GetVarint(p, &iDelta);
        if( pTab->bDescIdx ){
          pReader->iDocid -= iDelta;
        }else{
          pReader->iDocid += iDelta;
        }
      }
    }
  }

  return SQLITE_OK;
}


SQLITE_PRIVATE int sqlite3Fts3MsrOvfl(
  Fts3Cursor *pCsr, 
  Fts3MultiSegReader *pMsr,
  int *pnOvfl
){
  Fts3Table *p = (Fts3Table*)pCsr->base.pVtab;
  int nOvfl = 0;
  int ii;
  int rc = SQLITE_OK;
  int pgsz = p->nPgsz;

  assert( p->bFts4 );
  assert( pgsz>0 );

  for(ii=0; rc==SQLITE_OK && ii<pMsr->nSegment; ii++){
    Fts3SegReader *pReader = pMsr->apSegment[ii];
    if( !fts3SegReaderIsPending(pReader) 
     && !fts3SegReaderIsRootOnly(pReader) 
    ){
      sqlite3_int64 jj;
      for(jj=pReader->iStartBlock; jj<=pReader->iLeafEndBlock; jj++){
        int nBlob;
        rc = sqlite3Fts3ReadBlock(p, jj, 0, &nBlob, 0);
        if( rc!=SQLITE_OK ) break;
        if( (nBlob+35)>pgsz ){
          nOvfl += (nBlob + 34)/pgsz;
        }
      }
    }
  }
  *pnOvfl = nOvfl;
  return rc;
}

/*
** Free all allocations associated with the iterator passed as the 
** second argument.
*/
SQLITE_PRIVATE void sqlite3Fts3SegReaderFree(Fts3SegReader *pReader){
  if( pReader && !fts3SegReaderIsPending(pReader) ){
    sqlite3_free(pReader->zTerm);
    if( !fts3SegReaderIsRootOnly(pReader) ){
      sqlite3_free(pReader->aNode);
      sqlite3_blob_close(pReader->pBlob);
    }
  }
  sqlite3_free(pReader);
}

/*
** Allocate a new SegReader object.
*/
SQLITE_PRIVATE int sqlite3Fts3SegReaderNew(
  int iAge,                       /* Segment "age". */
  int bLookup,                    /* True for a lookup only */
  sqlite3_int64 iStartLeaf,       /* First leaf to traverse */
  sqlite3_int64 iEndLeaf,         /* Final leaf to traverse */
  sqlite3_int64 iEndBlock,        /* Final block of segment */
  const char *zRoot,              /* Buffer containing root node */
  int nRoot,                      /* Size of buffer containing root node */
  Fts3SegReader **ppReader        /* OUT: Allocated Fts3SegReader */
){
  Fts3SegReader *pReader;         /* Newly allocated SegReader object */
  int nExtra = 0;                 /* Bytes to allocate segment root node */

  assert( iStartLeaf<=iEndLeaf );
  if( iStartLeaf==0 ){
    nExtra = nRoot + FTS3_NODE_PADDING;
  }

  pReader = (Fts3SegReader *)sqlite3_malloc(sizeof(Fts3SegReader) + nExtra);
  if( !pReader ){
    return SQLITE_NOMEM;
  }
  memset(pReader, 0, sizeof(Fts3SegReader));
  pReader->iIdx = iAge;
  pReader->bLookup = bLookup!=0;
  pReader->iStartBlock = iStartLeaf;
  pReader->iLeafEndBlock = iEndLeaf;
  pReader->iEndBlock = iEndBlock;

  if( nExtra ){
    /* The entire segment is stored in the root node. */
    pReader->aNode = (char *)&pReader[1];
    pReader->rootOnly = 1;
    pReader->nNode = nRoot;
    memcpy(pReader->aNode, zRoot, nRoot);
    memset(&pReader->aNode[nRoot], 0, FTS3_NODE_PADDING);
  }else{
    pReader->iCurrentBlock = iStartLeaf-1;
  }
  *ppReader = pReader;
  return SQLITE_OK;
}

/*
** This is a comparison function used as a qsort() callback when sorting
** an array of pending terms by term. This occurs as part of flushing
** the contents of the pending-terms hash table to the database.
*/
static int fts3CompareElemByTerm(const void *lhs, const void *rhs){
  char *z1 = fts3HashKey(*(Fts3HashElem **)lhs);
  char *z2 = fts3HashKey(*(Fts3HashElem **)rhs);
  int n1 = fts3HashKeysize(*(Fts3HashElem **)lhs);
  int n2 = fts3HashKeysize(*(Fts3HashElem **)rhs);

  int n = (n1<n2 ? n1 : n2);
  int c = memcmp(z1, z2, n);
  if( c==0 ){
    c = n1 - n2;
  }
  return c;
}

/*
** This function is used to allocate an Fts3SegReader that iterates through
** a subset of the terms stored in the Fts3Table.pendingTerms array.
**
** If the isPrefixIter parameter is zero, then the returned SegReader iterates
** through each term in the pending-terms table. Or, if isPrefixIter is
** non-zero, it iterates through each term and its prefixes. For example, if
** the pending terms hash table contains the terms "sqlite", "mysql" and
** "firebird", then the iterator visits the following 'terms' (in the order
** shown):
**
**   f fi fir fire fireb firebi firebir firebird
**   m my mys mysq mysql
**   s sq sql sqli sqlit sqlite
**
** Whereas if isPrefixIter is zero, the terms visited are:
**
**   firebird mysql sqlite
*/
SQLITE_PRIVATE int sqlite3Fts3SegReaderPending(
  Fts3Table *p,                   /* Virtual table handle */
  int iIndex,                     /* Index for p->aIndex */
  const char *zTerm,              /* Term to search for */
  int nTerm,                      /* Size of buffer zTerm */
  int bPrefix,                    /* True for a prefix iterator */
  Fts3SegReader **ppReader        /* OUT: SegReader for pending-terms */
){
  Fts3SegReader *pReader = 0;     /* Fts3SegReader object to return */
  Fts3HashElem *pE;               /* Iterator variable */
  Fts3HashElem **aElem = 0;       /* Array of term hash entries to scan */
  int nElem = 0;                  /* Size of array at aElem */
  int rc = SQLITE_OK;             /* Return Code */
  Fts3Hash *pHash;

  pHash = &p->aIndex[iIndex].hPending;
  if( bPrefix ){
    int nAlloc = 0;               /* Size of allocated array at aElem */

    for(pE=fts3HashFirst(pHash); pE; pE=fts3HashNext(pE)){
      char *zKey = (char *)fts3HashKey(pE);
      int nKey = fts3HashKeysize(pE);
      if( nTerm==0 || (nKey>=nTerm && 0==memcmp(zKey, zTerm, nTerm)) ){
        if( nElem==nAlloc ){
          Fts3HashElem **aElem2;
          nAlloc += 16;
          aElem2 = (Fts3HashElem **)sqlite3_realloc(
              aElem, nAlloc*sizeof(Fts3HashElem *)
          );
          if( !aElem2 ){
            rc = SQLITE_NOMEM;
            nElem = 0;
            break;
          }
          aElem = aElem2;
        }

        aElem[nElem++] = pE;
      }
    }

    /* If more than one term matches the prefix, sort the Fts3HashElem
    ** objects in term order using qsort(). This uses the same comparison
    ** callback as is used when flushing terms to disk.
    */
    if( nElem>1 ){
      qsort(aElem, nElem, sizeof(Fts3HashElem *), fts3CompareElemByTerm);
    }

  }else{
    /* The query is a simple term lookup that matches at most one term in
    ** the index. All that is required is a straight hash-lookup. 
    **
    ** Because the stack address of pE may be accessed via the aElem pointer
    ** below, the "Fts3HashElem *pE" must be declared so that it is valid
    ** within this entire function, not just this "else{...}" block.
    */
    pE = fts3HashFindElem(pHash, zTerm, nTerm);
    if( pE ){
      aElem = &pE;
      nElem = 1;
    }
  }

  if( nElem>0 ){
    int nByte = sizeof(Fts3SegReader) + (nElem+1)*sizeof(Fts3HashElem *);
    pReader = (Fts3SegReader *)sqlite3_malloc(nByte);
    if( !pReader ){
      rc = SQLITE_NOMEM;
    }else{
      memset(pReader, 0, nByte);
      pReader->iIdx = 0x7FFFFFFF;
      pReader->ppNextElem = (Fts3HashElem **)&pReader[1];
      memcpy(pReader->ppNextElem, aElem, nElem*sizeof(Fts3HashElem *));
    }
  }

  if( bPrefix ){
    sqlite3_free(aElem);
  }
  *ppReader = pReader;
  return rc;
}

/*
** Compare the entries pointed to by two Fts3SegReader structures. 
** Comparison is as follows:
**
**   1) EOF is greater than not EOF.
**
**   2) The current terms (if any) are compared using memcmp(). If one
**      term is a prefix of another, the longer term is considered the
**      larger.
**
**   3) By segment age. An older segment is considered larger.
*/
static int fts3SegReaderCmp(Fts3SegReader *pLhs, Fts3SegReader *pRhs){
  int rc;
  if( pLhs->aNode && pRhs->aNode ){
    int rc2 = pLhs->nTerm - pRhs->nTerm;
    if( rc2<0 ){
      rc = memcmp(pLhs->zTerm, pRhs->zTerm, pLhs->nTerm);
    }else{
      rc = memcmp(pLhs->zTerm, pRhs->zTerm, pRhs->nTerm);
    }
    if( rc==0 ){
      rc = rc2;
    }
  }else{
    rc = (pLhs->aNode==0) - (pRhs->aNode==0);
  }
  if( rc==0 ){
    rc = pRhs->iIdx - pLhs->iIdx;
  }
  assert( rc!=0 );
  return rc;
}

/*
** A different comparison function for SegReader structures. In this
** version, it is assumed that each SegReader points to an entry in
** a doclist for identical terms. Comparison is made as follows:
**
**   1) EOF (end of doclist in this case) is greater than not EOF.
**
**   2) By current docid.
**
**   3) By segment age. An older segment is considered larger.
*/
static int fts3SegReaderDoclistCmp(Fts3SegReader *pLhs, Fts3SegReader *pRhs){
  int rc = (pLhs->pOffsetList==0)-(pRhs->pOffsetList==0);
  if( rc==0 ){
    if( pLhs->iDocid==pRhs->iDocid ){
      rc = pRhs->iIdx - pLhs->iIdx;
    }else{
      rc = (pLhs->iDocid > pRhs->iDocid) ? 1 : -1;
    }
  }
  assert( pLhs->aNode && pRhs->aNode );
  return rc;
}
static int fts3SegReaderDoclistCmpRev(Fts3SegReader *pLhs, Fts3SegReader *pRhs){
  int rc = (pLhs->pOffsetList==0)-(pRhs->pOffsetList==0);
  if( rc==0 ){
    if( pLhs->iDocid==pRhs->iDocid ){
      rc = pRhs->iIdx - pLhs->iIdx;
    }else{
      rc = (pLhs->iDocid < pRhs->iDocid) ? 1 : -1;
    }
  }
  assert( pLhs->aNode && pRhs->aNode );
  return rc;
}

/*
** Compare the term that the Fts3SegReader object passed as the first argument
** points to with the term specified by arguments zTerm and nTerm. 
**
** If the pSeg iterator is already at EOF, return 0. Otherwise, return
** -ve if the pSeg term is less than zTerm/nTerm, 0 if the two terms are
** equal, or +ve if the pSeg term is greater than zTerm/nTerm.
*/
static int fts3SegReaderTermCmp(
  Fts3SegReader *pSeg,            /* Segment reader object */
  const char *zTerm,              /* Term to compare to */
  int nTerm                       /* Size of term zTerm in bytes */
){
  int res = 0;
  if( pSeg->aNode ){
    if( pSeg->nTerm>nTerm ){
      res = memcmp(pSeg->zTerm, zTerm, nTerm);
    }else{
      res = memcmp(pSeg->zTerm, zTerm, pSeg->nTerm);
    }
    if( res==0 ){
      res = pSeg->nTerm-nTerm;
    }
  }
  return res;
}

/*
** Argument apSegment is an array of nSegment elements. It is known that
** the final (nSegment-nSuspect) members are already in sorted order
** (according to the comparison function provided). This function shuffles
** the array around until all entries are in sorted order.
*/
static void fts3SegReaderSort(
  Fts3SegReader **apSegment,                     /* Array to sort entries of */
  int nSegment,                                  /* Size of apSegment array */
  int nSuspect,                                  /* Unsorted entry count */
  int (*xCmp)(Fts3SegReader *, Fts3SegReader *)  /* Comparison function */
){
  int i;                          /* Iterator variable */

  assert( nSuspect<=nSegment );

  if( nSuspect==nSegment ) nSuspect--;
  for(i=nSuspect-1; i>=0; i--){
    int j;
    for(j=i; j<(nSegment-1); j++){
      Fts3SegReader *pTmp;
      if( xCmp(apSegment[j], apSegment[j+1])<0 ) break;
      pTmp = apSegment[j+1];
      apSegment[j+1] = apSegment[j];
      apSegment[j] = pTmp;
    }
  }

#ifndef NDEBUG
  /* Check that the list really is sorted now. */
  for(i=0; i<(nSuspect-1); i++){
    assert( xCmp(apSegment[i], apSegment[i+1])<0 );
  }
#endif
}

/* 
** Insert a record into the %_segments table.
*/
static int fts3WriteSegment(
  Fts3Table *p,                   /* Virtual table handle */
  sqlite3_int64 iBlock,           /* Block id for new block */
  char *z,                        /* Pointer to buffer containing block data */
  int n                           /* Size of buffer z in bytes */
){
  sqlite3_stmt *pStmt;
  int rc = fts3SqlStmt(p, SQL_INSERT_SEGMENTS, &pStmt, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pStmt, 1, iBlock);
    sqlite3_bind_blob(pStmt, 2, z, n, SQLITE_STATIC);
    sqlite3_step(pStmt);
    rc = sqlite3_reset(pStmt);
  }
  return rc;
}

/*
** Find the largest relative level number in the table. If successful, set
** *pnMax to this value and return SQLITE_OK. Otherwise, if an error occurs,
** set *pnMax to zero and return an SQLite error code.
*/
SQLITE_PRIVATE int sqlite3Fts3MaxLevel(Fts3Table *p, int *pnMax){
  int rc;
  int mxLevel = 0;
  sqlite3_stmt *pStmt = 0;

  rc = fts3SqlStmt(p, SQL_SELECT_MXLEVEL, &pStmt, 0);
  if( rc==SQLITE_OK ){
    if( SQLITE_ROW==sqlite3_step(pStmt) ){
      mxLevel = sqlite3_column_int(pStmt, 0);
    }
    rc = sqlite3_reset(pStmt);
  }
  *pnMax = mxLevel;
  return rc;
}

/* 
** Insert a record into the %_segdir table.
*/
static int fts3WriteSegdir(
  Fts3Table *p,                   /* Virtual table handle */
  sqlite3_int64 iLevel,           /* Value for "level" field (absolute level) */
  int iIdx,                       /* Value for "idx" field */
  sqlite3_int64 iStartBlock,      /* Value for "start_block" field */
  sqlite3_int64 iLeafEndBlock,    /* Value for "leaves_end_block" field */
  sqlite3_int64 iEndBlock,        /* Value for "end_block" field */
  sqlite3_int64 nLeafData,        /* Bytes of leaf data in segment */
  char *zRoot,                    /* Blob value for "root" field */
  int nRoot                       /* Number of bytes in buffer zRoot */
){
  sqlite3_stmt *pStmt;
  int rc = fts3SqlStmt(p, SQL_INSERT_SEGDIR, &pStmt, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pStmt, 1, iLevel);
    sqlite3_bind_int(pStmt, 2, iIdx);
    sqlite3_bind_int64(pStmt, 3, iStartBlock);
    sqlite3_bind_int64(pStmt, 4, iLeafEndBlock);
    if( nLeafData==0 ){
      sqlite3_bind_int64(pStmt, 5, iEndBlock);
    }else{
      char *zEnd = sqlite3_mprintf("%lld %lld", iEndBlock, nLeafData);
      if( !zEnd ) return SQLITE_NOMEM;
      sqlite3_bind_text(pStmt, 5, zEnd, -1, sqlite3_free);
    }
    sqlite3_bind_blob(pStmt, 6, zRoot, nRoot, SQLITE_STATIC);
    sqlite3_step(pStmt);
    rc = sqlite3_reset(pStmt);
  }
  return rc;
}

/*
** Return the size of the common prefix (if any) shared by zPrev and
** zNext, in bytes. For example, 
**
**   fts3PrefixCompress("abc", 3, "abcdef", 6)   // returns 3
**   fts3PrefixCompress("abX", 3, "abcdef", 6)   // returns 2
**   fts3PrefixCompress("abX", 3, "Xbcdef", 6)   // returns 0
*/
static int fts3PrefixCompress(
  const char *zPrev,              /* Buffer containing previous term */
  int nPrev,                      /* Size of buffer zPrev in bytes */
  const char *zNext,              /* Buffer containing next term */
  int nNext                       /* Size of buffer zNext in bytes */
){
  int n;
  UNUSED_PARAMETER(nNext);
  for(n=0; n<nPrev && zPrev[n]==zNext[n]; n++);
  return n;
}

/*
** Add term zTerm to the SegmentNode. It is guaranteed that zTerm is larger
** (according to memcmp) than the previous term.
*/
static int fts3NodeAddTerm(
  Fts3Table *p,                   /* Virtual table handle */
  SegmentNode **ppTree,           /* IN/OUT: SegmentNode handle */ 
  int isCopyTerm,                 /* True if zTerm/nTerm is transient */
  const char *zTerm,              /* Pointer to buffer containing term */
  int nTerm                       /* Size of term in bytes */
){
  SegmentNode *pTree = *ppTree;
  int rc;
  SegmentNode *pNew;

  /* First try to append the term to the current node. Return early if 
  ** this is possible.
  */
  if( pTree ){
    int nData = pTree->nData;     /* Current size of node in bytes */
    int nReq = nData;             /* Required space after adding zTerm */
    int nPrefix;                  /* Number of bytes of prefix compression */
    int nSuffix;                  /* Suffix length */

    nPrefix = fts3PrefixCompress(pTree->zTerm, pTree->nTerm, zTerm, nTerm);
    nSuffix = nTerm-nPrefix;

    nReq += sqlite3Fts3VarintLen(nPrefix)+sqlite3Fts3VarintLen(nSuffix)+nSuffix;
    if( nReq<=p->nNodeSize || !pTree->zTerm ){

      if( nReq>p->nNodeSize ){
        /* An unusual case: this is the first term to be added to the node
        ** and the static node buffer (p->nNodeSize bytes) is not large
        ** enough. Use a separately malloced buffer instead This wastes
        ** p->nNodeSize bytes, but since this scenario only comes about when
        ** the database contain two terms that share a prefix of almost 2KB, 
        ** this is not expected to be a serious problem. 
        */
        assert( pTree->aData==(char *)&pTree[1] );
        pTree->aData = (char *)sqlite3_malloc(nReq);
        if( !pTree->aData ){
          return SQLITE_NOMEM;
        }
      }

      if( pTree->zTerm ){
        /* There is no prefix-length field for first term in a node */
        nData += sqlite3Fts3PutVarint(&pTree->aData[nData], nPrefix);
      }

      nData += sqlite3Fts3PutVarint(&pTree->aData[nData], nSuffix);
      memcpy(&pTree->aData[nData], &zTerm[nPrefix], nSuffix);
      pTree->nData = nData + nSuffix;
      pTree->nEntry++;

      if( isCopyTerm ){
        if( pTree->nMalloc<nTerm ){
          char *zNew = sqlite3_realloc(pTree->zMalloc, nTerm*2);
          if( !zNew ){
            return SQLITE_NOMEM;
          }
          pTree->nMalloc = nTerm*2;
          pTree->zMalloc = zNew;
        }
        pTree->zTerm = pTree->zMalloc;
        memcpy(pTree->zTerm, zTerm, nTerm);
        pTree->nTerm = nTerm;
      }else{
        pTree->zTerm = (char *)zTerm;
        pTree->nTerm = nTerm;
      }
      return SQLITE_OK;
    }
  }

  /* If control flows to here, it was not possible to append zTerm to the
  ** current node. Create a new node (a right-sibling of the current node).
  ** If this is the first node in the tree, the term is added to it.
  **
  ** Otherwise, the term is not added to the new node, it is left empty for
  ** now. Instead, the term is inserted into the parent of pTree. If pTree 
  ** has no parent, one is created here.
  */
  pNew = (SegmentNode *)sqlite3_malloc(sizeof(SegmentNode) + p->nNodeSize);
  if( !pNew ){
    return SQLITE_NOMEM;
  }
  memset(pNew, 0, sizeof(SegmentNode));
  pNew->nData = 1 + FTS3_VARINT_MAX;
  pNew->aData = (char *)&pNew[1];

  if( pTree ){
    SegmentNode *pParent = pTree->pParent;
    rc = fts3NodeAddTerm(p, &pParent, isCopyTerm, zTerm, nTerm);
    if( pTree->pParent==0 ){
      pTree->pParent = pParent;
    }
    pTree->pRight = pNew;
    pNew->pLeftmost = pTree->pLeftmost;
    pNew->pParent = pParent;
    pNew->zMalloc = pTree->zMalloc;
    pNew->nMalloc = pTree->nMalloc;
    pTree->zMalloc = 0;
  }else{
    pNew->pLeftmost = pNew;
    rc = fts3NodeAddTerm(p, &pNew, isCopyTerm, zTerm, nTerm); 
  }

  *ppTree = pNew;
  return rc;
}

/*
** Helper function for fts3NodeWrite().
*/
static int fts3TreeFinishNode(
  SegmentNode *pTree, 
  int iHeight, 
  sqlite3_int64 iLeftChild
){
  int nStart;
  assert( iHeight>=1 && iHeight<128 );
  nStart = FTS3_VARINT_MAX - sqlite3Fts3VarintLen(iLeftChild);
  pTree->aData[nStart] = (char)iHeight;
  sqlite3Fts3PutVarint(&pTree->aData[nStart+1], iLeftChild);
  return nStart;
}

/*
** Write the buffer for the segment node pTree and all of its peers to the
** database. Then call this function recursively to write the parent of 
** pTree and its peers to the database. 
**
** Except, if pTree is a root node, do not write it to the database. Instead,
** set output variables *paRoot and *pnRoot to contain the root node.
**
** If successful, SQLITE_OK is returned and output variable *piLast is
** set to the largest blockid written to the database (or zero if no
** blocks were written to the db). Otherwise, an SQLite error code is 
** returned.
*/
static int fts3NodeWrite(
  Fts3Table *p,                   /* Virtual table handle */
  SegmentNode *pTree,             /* SegmentNode handle */
  int iHeight,                    /* Height of this node in tree */
  sqlite3_int64 iLeaf,            /* Block id of first leaf node */
  sqlite3_int64 iFree,            /* Block id of next free slot in %_segments */
  sqlite3_int64 *piLast,          /* OUT: Block id of last entry written */
  char **paRoot,                  /* OUT: Data for root node */
  int *pnRoot                     /* OUT: Size of root node in bytes */
){
  int rc = SQLITE_OK;

  if( !pTree->pParent ){
    /* Root node of the tree. */
    int nStart = fts3TreeFinishNode(pTree, iHeight, iLeaf);
    *piLast = iFree-1;
    *pnRoot = pTree->nData - nStart;
    *paRoot = &pTree->aData[nStart];
  }else{
    SegmentNode *pIter;
    sqlite3_int64 iNextFree = iFree;
    sqlite3_int64 iNextLeaf = iLeaf;
    for(pIter=pTree->pLeftmost; pIter && rc==SQLITE_OK; pIter=pIter->pRight){
      int nStart = fts3TreeFinishNode(pIter, iHeight, iNextLeaf);
      int nWrite = pIter->nData - nStart;
  
      rc = fts3WriteSegment(p, iNextFree, &pIter->aData[nStart], nWrite);
      iNextFree++;
      iNextLeaf += (pIter->nEntry+1);
    }
    if( rc==SQLITE_OK ){
      assert( iNextLeaf==iFree );
      rc = fts3NodeWrite(
          p, pTree->pParent, iHeight+1, iFree, iNextFree, piLast, paRoot, pnRoot
      );
    }
  }

  return rc;
}

/*
** Free all memory allocations associated with the tree pTree.
*/
static void fts3NodeFree(SegmentNode *pTree){
  if( pTree ){
    SegmentNode *p = pTree->pLeftmost;
    fts3NodeFree(p->pParent);
    while( p ){
      SegmentNode *pRight = p->pRight;
      if( p->aData!=(char *)&p[1] ){
        sqlite3_free(p->aData);
      }
      assert( pRight==0 || p->zMalloc==0 );
      sqlite3_free(p->zMalloc);
      sqlite3_free(p);
      p = pRight;
    }
  }
}

/*
** Add a term to the segment being constructed by the SegmentWriter object
** *ppWriter. When adding the first term to a segment, *ppWriter should
** be passed NULL. This function will allocate a new SegmentWriter object
** and return it via the input/output variable *ppWriter in this case.
**
** If successful, SQLITE_OK is returned. Otherwise, an SQLite error code.
*/
static int fts3SegWriterAdd(
  Fts3Table *p,                   /* Virtual table handle */
  SegmentWriter **ppWriter,       /* IN/OUT: SegmentWriter handle */ 
  int isCopyTerm,                 /* True if buffer zTerm must be copied */
  const char *zTerm,              /* Pointer to buffer containing term */
  int nTerm,                      /* Size of term in bytes */
  const char *aDoclist,           /* Pointer to buffer containing doclist */
  int nDoclist                    /* Size of doclist in bytes */
){
  int nPrefix;                    /* Size of term prefix in bytes */
  int nSuffix;                    /* Size of term suffix in bytes */
  int nReq;                       /* Number of bytes required on leaf page */
  int nData;
  SegmentWriter *pWriter = *ppWriter;

  if( !pWriter ){
    int rc;
    sqlite3_stmt *pStmt;

    /* Allocate the SegmentWriter structure */
    pWriter = (SegmentWriter *)sqlite3_malloc(sizeof(SegmentWriter));
    if( !pWriter ) return SQLITE_NOMEM;
    memset(pWriter, 0, sizeof(SegmentWriter));
    *ppWriter = pWriter;

    /* Allocate a buffer in which to accumulate data */
    pWriter->aData = (char *)sqlite3_malloc(p->nNodeSize);
    if( !pWriter->aData ) return SQLITE_NOMEM;
    pWriter->nSize = p->nNodeSize;

    /* Find the next free blockid in the %_segments table */
    rc = fts3SqlStmt(p, SQL_NEXT_SEGMENTS_ID, &pStmt, 0);
    if( rc!=SQLITE_OK ) return rc;
    if( SQLITE_ROW==sqlite3_step(pStmt) ){
      pWriter->iFree = sqlite3_column_int64(pStmt, 0);
      pWriter->iFirst = pWriter->iFree;
    }
    rc = sqlite3_reset(pStmt);
    if( rc!=SQLITE_OK ) return rc;
  }
  nData = pWriter->nData;

  nPrefix = fts3PrefixCompress(pWriter->zTerm, pWriter->nTerm, zTerm, nTerm);
  nSuffix = nTerm-nPrefix;

  /* Figure out how many bytes are required by this new entry */
  nReq = sqlite3Fts3VarintLen(nPrefix) +    /* varint containing prefix size */
    sqlite3Fts3VarintLen(nSuffix) +         /* varint containing suffix size */
    nSuffix +                               /* Term suffix */
    sqlite3Fts3VarintLen(nDoclist) +        /* Size of doclist */
    nDoclist;                               /* Doclist data */

  if( nData>0 && nData+nReq>p->nNodeSize ){
    int rc;

    /* The current leaf node is full. Write it out to the database. */
    rc = fts3WriteSegment(p, pWriter->iFree++, pWriter->aData, nData);
    if( rc!=SQLITE_OK ) return rc;
    p->nLeafAdd++;

    /* Add the current term to the interior node tree. The term added to
    ** the interior tree must:
    **
    **   a) be greater than the largest term on the leaf node just written
    **      to the database (still available in pWriter->zTerm), and
    **
    **   b) be less than or equal to the term about to be added to the new
    **      leaf node (zTerm/nTerm).
    **
    ** In other words, it must be the prefix of zTerm 1 byte longer than
    ** the common prefix (if any) of zTerm and pWriter->zTerm.
    */
    assert( nPrefix<nTerm );
    rc = fts3NodeAddTerm(p, &pWriter->pTree, isCopyTerm, zTerm, nPrefix+1);
    if( rc!=SQLITE_OK ) return rc;

    nData = 0;
    pWriter->nTerm = 0;

    nPrefix = 0;
    nSuffix = nTerm;
    nReq = 1 +                              /* varint containing prefix size */
      sqlite3Fts3VarintLen(nTerm) +         /* varint containing suffix size */
      nTerm +                               /* Term suffix */
      sqlite3Fts3VarintLen(nDoclist) +      /* Size of doclist */
      nDoclist;                             /* Doclist data */
  }

  /* Increase the total number of bytes written to account for the new entry. */
  pWriter->nLeafData += nReq;

  /* If the buffer currently allocated is too small for this entry, realloc
  ** the buffer to make it large enough.
  */
  if( nReq>pWriter->nSize ){
    char *aNew = sqlite3_realloc(pWriter->aData, nReq);
    if( !aNew ) return SQLITE_NOMEM;
    pWriter->aData = aNew;
    pWriter->nSize = nReq;
  }
  assert( nData+nReq<=pWriter->nSize );

  /* Append the prefix-compressed term and doclist to the buffer. */
  nData += sqlite3Fts3PutVarint(&pWriter->aData[nData], nPrefix);
  nData += sqlite3Fts3PutVarint(&pWriter->aData[nData], nSuffix);
  memcpy(&pWriter->aData[nData], &zTerm[nPrefix], nSuffix);
  nData += nSuffix;
  nData += sqlite3Fts3PutVarint(&pWriter->aData[nData], nDoclist);
  memcpy(&pWriter->aData[nData], aDoclist, nDoclist);
  pWriter->nData = nData + nDoclist;

  /* Save the current term so that it can be used to prefix-compress the next.
  ** If the isCopyTerm parameter is true, then the buffer pointed to by
  ** zTerm is transient, so take a copy of the term data. Otherwise, just
  ** store a copy of the pointer.
  */
  if( isCopyTerm ){
    if( nTerm>pWriter->nMalloc ){
      char *zNew = sqlite3_realloc(pWriter->zMalloc, nTerm*2);
      if( !zNew ){
        return SQLITE_NOMEM;
      }
      pWriter->nMalloc = nTerm*2;
      pWriter->zMalloc = zNew;
      pWriter->zTerm = zNew;
    }
    assert( pWriter->zTerm==pWriter->zMalloc );
    memcpy(pWriter->zTerm, zTerm, nTerm);
  }else{
    pWriter->zTerm = (char *)zTerm;
  }
  pWriter->nTerm = nTerm;

  return SQLITE_OK;
}

/*
** Flush all data associated with the SegmentWriter object pWriter to the
** database. This function must be called after all terms have been added
** to the segment using fts3SegWriterAdd(). If successful, SQLITE_OK is
** returned. Otherwise, an SQLite error code.
*/
static int fts3SegWriterFlush(
  Fts3Table *p,                   /* Virtual table handle */
  SegmentWriter *pWriter,         /* SegmentWriter to flush to the db */
  sqlite3_int64 iLevel,           /* Value for 'level' column of %_segdir */
  int iIdx                        /* Value for 'idx' column of %_segdir */
){
  int rc;                         /* Return code */
  if( pWriter->pTree ){
    sqlite3_int64 iLast = 0;      /* Largest block id written to database */
    sqlite3_int64 iLastLeaf;      /* Largest leaf block id written to db */
    char *zRoot = NULL;           /* Pointer to buffer containing root node */
    int nRoot = 0;                /* Size of buffer zRoot */

    iLastLeaf = pWriter->iFree;
    rc = fts3WriteSegment(p, pWriter->iFree++, pWriter->aData, pWriter->nData);
    if( rc==SQLITE_OK ){
      rc = fts3NodeWrite(p, pWriter->pTree, 1,
          pWriter->iFirst, pWriter->iFree, &iLast, &zRoot, &nRoot);
    }
    if( rc==SQLITE_OK ){
      rc = fts3WriteSegdir(p, iLevel, iIdx, 
          pWriter->iFirst, iLastLeaf, iLast, pWriter->nLeafData, zRoot, nRoot);
    }
  }else{
    /* The entire tree fits on the root node. Write it to the segdir table. */
    rc = fts3WriteSegdir(p, iLevel, iIdx, 
        0, 0, 0, pWriter->nLeafData, pWriter->aData, pWriter->nData);
  }
  p->nLeafAdd++;
  return rc;
}

/*
** Release all memory held by the SegmentWriter object passed as the 
** first argument.
*/
static void fts3SegWriterFree(SegmentWriter *pWriter){
  if( pWriter ){
    sqlite3_free(pWriter->aData);
    sqlite3_free(pWriter->zMalloc);
    fts3NodeFree(pWriter->pTree);
    sqlite3_free(pWriter);
  }
}

/*
** The first value in the apVal[] array is assumed to contain an integer.
** This function tests if there exist any documents with docid values that
** are different from that integer. i.e. if deleting the document with docid
** pRowid would mean the FTS3 table were empty.
**
** If successful, *pisEmpty is set to true if the table is empty except for
** document pRowid, or false otherwise, and SQLITE_OK is returned. If an
** error occurs, an SQLite error code is returned.
*/
static int fts3IsEmpty(Fts3Table *p, sqlite3_value *pRowid, int *pisEmpty){
  sqlite3_stmt *pStmt;
  int rc;
  if( p->zContentTbl ){
    /* If using the content=xxx option, assume the table is never empty */
    *pisEmpty = 0;
    rc = SQLITE_OK;
  }else{
    rc = fts3SqlStmt(p, SQL_IS_EMPTY, &pStmt, &pRowid);
    if( rc==SQLITE_OK ){
      if( SQLITE_ROW==sqlite3_step(pStmt) ){
        *pisEmpty = sqlite3_column_int(pStmt, 0);
      }
      rc = sqlite3_reset(pStmt);
    }
  }
  return rc;
}

/*
** Set *pnMax to the largest segment level in the database for the index
** iIndex.
**
** Segment levels are stored in the 'level' column of the %_segdir table.
**
** Return SQLITE_OK if successful, or an SQLite error code if not.
*/
static int fts3SegmentMaxLevel(
  Fts3Table *p, 
  int iLangid,
  int iIndex, 
  sqlite3_int64 *pnMax
){
  sqlite3_stmt *pStmt;
  int rc;
  assert( iIndex>=0 && iIndex<p->nIndex );

  /* Set pStmt to the compiled version of:
  **
  **   SELECT max(level) FROM %Q.'%q_segdir' WHERE level BETWEEN ? AND ?
  **
  ** (1024 is actually the value of macro FTS3_SEGDIR_PREFIXLEVEL_STR).
  */
  rc = fts3SqlStmt(p, SQL_SELECT_SEGDIR_MAX_LEVEL, &pStmt, 0);
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_int64(pStmt, 1, getAbsoluteLevel(p, iLangid, iIndex, 0));
  sqlite3_bind_int64(pStmt, 2, 
      getAbsoluteLevel(p, iLangid, iIndex, FTS3_SEGDIR_MAXLEVEL-1)
  );
  if( SQLITE_ROW==sqlite3_step(pStmt) ){
    *pnMax = sqlite3_column_int64(pStmt, 0);
  }
  return sqlite3_reset(pStmt);
}

/*
** iAbsLevel is an absolute level that may be assumed to exist within
** the database. This function checks if it is the largest level number
** within its index. Assuming no error occurs, *pbMax is set to 1 if
** iAbsLevel is indeed the largest level, or 0 otherwise, and SQLITE_OK
** is returned. If an error occurs, an error code is returned and the
** final value of *pbMax is undefined.
*/
static int fts3SegmentIsMaxLevel(Fts3Table *p, i64 iAbsLevel, int *pbMax){

  /* Set pStmt to the compiled version of:
  **
  **   SELECT max(level) FROM %Q.'%q_segdir' WHERE level BETWEEN ? AND ?
  **
  ** (1024 is actually the value of macro FTS3_SEGDIR_PREFIXLEVEL_STR).
  */
  sqlite3_stmt *pStmt;
  int rc = fts3SqlStmt(p, SQL_SELECT_SEGDIR_MAX_LEVEL, &pStmt, 0);
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_int64(pStmt, 1, iAbsLevel+1);
  sqlite3_bind_int64(pStmt, 2, 
      ((iAbsLevel/FTS3_SEGDIR_MAXLEVEL)+1) * FTS3_SEGDIR_MAXLEVEL
  );

  *pbMax = 0;
  if( SQLITE_ROW==sqlite3_step(pStmt) ){
    *pbMax = sqlite3_column_type(pStmt, 0)==SQLITE_NULL;
  }
  return sqlite3_reset(pStmt);
}

/*
** Delete all entries in the %_segments table associated with the segment
** opened with seg-reader pSeg. This function does not affect the contents
** of the %_segdir table.
*/
static int fts3DeleteSegment(
  Fts3Table *p,                   /* FTS table handle */
  Fts3SegReader *pSeg             /* Segment to delete */
){
  int rc = SQLITE_OK;             /* Return code */
  if( pSeg->iStartBlock ){
    sqlite3_stmt *pDelete;        /* SQL statement to delete rows */
    rc = fts3SqlStmt(p, SQL_DELETE_SEGMENTS_RANGE, &pDelete, 0);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(pDelete, 1, pSeg->iStartBlock);
      sqlite3_bind_int64(pDelete, 2, pSeg->iEndBlock);
      sqlite3_step(pDelete);
      rc = sqlite3_reset(pDelete);
    }
  }
  return rc;
}

/*
** This function is used after merging multiple segments into a single large
** segment to delete the old, now redundant, segment b-trees. Specifically,
** it:
** 
**   1) Deletes all %_segments entries for the segments associated with 
**      each of the SegReader objects in the array passed as the third 
**      argument, and
**
**   2) deletes all %_segdir entries with level iLevel, or all %_segdir
**      entries regardless of level if (iLevel<0).
**
** SQLITE_OK is returned if successful, otherwise an SQLite error code.
*/
static int fts3DeleteSegdir(
  Fts3Table *p,                   /* Virtual table handle */
  int iLangid,                    /* Language id */
  int iIndex,                     /* Index for p->aIndex */
  int iLevel,                     /* Level of %_segdir entries to delete */
  Fts3SegReader **apSegment,      /* Array of SegReader objects */
  int nReader                     /* Size of array apSegment */
){
  int rc = SQLITE_OK;             /* Return Code */
  int i;                          /* Iterator variable */
  sqlite3_stmt *pDelete = 0;      /* SQL statement to delete rows */

  for(i=0; rc==SQLITE_OK && i<nReader; i++){
    rc = fts3DeleteSegment(p, apSegment[i]);
  }
  if( rc!=SQLITE_OK ){
    return rc;
  }

  assert( iLevel>=0 || iLevel==FTS3_SEGCURSOR_ALL );
  if( iLevel==FTS3_SEGCURSOR_ALL ){
    rc = fts3SqlStmt(p, SQL_DELETE_SEGDIR_RANGE, &pDelete, 0);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(pDelete, 1, getAbsoluteLevel(p, iLangid, iIndex, 0));
      sqlite3_bind_int64(pDelete, 2, 
          getAbsoluteLevel(p, iLangid, iIndex, FTS3_SEGDIR_MAXLEVEL-1)
      );
    }
  }else{
    rc = fts3SqlStmt(p, SQL_DELETE_SEGDIR_LEVEL, &pDelete, 0);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(
          pDelete, 1, getAbsoluteLevel(p, iLangid, iIndex, iLevel)
      );
    }
  }

  if( rc==SQLITE_OK ){
    sqlite3_step(pDelete);
    rc = sqlite3_reset(pDelete);
  }

  return rc;
}

/*
** When this function is called, buffer *ppList (size *pnList bytes) contains 
** a position list that may (or may not) feature multiple columns. This
** function adjusts the pointer *ppList and the length *pnList so that they
** identify the subset of the position list that corresponds to column iCol.
**
** If there are no entries in the input position list for column iCol, then
** *pnList is set to zero before returning.
**
** If parameter bZero is non-zero, then any part of the input list following
** the end of the output list is zeroed before returning.
*/
static void fts3ColumnFilter(
  int iCol,                       /* Column to filter on */
  int bZero,                      /* Zero out anything following *ppList */
  char **ppList,                  /* IN/OUT: Pointer to position list */
  int *pnList                     /* IN/OUT: Size of buffer *ppList in bytes */
){
  char *pList = *ppList;
  int nList = *pnList;
  char *pEnd = &pList[nList];
  int iCurrent = 0;
  char *p = pList;

  assert( iCol>=0 );
  while( 1 ){
    char c = 0;
    while( p<pEnd && (c | *p)&0xFE ) c = *p++ & 0x80;
  
    if( iCol==iCurrent ){
      nList = (int)(p - pList);
      break;
    }

    nList -= (int)(p - pList);
    pList = p;
    if( nList==0 ){
      break;
    }
    p = &pList[1];
    p += fts3GetVarint32(p, &iCurrent);
  }

  if( bZero && &pList[nList]!=pEnd ){
    memset(&pList[nList], 0, pEnd - &pList[nList]);
  }
  *ppList = pList;
  *pnList = nList;
}

/*
** Cache data in the Fts3MultiSegReader.aBuffer[] buffer (overwriting any
** existing data). Grow the buffer if required.
**
** If successful, return SQLITE_OK. Otherwise, if an OOM error is encountered
** trying to resize the buffer, return SQLITE_NOMEM.
*/
static int fts3MsrBufferData(
  Fts3MultiSegReader *pMsr,       /* Multi-segment-reader handle */
  char *pList,
  int nList
){
  if( nList>pMsr->nBuffer ){
    char *pNew;
    pMsr->nBuffer = nList*2;
    pNew = (char *)sqlite3_realloc(pMsr->aBuffer, pMsr->nBuffer);
    if( !pNew ) return SQLITE_NOMEM;
    pMsr->aBuffer = pNew;
  }

  memcpy(pMsr->aBuffer, pList, nList);
  return SQLITE_OK;
}

SQLITE_PRIVATE int sqlite3Fts3MsrIncrNext(
  Fts3Table *p,                   /* Virtual table handle */
  Fts3MultiSegReader *pMsr,       /* Multi-segment-reader handle */
  sqlite3_int64 *piDocid,         /* OUT: Docid value */
  char **paPoslist,               /* OUT: Pointer to position list */
  int *pnPoslist                  /* OUT: Size of position list in bytes */
){
  int nMerge = pMsr->nAdvance;
  Fts3SegReader **apSegment = pMsr->apSegment;
  int (*xCmp)(Fts3SegReader *, Fts3SegReader *) = (
    p->bDescIdx ? fts3SegReaderDoclistCmpRev : fts3SegReaderDoclistCmp
  );

  if( nMerge==0 ){
    *paPoslist = 0;
    return SQLITE_OK;
  }

  while( 1 ){
    Fts3SegReader *pSeg;
    pSeg = pMsr->apSegment[0];

    if( pSeg->pOffsetList==0 ){
      *paPoslist = 0;
      break;
    }else{
      int rc;
      char *pList;
      int nList;
      int j;
      sqlite3_int64 iDocid = apSegment[0]->iDocid;

      rc = fts3SegReaderNextDocid(p, apSegment[0], &pList, &nList);
      j = 1;
      while( rc==SQLITE_OK 
        && j<nMerge
        && apSegment[j]->pOffsetList
        && apSegment[j]->iDocid==iDocid
      ){
        rc = fts3SegReaderNextDocid(p, apSegment[j], 0, 0);
        j++;
      }
      if( rc!=SQLITE_OK ) return rc;
      fts3SegReaderSort(pMsr->apSegment, nMerge, j, xCmp);

      if( nList>0 && fts3SegReaderIsPending(apSegment[0]) ){
        rc = fts3MsrBufferData(pMsr, pList, nList+1);
        if( rc!=SQLITE_OK ) return rc;
        assert( (pMsr->aBuffer[nList] & 0xFE)==0x00 );
        pList = pMsr->aBuffer;
      }

      if( pMsr->iColFilter>=0 ){
        fts3ColumnFilter(pMsr->iColFilter, 1, &pList, &nList);
      }

      if( nList>0 ){
        *paPoslist = pList;
        *piDocid = iDocid;
        *pnPoslist = nList;
        break;
      }
    }
  }

  return SQLITE_OK;
}

static int fts3SegReaderStart(
  Fts3Table *p,                   /* Virtual table handle */
  Fts3MultiSegReader *pCsr,       /* Cursor object */
  const char *zTerm,              /* Term searched for (or NULL) */
  int nTerm                       /* Length of zTerm in bytes */
){
  int i;
  int nSeg = pCsr->nSegment;

  /* If the Fts3SegFilter defines a specific term (or term prefix) to search 
  ** for, then advance each segment iterator until it points to a term of
  ** equal or greater value than the specified term. This prevents many
  ** unnecessary merge/sort operations for the case where single segment
  ** b-tree leaf nodes contain more than one term.
  */
  for(i=0; pCsr->bRestart==0 && i<pCsr->nSegment; i++){
    int res = 0;
    Fts3SegReader *pSeg = pCsr->apSegment[i];
    do {
      int rc = fts3SegReaderNext(p, pSeg, 0);
      if( rc!=SQLITE_OK ) return rc;
    }while( zTerm && (res = fts3SegReaderTermCmp(pSeg, zTerm, nTerm))<0 );

    if( pSeg->bLookup && res!=0 ){
      fts3SegReaderSetEof(pSeg);
    }
  }
  fts3SegReaderSort(pCsr->apSegment, nSeg, nSeg, fts3SegReaderCmp);

  return SQLITE_OK;
}

SQLITE_PRIVATE int sqlite3Fts3SegReaderStart(
  Fts3Table *p,                   /* Virtual table handle */
  Fts3MultiSegReader *pCsr,       /* Cursor object */
  Fts3SegFilter *pFilter          /* Restrictions on range of iteration */
){
  pCsr->pFilter = pFilter;
  return fts3SegReaderStart(p, pCsr, pFilter->zTerm, pFilter->nTerm);
}

SQLITE_PRIVATE int sqlite3Fts3MsrIncrStart(
  Fts3Table *p,                   /* Virtual table handle */
  Fts3MultiSegReader *pCsr,       /* Cursor object */
  int iCol,                       /* Column to match on. */
  const char *zTerm,              /* Term to iterate through a doclist for */
  int nTerm                       /* Number of bytes in zTerm */
){
  int i;
  int rc;
  int nSegment = pCsr->nSegment;
  int (*xCmp)(Fts3SegReader *, Fts3SegReader *) = (
    p->bDescIdx ? fts3SegReaderDoclistCmpRev : fts3SegReaderDoclistCmp
  );

  assert( pCsr->pFilter==0 );
  assert( zTerm && nTerm>0 );

  /* Advance each segment iterator until it points to the term zTerm/nTerm. */
  rc = fts3SegReaderStart(p, pCsr, zTerm, nTerm);
  if( rc!=SQLITE_OK ) return rc;

  /* Determine how many of the segments actually point to zTerm/nTerm. */
  for(i=0; i<nSegment; i++){
    Fts3SegReader *pSeg = pCsr->apSegment[i];
    if( !pSeg->aNode || fts3SegReaderTermCmp(pSeg, zTerm, nTerm) ){
      break;
    }
  }
  pCsr->nAdvance = i;

  /* Advance each of the segments to point to the first docid. */
  for(i=0; i<pCsr->nAdvance; i++){
    rc = fts3SegReaderFirstDocid(p, pCsr->apSegment[i]);
    if( rc!=SQLITE_OK ) return rc;
  }
  fts3SegReaderSort(pCsr->apSegment, i, i, xCmp);

  assert( iCol<0 || iCol<p->nColumn );
  pCsr->iColFilter = iCol;

  return SQLITE_OK;
}

/*
** This function is called on a MultiSegReader that has been started using
** sqlite3Fts3MsrIncrStart(). One or more calls to MsrIncrNext() may also
** have been made. Calling this function puts the MultiSegReader in such
** a state that if the next two calls are:
**
**   sqlite3Fts3SegReaderStart()
**   sqlite3Fts3SegReaderStep()
**
** then the entire doclist for the term is available in 
** MultiSegReader.aDoclist/nDoclist.
*/
SQLITE_PRIVATE int sqlite3Fts3MsrIncrRestart(Fts3MultiSegReader *pCsr){
  int i;                          /* Used to iterate through segment-readers */

  assert( pCsr->zTerm==0 );
  assert( pCsr->nTerm==0 );
  assert( pCsr->aDoclist==0 );
  assert( pCsr->nDoclist==0 );

  pCsr->nAdvance = 0;
  pCsr->bRestart = 1;
  for(i=0; i<pCsr->nSegment; i++){
    pCsr->apSegment[i]->pOffsetList = 0;
    pCsr->apSegment[i]->nOffsetList = 0;
    pCsr->apSegment[i]->iDocid = 0;
  }

  return SQLITE_OK;
}


SQLITE_PRIVATE int sqlite3Fts3SegReaderStep(
  Fts3Table *p,                   /* Virtual table handle */
  Fts3MultiSegReader *pCsr        /* Cursor object */
){
  int rc = SQLITE_OK;

  int isIgnoreEmpty =  (pCsr->pFilter->flags & FTS3_SEGMENT_IGNORE_EMPTY);
  int isRequirePos =   (pCsr->pFilter->flags & FTS3_SEGMENT_REQUIRE_POS);
  int isColFilter =    (pCsr->pFilter->flags & FTS3_SEGMENT_COLUMN_FILTER);
  int isPrefix =       (pCsr->pFilter->flags & FTS3_SEGMENT_PREFIX);
  int isScan =         (pCsr->pFilter->flags & FTS3_SEGMENT_SCAN);
  int isFirst =        (pCsr->pFilter->flags & FTS3_SEGMENT_FIRST);

  Fts3SegReader **apSegment = pCsr->apSegment;
  int nSegment = pCsr->nSegment;
  Fts3SegFilter *pFilter = pCsr->pFilter;
  int (*xCmp)(Fts3SegReader *, Fts3SegReader *) = (
    p->bDescIdx ? fts3SegReaderDoclistCmpRev : fts3SegReaderDoclistCmp
  );

  if( pCsr->nSegment==0 ) return SQLITE_OK;

  do {
    int nMerge;
    int i;
  
    /* Advance the first pCsr->nAdvance entries in the apSegment[] array
    ** forward. Then sort the list in order of current term again.  
    */
    for(i=0; i<pCsr->nAdvance; i++){
      Fts3SegReader *pSeg = apSegment[i];
      if( pSeg->bLookup ){
        fts3SegReaderSetEof(pSeg);
      }else{
        rc = fts3SegReaderNext(p, pSeg, 0);
      }
      if( rc!=SQLITE_OK ) return rc;
    }
    fts3SegReaderSort(apSegment, nSegment, pCsr->nAdvance, fts3SegReaderCmp);
    pCsr->nAdvance = 0;

    /* If all the seg-readers are at EOF, we're finished. return SQLITE_OK. */
    assert( rc==SQLITE_OK );
    if( apSegment[0]->aNode==0 ) break;

    pCsr->nTerm = apSegment[0]->nTerm;
    pCsr->zTerm = apSegment[0]->zTerm;

    /* If this is a prefix-search, and if the term that apSegment[0] points
    ** to does not share a suffix with pFilter->zTerm/nTerm, then all 
    ** required callbacks have been made. In this case exit early.
    **
    ** Similarly, if this is a search for an exact match, and the first term
    ** of segment apSegment[0] is not a match, exit early.
    */
    if( pFilter->zTerm && !isScan ){
      if( pCsr->nTerm<pFilter->nTerm 
       || (!isPrefix && pCsr->nTerm>pFilter->nTerm)
       || memcmp(pCsr->zTerm, pFilter->zTerm, pFilter->nTerm) 
      ){
        break;
      }
    }

    nMerge = 1;
    while( nMerge<nSegment 
        && apSegment[nMerge]->aNode
        && apSegment[nMerge]->nTerm==pCsr->nTerm 
        && 0==memcmp(pCsr->zTerm, apSegment[nMerge]->zTerm, pCsr->nTerm)
    ){
      nMerge++;
    }

    assert( isIgnoreEmpty || (isRequirePos && !isColFilter) );
    if( nMerge==1 
     && !isIgnoreEmpty 
     && !isFirst 
     && (p->bDescIdx==0 || fts3SegReaderIsPending(apSegment[0])==0)
    ){
      pCsr->nDoclist = apSegment[0]->nDoclist;
      if( fts3SegReaderIsPending(apSegment[0]) ){
        rc = fts3MsrBufferData(pCsr, apSegment[0]->aDoclist, pCsr->nDoclist);
        pCsr->aDoclist = pCsr->aBuffer;
      }else{
        pCsr->aDoclist = apSegment[0]->aDoclist;
      }
      if( rc==SQLITE_OK ) rc = SQLITE_ROW;
    }else{
      int nDoclist = 0;           /* Size of doclist */
      sqlite3_int64 iPrev = 0;    /* Previous docid stored in doclist */

      /* The current term of the first nMerge entries in the array
      ** of Fts3SegReader objects is the same. The doclists must be merged
      ** and a single term returned with the merged doclist.
      */
      for(i=0; i<nMerge; i++){
        fts3SegReaderFirstDocid(p, apSegment[i]);
      }
      fts3SegReaderSort(apSegment, nMerge, nMerge, xCmp);
      while( apSegment[0]->pOffsetList ){
        int j;                    /* Number of segments that share a docid */
        char *pList = 0;
        int nList = 0;
        int nByte;
        sqlite3_int64 iDocid = apSegment[0]->iDocid;
        fts3SegReaderNextDocid(p, apSegment[0], &pList, &nList);
        j = 1;
        while( j<nMerge
            && apSegment[j]->pOffsetList
            && apSegment[j]->iDocid==iDocid
        ){
          fts3SegReaderNextDocid(p, apSegment[j], 0, 0);
          j++;
        }

        if( isColFilter ){
          fts3ColumnFilter(pFilter->iCol, 0, &pList, &nList);
        }

        if( !isIgnoreEmpty || nList>0 ){

          /* Calculate the 'docid' delta value to write into the merged 
          ** doclist. */
          sqlite3_int64 iDelta;
          if( p->bDescIdx && nDoclist>0 ){
            iDelta = iPrev - iDocid;
          }else{
            iDelta = iDocid - iPrev;
          }
          assert( iDelta>0 || (nDoclist==0 && iDelta==iDocid) );
          assert( nDoclist>0 || iDelta==iDocid );

          nByte = sqlite3Fts3VarintLen(iDelta) + (isRequirePos?nList+1:0);
          if( nDoclist+nByte>pCsr->nBuffer ){
            char *aNew;
            pCsr->nBuffer = (nDoclist+nByte)*2;
            aNew = sqlite3_realloc(pCsr->aBuffer, pCsr->nBuffer);
            if( !aNew ){
              return SQLITE_NOMEM;
            }
            pCsr->aBuffer = aNew;
          }

          if( isFirst ){
            char *a = &pCsr->aBuffer[nDoclist];
            int nWrite;
           
            nWrite = sqlite3Fts3FirstFilter(iDelta, pList, nList, a);
            if( nWrite ){
              iPrev = iDocid;
              nDoclist += nWrite;
            }
          }else{
            nDoclist += sqlite3Fts3PutVarint(&pCsr->aBuffer[nDoclist], iDelta);
            iPrev = iDocid;
            if( isRequirePos ){
              memcpy(&pCsr->aBuffer[nDoclist], pList, nList);
              nDoclist += nList;
              pCsr->aBuffer[nDoclist++] = '\0';
            }
          }
        }

        fts3SegReaderSort(apSegment, nMerge, j, xCmp);
      }
      if( nDoclist>0 ){
        pCsr->aDoclist = pCsr->aBuffer;
        pCsr->nDoclist = nDoclist;
        rc = SQLITE_ROW;
      }
    }
    pCsr->nAdvance = nMerge;
  }while( rc==SQLITE_OK );

  return rc;
}


SQLITE_PRIVATE void sqlite3Fts3SegReaderFinish(
  Fts3MultiSegReader *pCsr       /* Cursor object */
){
  if( pCsr ){
    int i;
    for(i=0; i<pCsr->nSegment; i++){
      sqlite3Fts3SegReaderFree(pCsr->apSegment[i]);
    }
    sqlite3_free(pCsr->apSegment);
    sqlite3_free(pCsr->aBuffer);

    pCsr->nSegment = 0;
    pCsr->apSegment = 0;
    pCsr->aBuffer = 0;
  }
}

/*
** Decode the "end_block" field, selected by column iCol of the SELECT 
** statement passed as the first argument. 
**
** The "end_block" field may contain either an integer, or a text field
** containing the text representation of two non-negative integers separated 
** by one or more space (0x20) characters. In the first case, set *piEndBlock 
** to the integer value and *pnByte to zero before returning. In the second, 
** set *piEndBlock to the first value and *pnByte to the second.
*/
static void fts3ReadEndBlockField(
  sqlite3_stmt *pStmt, 
  int iCol, 
  i64 *piEndBlock,
  i64 *pnByte
){
  const unsigned char *zText = sqlite3_column_text(pStmt, iCol);
  if( zText ){
    int i;
    int iMul = 1;
    i64 iVal = 0;
    for(i=0; zText[i]>='0' && zText[i]<='9'; i++){
      iVal = iVal*10 + (zText[i] - '0');
    }
    *piEndBlock = iVal;
    while( zText[i]==' ' ) i++;
    iVal = 0;
    if( zText[i]=='-' ){
      i++;
      iMul = -1;
    }
    for(/* no-op */; zText[i]>='0' && zText[i]<='9'; i++){
      iVal = iVal*10 + (zText[i] - '0');
    }
    *pnByte = (iVal * (i64)iMul);
  }
}


/*
** A segment of size nByte bytes has just been written to absolute level
** iAbsLevel. Promote any segments that should be promoted as a result.
*/
static int fts3PromoteSegments(
  Fts3Table *p,                   /* FTS table handle */
  sqlite3_int64 iAbsLevel,        /* Absolute level just updated */
  sqlite3_int64 nByte             /* Size of new segment at iAbsLevel */
){
  int rc = SQLITE_OK;
  sqlite3_stmt *pRange;

  rc = fts3SqlStmt(p, SQL_SELECT_LEVEL_RANGE2, &pRange, 0);

  if( rc==SQLITE_OK ){
    int bOk = 0;
    i64 iLast = (iAbsLevel/FTS3_SEGDIR_MAXLEVEL + 1) * FTS3_SEGDIR_MAXLEVEL - 1;
    i64 nLimit = (nByte*3)/2;

    /* Loop through all entries in the %_segdir table corresponding to 
    ** segments in this index on levels greater than iAbsLevel. If there is
    ** at least one such segment, and it is possible to determine that all 
    ** such segments are smaller than nLimit bytes in size, they will be 
    ** promoted to level iAbsLevel.  */
    sqlite3_bind_int64(pRange, 1, iAbsLevel+1);
    sqlite3_bind_int64(pRange, 2, iLast);
    while( SQLITE_ROW==sqlite3_step(pRange) ){
      i64 nSize = 0, dummy;
      fts3ReadEndBlockField(pRange, 2, &dummy, &nSize);
      if( nSize<=0 || nSize>nLimit ){
        /* If nSize==0, then the %_segdir.end_block field does not not 
        ** contain a size value. This happens if it was written by an
        ** old version of FTS. In this case it is not possible to determine
        ** the size of the segment, and so segment promotion does not
        ** take place.  */
        bOk = 0;
        break;
      }
      bOk = 1;
    }
    rc = sqlite3_reset(pRange);

    if( bOk ){
      int iIdx = 0;
      sqlite3_stmt *pUpdate1;
      sqlite3_stmt *pUpdate2;

      if( rc==SQLITE_OK ){
        rc = fts3SqlStmt(p, SQL_UPDATE_LEVEL_IDX, &pUpdate1, 0);
      }
      if( rc==SQLITE_OK ){
        rc = fts3SqlStmt(p, SQL_UPDATE_LEVEL, &pUpdate2, 0);
      }

      if( rc==SQLITE_OK ){

        /* Loop through all %_segdir entries for segments in this index with
        ** levels equal to or greater than iAbsLevel. As each entry is visited,
        ** updated it to set (level = -1) and (idx = N), where N is 0 for the
        ** oldest segment in the range, 1 for the next oldest, and so on.
        **
        ** In other words, move all segments being promoted to level -1,
        ** setting the "idx" fields as appropriate to keep them in the same
        ** order. The contents of level -1 (which is never used, except
        ** transiently here), will be moved back to level iAbsLevel below.  */
        sqlite3_bind_int64(pRange, 1, iAbsLevel);
        while( SQLITE_ROW==sqlite3_step(pRange) ){
          sqlite3_bind_int(pUpdate1, 1, iIdx++);
          sqlite3_bind_int(pUpdate1, 2, sqlite3_column_int(pRange, 0));
          sqlite3_bind_int(pUpdate1, 3, sqlite3_column_int(pRange, 1));
          sqlite3_step(pUpdate1);
          rc = sqlite3_reset(pUpdate1);
          if( rc!=SQLITE_OK ){
            sqlite3_reset(pRange);
            break;
          }
        }
      }
      if( rc==SQLITE_OK ){
        rc = sqlite3_reset(pRange);
      }

      /* Move level -1 to level iAbsLevel */
      if( rc==SQLITE_OK ){
        sqlite3_bind_int64(pUpdate2, 1, iAbsLevel);
        sqlite3_step(pUpdate2);
        rc = sqlite3_reset(pUpdate2);
      }
    }
  }


  return rc;
}

/*
** Merge all level iLevel segments in the database into a single 
** iLevel+1 segment. Or, if iLevel<0, merge all segments into a
** single segment with a level equal to the numerically largest level 
** currently present in the database.
**
** If this function is called with iLevel<0, but there is only one
** segment in the database, SQLITE_DONE is returned immediately. 
** Otherwise, if successful, SQLITE_OK is returned. If an error occurs, 
** an SQLite error code is returned.
*/
static int fts3SegmentMerge(
  Fts3Table *p, 
  int iLangid,                    /* Language id to merge */
  int iIndex,                     /* Index in p->aIndex[] to merge */
  int iLevel                      /* Level to merge */
){
  int rc;                         /* Return code */
  int iIdx = 0;                   /* Index of new segment */
  sqlite3_int64 iNewLevel = 0;    /* Level/index to create new segment at */
  SegmentWriter *pWriter = 0;     /* Used to write the new, merged, segment */
  Fts3SegFilter filter;           /* Segment term filter condition */
  Fts3MultiSegReader csr;         /* Cursor to iterate through level(s) */
  int bIgnoreEmpty = 0;           /* True to ignore empty segments */
  i64 iMaxLevel = 0;              /* Max level number for this index/langid */

  assert( iLevel==FTS3_SEGCURSOR_ALL
       || iLevel==FTS3_SEGCURSOR_PENDING
       || iLevel>=0
  );
  assert( iLevel<FTS3_SEGDIR_MAXLEVEL );
  assert( iIndex>=0 && iIndex<p->nIndex );

  rc = sqlite3Fts3SegReaderCursor(p, iLangid, iIndex, iLevel, 0, 0, 1, 0, &csr);
  if( rc!=SQLITE_OK || csr.nSegment==0 ) goto finished;

  if( iLevel!=FTS3_SEGCURSOR_PENDING ){
    rc = fts3SegmentMaxLevel(p, iLangid, iIndex, &iMaxLevel);
    if( rc!=SQLITE_OK ) goto finished;
  }

  if( iLevel==FTS3_SEGCURSOR_ALL ){
    /* This call is to merge all segments in the database to a single
    ** segment. The level of the new segment is equal to the numerically
    ** greatest segment level currently present in the database for this
    ** index. The idx of the new segment is always 0.  */
    if( csr.nSegment==1 ){
      rc = SQLITE_DONE;
      goto finished;
    }
    iNewLevel = iMaxLevel;
    bIgnoreEmpty = 1;

  }else{
    /* This call is to merge all segments at level iLevel. find the next
    ** available segment index at level iLevel+1. The call to
    ** fts3AllocateSegdirIdx() will merge the segments at level iLevel+1 to 
    ** a single iLevel+2 segment if necessary.  */
    assert( FTS3_SEGCURSOR_PENDING==-1 );
    iNewLevel = getAbsoluteLevel(p, iLangid, iIndex, iLevel+1);
    rc = fts3AllocateSegdirIdx(p, iLangid, iIndex, iLevel+1, &iIdx);
    bIgnoreEmpty = (iLevel!=FTS3_SEGCURSOR_PENDING) && (iNewLevel>iMaxLevel);
  }
  if( rc!=SQLITE_OK ) goto finished;

  assert( csr.nSegment>0 );
  assert( iNewLevel>=getAbsoluteLevel(p, iLangid, iIndex, 0) );
  assert( iNewLevel<getAbsoluteLevel(p, iLangid, iIndex,FTS3_SEGDIR_MAXLEVEL) );

  memset(&filter, 0, sizeof(Fts3SegFilter));
  filter.flags = FTS3_SEGMENT_REQUIRE_POS;
  filter.flags |= (bIgnoreEmpty ? FTS3_SEGMENT_IGNORE_EMPTY : 0);

  rc = sqlite3Fts3SegReaderStart(p, &csr, &filter);
  while( SQLITE_OK==rc ){
    rc = sqlite3Fts3SegReaderStep(p, &csr);
    if( rc!=SQLITE_ROW ) break;
    rc = fts3SegWriterAdd(p, &pWriter, 1, 
        csr.zTerm, csr.nTerm, csr.aDoclist, csr.nDoclist);
  }
  if( rc!=SQLITE_OK ) goto finished;
  assert( pWriter || bIgnoreEmpty );

  if( iLevel!=FTS3_SEGCURSOR_PENDING ){
    rc = fts3DeleteSegdir(
        p, iLangid, iIndex, iLevel, csr.apSegment, csr.nSegment
    );
    if( rc!=SQLITE_OK ) goto finished;
  }
  if( pWriter ){
    rc = fts3SegWriterFlush(p, pWriter, iNewLevel, iIdx);
    if( rc==SQLITE_OK ){
      if( iLevel==FTS3_SEGCURSOR_PENDING || iNewLevel<iMaxLevel ){
        rc = fts3PromoteSegments(p, iNewLevel, pWriter->nLeafData);
      }
    }
  }

 finished:
  fts3SegWriterFree(pWriter);
  sqlite3Fts3SegReaderFinish(&csr);
  return rc;
}


/* 
** Flush the contents of pendingTerms to level 0 segments. 
*/
SQLITE_PRIVATE int sqlite3Fts3PendingTermsFlush(Fts3Table *p){
  int rc = SQLITE_OK;
  int i;
        
  for(i=0; rc==SQLITE_OK && i<p->nIndex; i++){
    rc = fts3SegmentMerge(p, p->iPrevLangid, i, FTS3_SEGCURSOR_PENDING);
    if( rc==SQLITE_DONE ) rc = SQLITE_OK;
  }
  sqlite3Fts3PendingTermsClear(p);

  /* Determine the auto-incr-merge setting if unknown.  If enabled,
  ** estimate the number of leaf blocks of content to be written
  */
  if( rc==SQLITE_OK && p->bHasStat
   && p->nAutoincrmerge==0xff && p->nLeafAdd>0
  ){
    sqlite3_stmt *pStmt = 0;
    rc = fts3SqlStmt(p, SQL_SELECT_STAT, &pStmt, 0);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int(pStmt, 1, FTS_STAT_AUTOINCRMERGE);
      rc = sqlite3_step(pStmt);
      if( rc==SQLITE_ROW ){
        p->nAutoincrmerge = sqlite3_column_int(pStmt, 0);
        if( p->nAutoincrmerge==1 ) p->nAutoincrmerge = 8;
      }else if( rc==SQLITE_DONE ){
        p->nAutoincrmerge = 0;
      }
      rc = sqlite3_reset(pStmt);
    }
  }
  return rc;
}

/*
** Encode N integers as varints into a blob.
*/
static void fts3EncodeIntArray(
  int N,             /* The number of integers to encode */
  u32 *a,            /* The integer values */
  char *zBuf,        /* Write the BLOB here */
  int *pNBuf         /* Write number of bytes if zBuf[] used here */
){
  int i, j;
  for(i=j=0; i<N; i++){
    j += sqlite3Fts3PutVarint(&zBuf[j], (sqlite3_int64)a[i]);
  }
  *pNBuf = j;
}

/*
** Decode a blob of varints into N integers
*/
static void fts3DecodeIntArray(
  int N,             /* The number of integers to decode */
  u32 *a,            /* Write the integer values */
  const char *zBuf,  /* The BLOB containing the varints */
  int nBuf           /* size of the BLOB */
){
  int i, j;
  UNUSED_PARAMETER(nBuf);
  for(i=j=0; i<N; i++){
    sqlite3_int64 x;
    j += sqlite3Fts3GetVarint(&zBuf[j], &x);
    assert(j<=nBuf);
    a[i] = (u32)(x & 0xffffffff);
  }
}

/*
** Insert the sizes (in tokens) for each column of the document
** with docid equal to p->iPrevDocid.  The sizes are encoded as
** a blob of varints.
*/
static void fts3InsertDocsize(
  int *pRC,                       /* Result code */
  Fts3Table *p,                   /* Table into which to insert */
  u32 *aSz                        /* Sizes of each column, in tokens */
){
  char *pBlob;             /* The BLOB encoding of the document size */
  int nBlob;               /* Number of bytes in the BLOB */
  sqlite3_stmt *pStmt;     /* Statement used to insert the encoding */
  int rc;                  /* Result code from subfunctions */

  if( *pRC ) return;
  pBlob = sqlite3_malloc( 10*p->nColumn );
  if( pBlob==0 ){
    *pRC = SQLITE_NOMEM;
    return;
  }
  fts3EncodeIntArray(p->nColumn, aSz, pBlob, &nBlob);
  rc = fts3SqlStmt(p, SQL_REPLACE_DOCSIZE, &pStmt, 0);
  if( rc ){
    sqlite3_free(pBlob);
    *pRC = rc;
    return;
  }
  sqlite3_bind_int64(pStmt, 1, p->iPrevDocid);
  sqlite3_bind_blob(pStmt, 2, pBlob, nBlob, sqlite3_free);
  sqlite3_step(pStmt);
  *pRC = sqlite3_reset(pStmt);
}

/*
** Record 0 of the %_stat table contains a blob consisting of N varints,
** where N is the number of user defined columns in the fts3 table plus
** two. If nCol is the number of user defined columns, then values of the 
** varints are set as follows:
**
**   Varint 0:       Total number of rows in the table.
**
**   Varint 1..nCol: For each column, the total number of tokens stored in
**                   the column for all rows of the table.
**
**   Varint 1+nCol:  The total size, in bytes, of all text values in all
**                   columns of all rows of the table.
**
*/
static void fts3UpdateDocTotals(
  int *pRC,                       /* The result code */
  Fts3Table *p,                   /* Table being updated */
  u32 *aSzIns,                    /* Size increases */
  u32 *aSzDel,                    /* Size decreases */
  int nChng                       /* Change in the number of documents */
){
  char *pBlob;             /* Storage for BLOB written into %_stat */
  int nBlob;               /* Size of BLOB written into %_stat */
  u32 *a;                  /* Array of integers that becomes the BLOB */
  sqlite3_stmt *pStmt;     /* Statement for reading and writing */
  int i;                   /* Loop counter */
  int rc;                  /* Result code from subfunctions */

  const int nStat = p->nColumn+2;

  if( *pRC ) return;
  a = sqlite3_malloc( (sizeof(u32)+10)*nStat );
  if( a==0 ){
    *pRC = SQLITE_NOMEM;
    return;
  }
  pBlob = (char*)&a[nStat];
  rc = fts3SqlStmt(p, SQL_SELECT_STAT, &pStmt, 0);
  if( rc ){
    sqlite3_free(a);
    *pRC = rc;
    return;
  }
  sqlite3_bind_int(pStmt, 1, FTS_STAT_DOCTOTAL);
  if( sqlite3_step(pStmt)==SQLITE_ROW ){
    fts3DecodeIntArray(nStat, a,
         sqlite3_column_blob(pStmt, 0),
         sqlite3_column_bytes(pStmt, 0));
  }else{
    memset(a, 0, sizeof(u32)*(nStat) );
  }
  rc = sqlite3_reset(pStmt);
  if( rc!=SQLITE_OK ){
    sqlite3_free(a);
    *pRC = rc;
    return;
  }
  if( nChng<0 && a[0]<(u32)(-nChng) ){
    a[0] = 0;
  }else{
    a[0] += nChng;
  }
  for(i=0; i<p->nColumn+1; i++){
    u32 x = a[i+1];
    if( x+aSzIns[i] < aSzDel[i] ){
      x = 0;
    }else{
      x = x + aSzIns[i] - aSzDel[i];
    }
    a[i+1] = x;
  }
  fts3EncodeIntArray(nStat, a, pBlob, &nBlob);
  rc = fts3SqlStmt(p, SQL_REPLACE_STAT, &pStmt, 0);
  if( rc ){
    sqlite3_free(a);
    *pRC = rc;
    return;
  }
  sqlite3_bind_int(pStmt, 1, FTS_STAT_DOCTOTAL);
  sqlite3_bind_blob(pStmt, 2, pBlob, nBlob, SQLITE_STATIC);
  sqlite3_step(pStmt);
  *pRC = sqlite3_reset(pStmt);
  sqlite3_free(a);
}

/*
** Merge the entire database so that there is one segment for each 
** iIndex/iLangid combination.
*/
static int fts3DoOptimize(Fts3Table *p, int bReturnDone){
  int bSeenDone = 0;
  int rc;
  sqlite3_stmt *pAllLangid = 0;

  rc = fts3SqlStmt(p, SQL_SELECT_ALL_LANGID, &pAllLangid, 0);
  if( rc==SQLITE_OK ){
    int rc2;
    sqlite3_bind_int(pAllLangid, 1, p->nIndex);
    while( sqlite3_step(pAllLangid)==SQLITE_ROW ){
      int i;
      int iLangid = sqlite3_column_int(pAllLangid, 0);
      for(i=0; rc==SQLITE_OK && i<p->nIndex; i++){
        rc = fts3SegmentMerge(p, iLangid, i, FTS3_SEGCURSOR_ALL);
        if( rc==SQLITE_DONE ){
          bSeenDone = 1;
          rc = SQLITE_OK;
        }
      }
    }
    rc2 = sqlite3_reset(pAllLangid);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  sqlite3Fts3SegmentsClose(p);
  sqlite3Fts3PendingTermsClear(p);

  return (rc==SQLITE_OK && bReturnDone && bSeenDone) ? SQLITE_DONE : rc;
}

/*
** This function is called when the user executes the following statement:
**
**     INSERT INTO <tbl>(<tbl>) VALUES('rebuild');
**
** The entire FTS index is discarded and rebuilt. If the table is one 
** created using the content=xxx option, then the new index is based on
** the current contents of the xxx table. Otherwise, it is rebuilt based
** on the contents of the %_content table.
*/
static int fts3DoRebuild(Fts3Table *p){
  int rc;                         /* Return Code */

  rc = fts3DeleteAll(p, 0);
  if( rc==SQLITE_OK ){
    u32 *aSz = 0;
    u32 *aSzIns = 0;
    u32 *aSzDel = 0;
    sqlite3_stmt *pStmt = 0;
    int nEntry = 0;

    /* Compose and prepare an SQL statement to loop through the content table */
    char *zSql = sqlite3_mprintf("SELECT %s" , p->zReadExprlist);
    if( !zSql ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlite3_prepare_v2(p->db, zSql, -1, &pStmt, 0);
      sqlite3_free(zSql);
    }

    if( rc==SQLITE_OK ){
      int nByte = sizeof(u32) * (p->nColumn+1)*3;
      aSz = (u32 *)sqlite3_malloc(nByte);
      if( aSz==0 ){
        rc = SQLITE_NOMEM;
      }else{
        memset(aSz, 0, nByte);
        aSzIns = &aSz[p->nColumn+1];
        aSzDel = &aSzIns[p->nColumn+1];
      }
    }

    while( rc==SQLITE_OK && SQLITE_ROW==sqlite3_step(pStmt) ){
      int iCol;
      int iLangid = langidFromSelect(p, pStmt);
      rc = fts3PendingTermsDocid(p, iLangid, sqlite3_column_int64(pStmt, 0));
      memset(aSz, 0, sizeof(aSz[0]) * (p->nColumn+1));
      for(iCol=0; rc==SQLITE_OK && iCol<p->nColumn; iCol++){
        if( p->abNotindexed[iCol]==0 ){
          const char *z = (const char *) sqlite3_column_text(pStmt, iCol+1);
          rc = fts3PendingTermsAdd(p, iLangid, z, iCol, &aSz[iCol]);
          aSz[p->nColumn] += sqlite3_column_bytes(pStmt, iCol+1);
        }
      }
      if( p->bHasDocsize ){
        fts3InsertDocsize(&rc, p, aSz);
      }
      if( rc!=SQLITE_OK ){
        sqlite3_finalize(pStmt);
        pStmt = 0;
      }else{
        nEntry++;
        for(iCol=0; iCol<=p->nColumn; iCol++){
          aSzIns[iCol] += aSz[iCol];
        }
      }
    }
    if( p->bFts4 ){
      fts3UpdateDocTotals(&rc, p, aSzIns, aSzDel, nEntry);
    }
    sqlite3_free(aSz);

    if( pStmt ){
      int rc2 = sqlite3_finalize(pStmt);
      if( rc==SQLITE_OK ){
        rc = rc2;
      }
    }
  }

  return rc;
}


/*
** This function opens a cursor used to read the input data for an 
** incremental merge operation. Specifically, it opens a cursor to scan
** the oldest nSeg segments (idx=0 through idx=(nSeg-1)) in absolute 
** level iAbsLevel.
*/
static int fts3IncrmergeCsr(
  Fts3Table *p,                   /* FTS3 table handle */
  sqlite3_int64 iAbsLevel,        /* Absolute level to open */
  int nSeg,                       /* Number of segments to merge */
  Fts3MultiSegReader *pCsr        /* Cursor object to populate */
){
  int rc;                         /* Return Code */
  sqlite3_stmt *pStmt = 0;        /* Statement used to read %_segdir entry */  
  int nByte;                      /* Bytes allocated at pCsr->apSegment[] */

  /* Allocate space for the Fts3MultiSegReader.aCsr[] array */
  memset(pCsr, 0, sizeof(*pCsr));
  nByte = sizeof(Fts3SegReader *) * nSeg;
  pCsr->apSegment = (Fts3SegReader **)sqlite3_malloc(nByte);

  if( pCsr->apSegment==0 ){
    rc = SQLITE_NOMEM;
  }else{
    memset(pCsr->apSegment, 0, nByte);
    rc = fts3SqlStmt(p, SQL_SELECT_LEVEL, &pStmt, 0);
  }
  if( rc==SQLITE_OK ){
    int i;
    int rc2;
    sqlite3_bind_int64(pStmt, 1, iAbsLevel);
    assert( pCsr->nSegment==0 );
    for(i=0; rc==SQLITE_OK && sqlite3_step(pStmt)==SQLITE_ROW && i<nSeg; i++){
      rc = sqlite3Fts3SegReaderNew(i, 0,
          sqlite3_column_int64(pStmt, 1),        /* segdir.start_block */
          sqlite3_column_int64(pStmt, 2),        /* segdir.leaves_end_block */
          sqlite3_column_int64(pStmt, 3),        /* segdir.end_block */
          sqlite3_column_blob(pStmt, 4),         /* segdir.root */
          sqlite3_column_bytes(pStmt, 4),        /* segdir.root */
          &pCsr->apSegment[i]
      );
      pCsr->nSegment++;
    }
    rc2 = sqlite3_reset(pStmt);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  return rc;
}

typedef struct IncrmergeWriter IncrmergeWriter;
typedef struct NodeWriter NodeWriter;
typedef struct Blob Blob;
typedef struct NodeReader NodeReader;

/*
** An instance of the following structure is used as a dynamic buffer
** to build up nodes or other blobs of data in.
**
** The function blobGrowBuffer() is used to extend the allocation.
*/
struct Blob {
  char *a;                        /* Pointer to allocation */
  int n;                          /* Number of valid bytes of data in a[] */
  int nAlloc;                     /* Allocated size of a[] (nAlloc>=n) */
};

/*
** This structure is used to build up buffers containing segment b-tree 
** nodes (blocks).
*/
struct NodeWriter {
  sqlite3_int64 iBlock;           /* Current block id */
  Blob key;                       /* Last key written to the current block */
  Blob block;                     /* Current block image */
};

/*
** An object of this type contains the state required to create or append
** to an appendable b-tree segment.
*/
struct IncrmergeWriter {
  int nLeafEst;                   /* Space allocated for leaf blocks */
  int nWork;                      /* Number of leaf pages flushed */
  sqlite3_int64 iAbsLevel;        /* Absolute level of input segments */
  int iIdx;                       /* Index of *output* segment in iAbsLevel+1 */
  sqlite3_int64 iStart;           /* Block number of first allocated block */
  sqlite3_int64 iEnd;             /* Block number of last allocated block */
  sqlite3_int64 nLeafData;        /* Bytes of leaf page data so far */
  u8 bNoLeafData;                 /* If true, store 0 for segment size */
  NodeWriter aNodeWriter[FTS_MAX_APPENDABLE_HEIGHT];
};

/*
** An object of the following type is used to read data from a single
** FTS segment node. See the following functions:
**
**     nodeReaderInit()
**     nodeReaderNext()
**     nodeReaderRelease()
*/
struct NodeReader {
  const char *aNode;
  int nNode;
  int iOff;                       /* Current offset within aNode[] */

  /* Output variables. Containing the current node entry. */
  sqlite3_int64 iChild;           /* Pointer to child node */
  Blob term;                      /* Current term */
  const char *aDoclist;           /* Pointer to doclist */
  int nDoclist;                   /* Size of doclist in bytes */
};

/*
** If *pRc is not SQLITE_OK when this function is called, it is a no-op.
** Otherwise, if the allocation at pBlob->a is not already at least nMin
** bytes in size, extend (realloc) it to be so.
**
** If an OOM error occurs, set *pRc to SQLITE_NOMEM and leave pBlob->a
** unmodified. Otherwise, if the allocation succeeds, update pBlob->nAlloc
** to reflect the new size of the pBlob->a[] buffer.
*/
static void blobGrowBuffer(Blob *pBlob, int nMin, int *pRc){
  if( *pRc==SQLITE_OK && nMin>pBlob->nAlloc ){
    int nAlloc = nMin;
    char *a = (char *)sqlite3_realloc(pBlob->a, nAlloc);
    if( a ){
      pBlob->nAlloc = nAlloc;
      pBlob->a = a;
    }else{
      *pRc = SQLITE_NOMEM;
    }
  }
}

/*
** Attempt to advance the node-reader object passed as the first argument to
** the next entry on the node. 
**
** Return an error code if an error occurs (SQLITE_NOMEM is possible). 
** Otherwise return SQLITE_OK. If there is no next entry on the node
** (e.g. because the current entry is the last) set NodeReader->aNode to
** NULL to indicate EOF. Otherwise, populate the NodeReader structure output 
** variables for the new entry.
*/
static int nodeReaderNext(NodeReader *p){
  int bFirst = (p->term.n==0);    /* True for first term on the node */
  int nPrefix = 0;                /* Bytes to copy from previous term */
  int nSuffix = 0;                /* Bytes to append to the prefix */
  int rc = SQLITE_OK;             /* Return code */

  assert( p->aNode );
  if( p->iChild && bFirst==0 ) p->iChild++;
  if( p->iOff>=p->nNode ){
    /* EOF */
    p->aNode = 0;
  }else{
    if( bFirst==0 ){
      p->iOff += fts3GetVarint32(&p->aNode[p->iOff], &nPrefix);
    }
    p->iOff += fts3GetVarint32(&p->aNode[p->iOff], &nSuffix);

    blobGrowBuffer(&p->term, nPrefix+nSuffix, &rc);
    if( rc==SQLITE_OK ){
      memcpy(&p->term.a[nPrefix], &p->aNode[p->iOff], nSuffix);
      p->term.n = nPrefix+nSuffix;
      p->iOff += nSuffix;
      if( p->iChild==0 ){
        p->iOff += fts3GetVarint32(&p->aNode[p->iOff], &p->nDoclist);
        p->aDoclist = &p->aNode[p->iOff];
        p->iOff += p->nDoclist;
      }
    }
  }

  assert( p->iOff<=p->nNode );

  return rc;
}

/*
** Release all dynamic resources held by node-reader object *p.
*/
static void nodeReaderRelease(NodeReader *p){
  sqlite3_free(p->term.a);
}

/*
** Initialize a node-reader object to read the node in buffer aNode/nNode.
**
** If successful, SQLITE_OK is returned and the NodeReader object set to 
** point to the first entry on the node (if any). Otherwise, an SQLite
** error code is returned.
*/
static int nodeReaderInit(NodeReader *p, const char *aNode, int nNode){
  memset(p, 0, sizeof(NodeReader));
  p->aNode = aNode;
  p->nNode = nNode;

  /* Figure out if this is a leaf or an internal node. */
  if( p->aNode[0] ){
    /* An internal node. */
    p->iOff = 1 + sqlite3Fts3GetVarint(&p->aNode[1], &p->iChild);
  }else{
    p->iOff = 1;
  }

  return nodeReaderNext(p);
}

/*
** This function is called while writing an FTS segment each time a leaf o
** node is finished and written to disk. The key (zTerm/nTerm) is guaranteed
** to be greater than the largest key on the node just written, but smaller
** than or equal to the first key that will be written to the next leaf
** node.
**
** The block id of the leaf node just written to disk may be found in
** (pWriter->aNodeWriter[0].iBlock) when this function is called.
*/
static int fts3IncrmergePush(
  Fts3Table *p,                   /* Fts3 table handle */
  IncrmergeWriter *pWriter,       /* Writer object */
  const char *zTerm,              /* Term to write to internal node */
  int nTerm                       /* Bytes at zTerm */
){
  sqlite3_int64 iPtr = pWriter->aNodeWriter[0].iBlock;
  int iLayer;

  assert( nTerm>0 );
  for(iLayer=1; ALWAYS(iLayer<FTS_MAX_APPENDABLE_HEIGHT); iLayer++){
    sqlite3_int64 iNextPtr = 0;
    NodeWriter *pNode = &pWriter->aNodeWriter[iLayer];
    int rc = SQLITE_OK;
    int nPrefix;
    int nSuffix;
    int nSpace;

    /* Figure out how much space the key will consume if it is written to
    ** the current node of layer iLayer. Due to the prefix compression, 
    ** the space required changes depending on which node the key is to
    ** be added to.  */
    nPrefix = fts3PrefixCompress(pNode->key.a, pNode->key.n, zTerm, nTerm);
    nSuffix = nTerm - nPrefix;
    nSpace  = sqlite3Fts3VarintLen(nPrefix);
    nSpace += sqlite3Fts3VarintLen(nSuffix) + nSuffix;

    if( pNode->key.n==0 || (pNode->block.n + nSpace)<=p->nNodeSize ){ 
      /* If the current node of layer iLayer contains zero keys, or if adding
      ** the key to it will not cause it to grow to larger than nNodeSize 
      ** bytes in size, write the key here.  */

      Blob *pBlk = &pNode->block;
      if( pBlk->n==0 ){
        blobGrowBuffer(pBlk, p->nNodeSize, &rc);
        if( rc==SQLITE_OK ){
          pBlk->a[0] = (char)iLayer;
          pBlk->n = 1 + sqlite3Fts3PutVarint(&pBlk->a[1], iPtr);
        }
      }
      blobGrowBuffer(pBlk, pBlk->n + nSpace, &rc);
      blobGrowBuffer(&pNode->key, nTerm, &rc);

      if( rc==SQLITE_OK ){
        if( pNode->key.n ){
          pBlk->n += sqlite3Fts3PutVarint(&pBlk->a[pBlk->n], nPrefix);
        }
        pBlk->n += sqlite3Fts3PutVarint(&pBlk->a[pBlk->n], nSuffix);
        memcpy(&pBlk->a[pBlk->n], &zTerm[nPrefix], nSuffix);
        pBlk->n += nSuffix;

        memcpy(pNode->key.a, zTerm, nTerm);
        pNode->key.n = nTerm;
      }
    }else{
      /* Otherwise, flush the current node of layer iLayer to disk.
      ** Then allocate a new, empty sibling node. The key will be written
      ** into the parent of this node. */
      rc = fts3WriteSegment(p, pNode->iBlock, pNode->block.a, pNode->block.n);

      assert( pNode->block.nAlloc>=p->nNodeSize );
      pNode->block.a[0] = (char)iLayer;
      pNode->block.n = 1 + sqlite3Fts3PutVarint(&pNode->block.a[1], iPtr+1);

      iNextPtr = pNode->iBlock;
      pNode->iBlock++;
      pNode->key.n = 0;
    }

    if( rc!=SQLITE_OK || iNextPtr==0 ) return rc;
    iPtr = iNextPtr;
  }

  assert( 0 );
  return 0;
}

/*
** Append a term and (optionally) doclist to the FTS segment node currently
** stored in blob *pNode. The node need not contain any terms, but the
** header must be written before this function is called.
**
** A node header is a single 0x00 byte for a leaf node, or a height varint
** followed by the left-hand-child varint for an internal node.
**
** The term to be appended is passed via arguments zTerm/nTerm. For a 
** leaf node, the doclist is passed as aDoclist/nDoclist. For an internal
** node, both aDoclist and nDoclist must be passed 0.
**
** If the size of the value in blob pPrev is zero, then this is the first
** term written to the node. Otherwise, pPrev contains a copy of the 
** previous term. Before this function returns, it is updated to contain a
** copy of zTerm/nTerm.
**
** It is assumed that the buffer associated with pNode is already large
** enough to accommodate the new entry. The buffer associated with pPrev
** is extended by this function if requrired.
**
** If an error (i.e. OOM condition) occurs, an SQLite error code is
** returned. Otherwise, SQLITE_OK.
*/
static int fts3AppendToNode(
  Blob *pNode,                    /* Current node image to append to */
  Blob *pPrev,                    /* Buffer containing previous term written */
  const char *zTerm,              /* New term to write */
  int nTerm,                      /* Size of zTerm in bytes */
  const char *aDoclist,           /* Doclist (or NULL) to write */
  int nDoclist                    /* Size of aDoclist in bytes */ 
){
  int rc = SQLITE_OK;             /* Return code */
  int bFirst = (pPrev->n==0);     /* True if this is the first term written */
  int nPrefix;                    /* Size of term prefix in bytes */
  int nSuffix;                    /* Size of term suffix in bytes */

  /* Node must have already been started. There must be a doclist for a
  ** leaf node, and there must not be a doclist for an internal node.  */
  assert( pNode->n>0 );
  assert( (pNode->a[0]=='\0')==(aDoclist!=0) );

  blobGrowBuffer(pPrev, nTerm, &rc);
  if( rc!=SQLITE_OK ) return rc;

  nPrefix = fts3PrefixCompress(pPrev->a, pPrev->n, zTerm, nTerm);
  nSuffix = nTerm - nPrefix;
  memcpy(pPrev->a, zTerm, nTerm);
  pPrev->n = nTerm;

  if( bFirst==0 ){
    pNode->n += sqlite3Fts3PutVarint(&pNode->a[pNode->n], nPrefix);
  }
  pNode->n += sqlite3Fts3PutVarint(&pNode->a[pNode->n], nSuffix);
  memcpy(&pNode->a[pNode->n], &zTerm[nPrefix], nSuffix);
  pNode->n += nSuffix;

  if( aDoclist ){
    pNode->n += sqlite3Fts3PutVarint(&pNode->a[pNode->n], nDoclist);
    memcpy(&pNode->a[pNode->n], aDoclist, nDoclist);
    pNode->n += nDoclist;
  }

  assert( pNode->n<=pNode->nAlloc );

  return SQLITE_OK;
}

/*
** Append the current term and doclist pointed to by cursor pCsr to the
** appendable b-tree segment opened for writing by pWriter.
**
** Return SQLITE_OK if successful, or an SQLite error code otherwise.
*/
static int fts3IncrmergeAppend(
  Fts3Table *p,                   /* Fts3 table handle */
  IncrmergeWriter *pWriter,       /* Writer object */
  Fts3MultiSegReader *pCsr        /* Cursor containing term and doclist */
){
  const char *zTerm = pCsr->zTerm;
  int nTerm = pCsr->nTerm;
  const char *aDoclist = pCsr->aDoclist;
  int nDoclist = pCsr->nDoclist;
  int rc = SQLITE_OK;           /* Return code */
  int nSpace;                   /* Total space in bytes required on leaf */
  int nPrefix;                  /* Size of prefix shared with previous term */
  int nSuffix;                  /* Size of suffix (nTerm - nPrefix) */
  NodeWriter *pLeaf;            /* Object used to write leaf nodes */

  pLeaf = &pWriter->aNodeWriter[0];
  nPrefix = fts3PrefixCompress(pLeaf->key.a, pLeaf->key.n, zTerm, nTerm);
  nSuffix = nTerm - nPrefix;

  nSpace  = sqlite3Fts3VarintLen(nPrefix);
  nSpace += sqlite3Fts3VarintLen(nSuffix) + nSuffix;
  nSpace += sqlite3Fts3VarintLen(nDoclist) + nDoclist;

  /* If the current block is not empty, and if adding this term/doclist
  ** to the current block would make it larger than Fts3Table.nNodeSize
  ** bytes, write this block out to the database. */
  if( pLeaf->block.n>0 && (pLeaf->block.n + nSpace)>p->nNodeSize ){
    rc = fts3WriteSegment(p, pLeaf->iBlock, pLeaf->block.a, pLeaf->block.n);
    pWriter->nWork++;

    /* Add the current term to the parent node. The term added to the 
    ** parent must:
    **
    **   a) be greater than the largest term on the leaf node just written
    **      to the database (still available in pLeaf->key), and
    **
    **   b) be less than or equal to the term about to be added to the new
    **      leaf node (zTerm/nTerm).
    **
    ** In other words, it must be the prefix of zTerm 1 byte longer than
    ** the common prefix (if any) of zTerm and pWriter->zTerm.
    */
    if( rc==SQLITE_OK ){
      rc = fts3IncrmergePush(p, pWriter, zTerm, nPrefix+1);
    }

    /* Advance to the next output block */
    pLeaf->iBlock++;
    pLeaf->key.n = 0;
    pLeaf->block.n = 0;

    nSuffix = nTerm;
    nSpace  = 1;
    nSpace += sqlite3Fts3VarintLen(nSuffix) + nSuffix;
    nSpace += sqlite3Fts3VarintLen(nDoclist) + nDoclist;
  }

  pWriter->nLeafData += nSpace;
  blobGrowBuffer(&pLeaf->block, pLeaf->block.n + nSpace, &rc);
  if( rc==SQLITE_OK ){
    if( pLeaf->block.n==0 ){
      pLeaf->block.n = 1;
      pLeaf->block.a[0] = '\0';
    }
    rc = fts3AppendToNode(
        &pLeaf->block, &pLeaf->key, zTerm, nTerm, aDoclist, nDoclist
    );
  }

  return rc;
}

/*
** This function is called to release all dynamic resources held by the
** merge-writer object pWriter, and if no error has occurred, to flush
** all outstanding node buffers held by pWriter to disk.
**
** If *pRc is not SQLITE_OK when this function is called, then no attempt
** is made to write any data to disk. Instead, this function serves only
** to release outstanding resources.
**
** Otherwise, if *pRc is initially SQLITE_OK and an error occurs while
** flushing buffers to disk, *pRc is set to an SQLite error code before
** returning.
*/
static void fts3IncrmergeRelease(
  Fts3Table *p,                   /* FTS3 table handle */
  IncrmergeWriter *pWriter,       /* Merge-writer object */
  int *pRc                        /* IN/OUT: Error code */
){
  int i;                          /* Used to iterate through non-root layers */
  int iRoot;                      /* Index of root in pWriter->aNodeWriter */
  NodeWriter *pRoot;              /* NodeWriter for root node */
  int rc = *pRc;                  /* Error code */

  /* Set iRoot to the index in pWriter->aNodeWriter[] of the output segment 
  ** root node. If the segment fits entirely on a single leaf node, iRoot
  ** will be set to 0. If the root node is the parent of the leaves, iRoot
  ** will be 1. And so on.  */
  for(iRoot=FTS_MAX_APPENDABLE_HEIGHT-1; iRoot>=0; iRoot--){
    NodeWriter *pNode = &pWriter->aNodeWriter[iRoot];
    if( pNode->block.n>0 ) break;
    assert( *pRc || pNode->block.nAlloc==0 );
    assert( *pRc || pNode->key.nAlloc==0 );
    sqlite3_free(pNode->block.a);
    sqlite3_free(pNode->key.a);
  }

  /* Empty output segment. This is a no-op. */
  if( iRoot<0 ) return;

  /* The entire output segment fits on a single node. Normally, this means
  ** the node would be stored as a blob in the "root" column of the %_segdir
  ** table. However, this is not permitted in this case. The problem is that 
  ** space has already been reserved in the %_segments table, and so the 
  ** start_block and end_block fields of the %_segdir table must be populated. 
  ** And, by design or by accident, released versions of FTS cannot handle 
  ** segments that fit entirely on the root node with start_block!=0.
  **
  ** Instead, create a synthetic root node that contains nothing but a 
  ** pointer to the single content node. So that the segment consists of a
  ** single leaf and a single interior (root) node.
  **
  ** Todo: Better might be to defer allocating space in the %_segments 
  ** table until we are sure it is needed.
  */
  if( iRoot==0 ){
    Blob *pBlock = &pWriter->aNodeWriter[1].block;
    blobGrowBuffer(pBlock, 1 + FTS3_VARINT_MAX, &rc);
    if( rc==SQLITE_OK ){
      pBlock->a[0] = 0x01;
      pBlock->n = 1 + sqlite3Fts3PutVarint(
          &pBlock->a[1], pWriter->aNodeWriter[0].iBlock
      );
    }
    iRoot = 1;
  }
  pRoot = &pWriter->aNodeWriter[iRoot];

  /* Flush all currently outstanding nodes to disk. */
  for(i=0; i<iRoot; i++){
    NodeWriter *pNode = &pWriter->aNodeWriter[i];
    if( pNode->block.n>0 && rc==SQLITE_OK ){
      rc = fts3WriteSegment(p, pNode->iBlock, pNode->block.a, pNode->block.n);
    }
    sqlite3_free(pNode->block.a);
    sqlite3_free(pNode->key.a);
  }

  /* Write the %_segdir record. */
  if( rc==SQLITE_OK ){
    rc = fts3WriteSegdir(p, 
        pWriter->iAbsLevel+1,               /* level */
        pWriter->iIdx,                      /* idx */
        pWriter->iStart,                    /* start_block */
        pWriter->aNodeWriter[0].iBlock,     /* leaves_end_block */
        pWriter->iEnd,                      /* end_block */
        (pWriter->bNoLeafData==0 ? pWriter->nLeafData : 0),   /* end_block */
        pRoot->block.a, pRoot->block.n      /* root */
    );
  }
  sqlite3_free(pRoot->block.a);
  sqlite3_free(pRoot->key.a);

  *pRc = rc;
}

/*
** Compare the term in buffer zLhs (size in bytes nLhs) with that in
** zRhs (size in bytes nRhs) using memcmp. If one term is a prefix of
** the other, it is considered to be smaller than the other.
**
** Return -ve if zLhs is smaller than zRhs, 0 if it is equal, or +ve
** if it is greater.
*/
static int fts3TermCmp(
  const char *zLhs, int nLhs,     /* LHS of comparison */
  const char *zRhs, int nRhs      /* RHS of comparison */
){
  int nCmp = MIN(nLhs, nRhs);
  int res;

  res = memcmp(zLhs, zRhs, nCmp);
  if( res==0 ) res = nLhs - nRhs;

  return res;
}


/*
** Query to see if the entry in the %_segments table with blockid iEnd is 
** NULL. If no error occurs and the entry is NULL, set *pbRes 1 before
** returning. Otherwise, set *pbRes to 0. 
**
** Or, if an error occurs while querying the database, return an SQLite 
** error code. The final value of *pbRes is undefined in this case.
**
** This is used to test if a segment is an "appendable" segment. If it
** is, then a NULL entry has been inserted into the %_segments table
** with blockid %_segdir.end_block.
*/
static int fts3IsAppendable(Fts3Table *p, sqlite3_int64 iEnd, int *pbRes){
  int bRes = 0;                   /* Result to set *pbRes to */
  sqlite3_stmt *pCheck = 0;       /* Statement to query database with */
  int rc;                         /* Return code */

  rc = fts3SqlStmt(p, SQL_SEGMENT_IS_APPENDABLE, &pCheck, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pCheck, 1, iEnd);
    if( SQLITE_ROW==sqlite3_step(pCheck) ) bRes = 1;
    rc = sqlite3_reset(pCheck);
  }
  
  *pbRes = bRes;
  return rc;
}

/*
** This function is called when initializing an incremental-merge operation.
** It checks if the existing segment with index value iIdx at absolute level 
** (iAbsLevel+1) can be appended to by the incremental merge. If it can, the
** merge-writer object *pWriter is initialized to write to it.
**
** An existing segment can be appended to by an incremental merge if:
**
**   * It was initially created as an appendable segment (with all required
**     space pre-allocated), and
**
**   * The first key read from the input (arguments zKey and nKey) is 
**     greater than the largest key currently stored in the potential
**     output segment.
*/
static int fts3IncrmergeLoad(
  Fts3Table *p,                   /* Fts3 table handle */
  sqlite3_int64 iAbsLevel,        /* Absolute level of input segments */
  int iIdx,                       /* Index of candidate output segment */
  const char *zKey,               /* First key to write */
  int nKey,                       /* Number of bytes in nKey */
  IncrmergeWriter *pWriter        /* Populate this object */
){
  int rc;                         /* Return code */
  sqlite3_stmt *pSelect = 0;      /* SELECT to read %_segdir entry */

  rc = fts3SqlStmt(p, SQL_SELECT_SEGDIR, &pSelect, 0);
  if( rc==SQLITE_OK ){
    sqlite3_int64 iStart = 0;     /* Value of %_segdir.start_block */
    sqlite3_int64 iLeafEnd = 0;   /* Value of %_segdir.leaves_end_block */
    sqlite3_int64 iEnd = 0;       /* Value of %_segdir.end_block */
    const char *aRoot = 0;        /* Pointer to %_segdir.root buffer */
    int nRoot = 0;                /* Size of aRoot[] in bytes */
    int rc2;                      /* Return code from sqlite3_reset() */
    int bAppendable = 0;          /* Set to true if segment is appendable */

    /* Read the %_segdir entry for index iIdx absolute level (iAbsLevel+1) */
    sqlite3_bind_int64(pSelect, 1, iAbsLevel+1);
    sqlite3_bind_int(pSelect, 2, iIdx);
    if( sqlite3_step(pSelect)==SQLITE_ROW ){
      iStart = sqlite3_column_int64(pSelect, 1);
      iLeafEnd = sqlite3_column_int64(pSelect, 2);
      fts3ReadEndBlockField(pSelect, 3, &iEnd, &pWriter->nLeafData);
      if( pWriter->nLeafData<0 ){
        pWriter->nLeafData = pWriter->nLeafData * -1;
      }
      pWriter->bNoLeafData = (pWriter->nLeafData==0);
      nRoot = sqlite3_column_bytes(pSelect, 4);
      aRoot = sqlite3_column_blob(pSelect, 4);
    }else{
      return sqlite3_reset(pSelect);
    }

    /* Check for the zero-length marker in the %_segments table */
    rc = fts3IsAppendable(p, iEnd, &bAppendable);

    /* Check that zKey/nKey is larger than the largest key the candidate */
    if( rc==SQLITE_OK && bAppendable ){
      char *aLeaf = 0;
      int nLeaf = 0;

      rc = sqlite3Fts3ReadBlock(p, iLeafEnd, &aLeaf, &nLeaf, 0);
      if( rc==SQLITE_OK ){
        NodeReader reader;
        for(rc = nodeReaderInit(&reader, aLeaf, nLeaf);
            rc==SQLITE_OK && reader.aNode;
            rc = nodeReaderNext(&reader)
        ){
          assert( reader.aNode );
        }
        if( fts3TermCmp(zKey, nKey, reader.term.a, reader.term.n)<=0 ){
          bAppendable = 0;
        }
        nodeReaderRelease(&reader);
      }
      sqlite3_free(aLeaf);
    }

    if( rc==SQLITE_OK && bAppendable ){
      /* It is possible to append to this segment. Set up the IncrmergeWriter
      ** object to do so.  */
      int i;
      int nHeight = (int)aRoot[0];
      NodeWriter *pNode;

      pWriter->nLeafEst = (int)((iEnd - iStart) + 1)/FTS_MAX_APPENDABLE_HEIGHT;
      pWriter->iStart = iStart;
      pWriter->iEnd = iEnd;
      pWriter->iAbsLevel = iAbsLevel;
      pWriter->iIdx = iIdx;

      for(i=nHeight+1; i<FTS_MAX_APPENDABLE_HEIGHT; i++){
        pWriter->aNodeWriter[i].iBlock = pWriter->iStart + i*pWriter->nLeafEst;
      }

      pNode = &pWriter->aNodeWriter[nHeight];
      pNode->iBlock = pWriter->iStart + pWriter->nLeafEst*nHeight;
      blobGrowBuffer(&pNode->block, MAX(nRoot, p->nNodeSize), &rc);
      if( rc==SQLITE_OK ){
        memcpy(pNode->block.a, aRoot, nRoot);
        pNode->block.n = nRoot;
      }

      for(i=nHeight; i>=0 && rc==SQLITE_OK; i--){
        NodeReader reader;
        pNode = &pWriter->aNodeWriter[i];

        rc = nodeReaderInit(&reader, pNode->block.a, pNode->block.n);
        while( reader.aNode && rc==SQLITE_OK ) rc = nodeReaderNext(&reader);
        blobGrowBuffer(&pNode->key, reader.term.n, &rc);
        if( rc==SQLITE_OK ){
          memcpy(pNode->key.a, reader.term.a, reader.term.n);
          pNode->key.n = reader.term.n;
          if( i>0 ){
            char *aBlock = 0;
            int nBlock = 0;
            pNode = &pWriter->aNodeWriter[i-1];
            pNode->iBlock = reader.iChild;
            rc = sqlite3Fts3ReadBlock(p, reader.iChild, &aBlock, &nBlock, 0);
            blobGrowBuffer(&pNode->block, MAX(nBlock, p->nNodeSize), &rc);
            if( rc==SQLITE_OK ){
              memcpy(pNode->block.a, aBlock, nBlock);
              pNode->block.n = nBlock;
            }
            sqlite3_free(aBlock);
          }
        }
        nodeReaderRelease(&reader);
      }
    }

    rc2 = sqlite3_reset(pSelect);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  return rc;
}

/*
** Determine the largest segment index value that exists within absolute
** level iAbsLevel+1. If no error occurs, set *piIdx to this value plus
** one before returning SQLITE_OK. Or, if there are no segments at all 
** within level iAbsLevel, set *piIdx to zero.
**
** If an error occurs, return an SQLite error code. The final value of
** *piIdx is undefined in this case.
*/
static int fts3IncrmergeOutputIdx( 
  Fts3Table *p,                   /* FTS Table handle */
  sqlite3_int64 iAbsLevel,        /* Absolute index of input segments */
  int *piIdx                      /* OUT: Next free index at iAbsLevel+1 */
){
  int rc;
  sqlite3_stmt *pOutputIdx = 0;   /* SQL used to find output index */

  rc = fts3SqlStmt(p, SQL_NEXT_SEGMENT_INDEX, &pOutputIdx, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pOutputIdx, 1, iAbsLevel+1);
    sqlite3_step(pOutputIdx);
    *piIdx = sqlite3_column_int(pOutputIdx, 0);
    rc = sqlite3_reset(pOutputIdx);
  }

  return rc;
}

/* 
** Allocate an appendable output segment on absolute level iAbsLevel+1
** with idx value iIdx.
**
** In the %_segdir table, a segment is defined by the values in three
** columns:
**
**     start_block
**     leaves_end_block
**     end_block
**
** When an appendable segment is allocated, it is estimated that the
** maximum number of leaf blocks that may be required is the sum of the
** number of leaf blocks consumed by the input segments, plus the number
** of input segments, multiplied by two. This value is stored in stack 
** variable nLeafEst.
**
** A total of 16*nLeafEst blocks are allocated when an appendable segment
** is created ((1 + end_block - start_block)==16*nLeafEst). The contiguous
** array of leaf nodes starts at the first block allocated. The array
** of interior nodes that are parents of the leaf nodes start at block
** (start_block + (1 + end_block - start_block) / 16). And so on.
**
** In the actual code below, the value "16" is replaced with the 
** pre-processor macro FTS_MAX_APPENDABLE_HEIGHT.
*/
static int fts3IncrmergeWriter( 
  Fts3Table *p,                   /* Fts3 table handle */
  sqlite3_int64 iAbsLevel,        /* Absolute level of input segments */
  int iIdx,                       /* Index of new output segment */
  Fts3MultiSegReader *pCsr,       /* Cursor that data will be read from */
  IncrmergeWriter *pWriter        /* Populate this object */
){
  int rc;                         /* Return Code */
  int i;                          /* Iterator variable */
  int nLeafEst = 0;               /* Blocks allocated for leaf nodes */
  sqlite3_stmt *pLeafEst = 0;     /* SQL used to determine nLeafEst */
  sqlite3_stmt *pFirstBlock = 0;  /* SQL used to determine first block */

  /* Calculate nLeafEst. */
  rc = fts3SqlStmt(p, SQL_MAX_LEAF_NODE_ESTIMATE, &pLeafEst, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pLeafEst, 1, iAbsLevel);
    sqlite3_bind_int64(pLeafEst, 2, pCsr->nSegment);
    if( SQLITE_ROW==sqlite3_step(pLeafEst) ){
      nLeafEst = sqlite3_column_int(pLeafEst, 0);
    }
    rc = sqlite3_reset(pLeafEst);
  }
  if( rc!=SQLITE_OK ) return rc;

  /* Calculate the first block to use in the output segment */
  rc = fts3SqlStmt(p, SQL_NEXT_SEGMENTS_ID, &pFirstBlock, 0);
  if( rc==SQLITE_OK ){
    if( SQLITE_ROW==sqlite3_step(pFirstBlock) ){
      pWriter->iStart = sqlite3_column_int64(pFirstBlock, 0);
      pWriter->iEnd = pWriter->iStart - 1;
      pWriter->iEnd += nLeafEst * FTS_MAX_APPENDABLE_HEIGHT;
    }
    rc = sqlite3_reset(pFirstBlock);
  }
  if( rc!=SQLITE_OK ) return rc;

  /* Insert the marker in the %_segments table to make sure nobody tries
  ** to steal the space just allocated. This is also used to identify 
  ** appendable segments.  */
  rc = fts3WriteSegment(p, pWriter->iEnd, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  pWriter->iAbsLevel = iAbsLevel;
  pWriter->nLeafEst = nLeafEst;
  pWriter->iIdx = iIdx;

  /* Set up the array of NodeWriter objects */
  for(i=0; i<FTS_MAX_APPENDABLE_HEIGHT; i++){
    pWriter->aNodeWriter[i].iBlock = pWriter->iStart + i*pWriter->nLeafEst;
  }
  return SQLITE_OK;
}

/*
** Remove an entry from the %_segdir table. This involves running the 
** following two statements:
**
**   DELETE FROM %_segdir WHERE level = :iAbsLevel AND idx = :iIdx
**   UPDATE %_segdir SET idx = idx - 1 WHERE level = :iAbsLevel AND idx > :iIdx
**
** The DELETE statement removes the specific %_segdir level. The UPDATE 
** statement ensures that the remaining segments have contiguously allocated
** idx values.
*/
static int fts3RemoveSegdirEntry(
  Fts3Table *p,                   /* FTS3 table handle */
  sqlite3_int64 iAbsLevel,        /* Absolute level to delete from */
  int iIdx                        /* Index of %_segdir entry to delete */
){
  int rc;                         /* Return code */
  sqlite3_stmt *pDelete = 0;      /* DELETE statement */

  rc = fts3SqlStmt(p, SQL_DELETE_SEGDIR_ENTRY, &pDelete, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pDelete, 1, iAbsLevel);
    sqlite3_bind_int(pDelete, 2, iIdx);
    sqlite3_step(pDelete);
    rc = sqlite3_reset(pDelete);
  }

  return rc;
}

/*
** One or more segments have just been removed from absolute level iAbsLevel.
** Update the 'idx' values of the remaining segments in the level so that
** the idx values are a contiguous sequence starting from 0.
*/
static int fts3RepackSegdirLevel(
  Fts3Table *p,                   /* FTS3 table handle */
  sqlite3_int64 iAbsLevel         /* Absolute level to repack */
){
  int rc;                         /* Return code */
  int *aIdx = 0;                  /* Array of remaining idx values */
  int nIdx = 0;                   /* Valid entries in aIdx[] */
  int nAlloc = 0;                 /* Allocated size of aIdx[] */
  int i;                          /* Iterator variable */
  sqlite3_stmt *pSelect = 0;      /* Select statement to read idx values */
  sqlite3_stmt *pUpdate = 0;      /* Update statement to modify idx values */

  rc = fts3SqlStmt(p, SQL_SELECT_INDEXES, &pSelect, 0);
  if( rc==SQLITE_OK ){
    int rc2;
    sqlite3_bind_int64(pSelect, 1, iAbsLevel);
    while( SQLITE_ROW==sqlite3_step(pSelect) ){
      if( nIdx>=nAlloc ){
        int *aNew;
        nAlloc += 16;
        aNew = sqlite3_realloc(aIdx, nAlloc*sizeof(int));
        if( !aNew ){
          rc = SQLITE_NOMEM;
          break;
        }
        aIdx = aNew;
      }
      aIdx[nIdx++] = sqlite3_column_int(pSelect, 0);
    }
    rc2 = sqlite3_reset(pSelect);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  if( rc==SQLITE_OK ){
    rc = fts3SqlStmt(p, SQL_SHIFT_SEGDIR_ENTRY, &pUpdate, 0);
  }
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pUpdate, 2, iAbsLevel);
  }

  assert( p->bIgnoreSavepoint==0 );
  p->bIgnoreSavepoint = 1;
  for(i=0; rc==SQLITE_OK && i<nIdx; i++){
    if( aIdx[i]!=i ){
      sqlite3_bind_int(pUpdate, 3, aIdx[i]);
      sqlite3_bind_int(pUpdate, 1, i);
      sqlite3_step(pUpdate);
      rc = sqlite3_reset(pUpdate);
    }
  }
  p->bIgnoreSavepoint = 0;

  sqlite3_free(aIdx);
  return rc;
}

static void fts3StartNode(Blob *pNode, int iHeight, sqlite3_int64 iChild){
  pNode->a[0] = (char)iHeight;
  if( iChild ){
    assert( pNode->nAlloc>=1+sqlite3Fts3VarintLen(iChild) );
    pNode->n = 1 + sqlite3Fts3PutVarint(&pNode->a[1], iChild);
  }else{
    assert( pNode->nAlloc>=1 );
    pNode->n = 1;
  }
}

/*
** The first two arguments are a pointer to and the size of a segment b-tree
** node. The node may be a leaf or an internal node.
**
** This function creates a new node image in blob object *pNew by copying
** all terms that are greater than or equal to zTerm/nTerm (for leaf nodes)
** or greater than zTerm/nTerm (for internal nodes) from aNode/nNode.
*/
static int fts3TruncateNode(
  const char *aNode,              /* Current node image */
  int nNode,                      /* Size of aNode in bytes */
  Blob *pNew,                     /* OUT: Write new node image here */
  const char *zTerm,              /* Omit all terms smaller than this */
  int nTerm,                      /* Size of zTerm in bytes */
  sqlite3_int64 *piBlock          /* OUT: Block number in next layer down */
){
  NodeReader reader;              /* Reader object */
  Blob prev = {0, 0, 0};          /* Previous term written to new node */
  int rc = SQLITE_OK;             /* Return code */
  int bLeaf = aNode[0]=='\0';     /* True for a leaf node */

  /* Allocate required output space */
  blobGrowBuffer(pNew, nNode, &rc);
  if( rc!=SQLITE_OK ) return rc;
  pNew->n = 0;

  /* Populate new node buffer */
  for(rc = nodeReaderInit(&reader, aNode, nNode); 
      rc==SQLITE_OK && reader.aNode; 
      rc = nodeReaderNext(&reader)
  ){
    if( pNew->n==0 ){
      int res = fts3TermCmp(reader.term.a, reader.term.n, zTerm, nTerm);
      if( res<0 || (bLeaf==0 && res==0) ) continue;
      fts3StartNode(pNew, (int)aNode[0], reader.iChild);
      *piBlock = reader.iChild;
    }
    rc = fts3AppendToNode(
        pNew, &prev, reader.term.a, reader.term.n,
        reader.aDoclist, reader.nDoclist
    );
    if( rc!=SQLITE_OK ) break;
  }
  if( pNew->n==0 ){
    fts3StartNode(pNew, (int)aNode[0], reader.iChild);
    *piBlock = reader.iChild;
  }
  assert( pNew->n<=pNew->nAlloc );

  nodeReaderRelease(&reader);
  sqlite3_free(prev.a);
  return rc;
}

/*
** Remove all terms smaller than zTerm/nTerm from segment iIdx in absolute 
** level iAbsLevel. This may involve deleting entries from the %_segments
** table, and modifying existing entries in both the %_segments and %_segdir
** tables.
**
** SQLITE_OK is returned if the segment is updated successfully. Or an
** SQLite error code otherwise.
*/
static int fts3TruncateSegment(
  Fts3Table *p,                   /* FTS3 table handle */
  sqlite3_int64 iAbsLevel,        /* Absolute level of segment to modify */
  int iIdx,                       /* Index within level of segment to modify */
  const char *zTerm,              /* Remove terms smaller than this */
  int nTerm                      /* Number of bytes in buffer zTerm */
){
  int rc = SQLITE_OK;             /* Return code */
  Blob root = {0,0,0};            /* New root page image */
  Blob block = {0,0,0};           /* Buffer used for any other block */
  sqlite3_int64 iBlock = 0;       /* Block id */
  sqlite3_int64 iNewStart = 0;    /* New value for iStartBlock */
  sqlite3_int64 iOldStart = 0;    /* Old value for iStartBlock */
  sqlite3_stmt *pFetch = 0;       /* Statement used to fetch segdir */

  rc = fts3SqlStmt(p, SQL_SELECT_SEGDIR, &pFetch, 0);
  if( rc==SQLITE_OK ){
    int rc2;                      /* sqlite3_reset() return code */
    sqlite3_bind_int64(pFetch, 1, iAbsLevel);
    sqlite3_bind_int(pFetch, 2, iIdx);
    if( SQLITE_ROW==sqlite3_step(pFetch) ){
      const char *aRoot = sqlite3_column_blob(pFetch, 4);
      int nRoot = sqlite3_column_bytes(pFetch, 4);
      iOldStart = sqlite3_column_int64(pFetch, 1);
      rc = fts3TruncateNode(aRoot, nRoot, &root, zTerm, nTerm, &iBlock);
    }
    rc2 = sqlite3_reset(pFetch);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  while( rc==SQLITE_OK && iBlock ){
    char *aBlock = 0;
    int nBlock = 0;
    iNewStart = iBlock;

    rc = sqlite3Fts3ReadBlock(p, iBlock, &aBlock, &nBlock, 0);
    if( rc==SQLITE_OK ){
      rc = fts3TruncateNode(aBlock, nBlock, &block, zTerm, nTerm, &iBlock);
    }
    if( rc==SQLITE_OK ){
      rc = fts3WriteSegment(p, iNewStart, block.a, block.n);
    }
    sqlite3_free(aBlock);
  }

  /* Variable iNewStart now contains the first valid leaf node. */
  if( rc==SQLITE_OK && iNewStart ){
    sqlite3_stmt *pDel = 0;
    rc = fts3SqlStmt(p, SQL_DELETE_SEGMENTS_RANGE, &pDel, 0);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(pDel, 1, iOldStart);
      sqlite3_bind_int64(pDel, 2, iNewStart-1);
      sqlite3_step(pDel);
      rc = sqlite3_reset(pDel);
    }
  }

  if( rc==SQLITE_OK ){
    sqlite3_stmt *pChomp = 0;
    rc = fts3SqlStmt(p, SQL_CHOMP_SEGDIR, &pChomp, 0);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(pChomp, 1, iNewStart);
      sqlite3_bind_blob(pChomp, 2, root.a, root.n, SQLITE_STATIC);
      sqlite3_bind_int64(pChomp, 3, iAbsLevel);
      sqlite3_bind_int(pChomp, 4, iIdx);
      sqlite3_step(pChomp);
      rc = sqlite3_reset(pChomp);
    }
  }

  sqlite3_free(root.a);
  sqlite3_free(block.a);
  return rc;
}

/*
** This function is called after an incrmental-merge operation has run to
** merge (or partially merge) two or more segments from absolute level
** iAbsLevel.
**
** Each input segment is either removed from the db completely (if all of
** its data was copied to the output segment by the incrmerge operation)
** or modified in place so that it no longer contains those entries that
** have been duplicated in the output segment.
*/
static int fts3IncrmergeChomp(
  Fts3Table *p,                   /* FTS table handle */
  sqlite3_int64 iAbsLevel,        /* Absolute level containing segments */
  Fts3MultiSegReader *pCsr,       /* Chomp all segments opened by this cursor */
  int *pnRem                      /* Number of segments not deleted */
){
  int i;
  int nRem = 0;
  int rc = SQLITE_OK;

  for(i=pCsr->nSegment-1; i>=0 && rc==SQLITE_OK; i--){
    Fts3SegReader *pSeg = 0;
    int j;

    /* Find the Fts3SegReader object with Fts3SegReader.iIdx==i. It is hiding
    ** somewhere in the pCsr->apSegment[] array.  */
    for(j=0; ALWAYS(j<pCsr->nSegment); j++){
      pSeg = pCsr->apSegment[j];
      if( pSeg->iIdx==i ) break;
    }
    assert( j<pCsr->nSegment && pSeg->iIdx==i );

    if( pSeg->aNode==0 ){
      /* Seg-reader is at EOF. Remove the entire input segment. */
      rc = fts3DeleteSegment(p, pSeg);
      if( rc==SQLITE_OK ){
        rc = fts3RemoveSegdirEntry(p, iAbsLevel, pSeg->iIdx);
      }
      *pnRem = 0;
    }else{
      /* The incremental merge did not copy all the data from this 
      ** segment to the upper level. The segment is modified in place
      ** so that it contains no keys smaller than zTerm/nTerm. */ 
      const char *zTerm = pSeg->zTerm;
      int nTerm = pSeg->nTerm;
      rc = fts3TruncateSegment(p, iAbsLevel, pSeg->iIdx, zTerm, nTerm);
      nRem++;
    }
  }

  if( rc==SQLITE_OK && nRem!=pCsr->nSegment ){
    rc = fts3RepackSegdirLevel(p, iAbsLevel);
  }

  *pnRem = nRem;
  return rc;
}

/*
** Store an incr-merge hint in the database.
*/
static int fts3IncrmergeHintStore(Fts3Table *p, Blob *pHint){
  sqlite3_stmt *pReplace = 0;
  int rc;                         /* Return code */

  rc = fts3SqlStmt(p, SQL_REPLACE_STAT, &pReplace, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int(pReplace, 1, FTS_STAT_INCRMERGEHINT);
    sqlite3_bind_blob(pReplace, 2, pHint->a, pHint->n, SQLITE_STATIC);
    sqlite3_step(pReplace);
    rc = sqlite3_reset(pReplace);
  }

  return rc;
}

/*
** Load an incr-merge hint from the database. The incr-merge hint, if one 
** exists, is stored in the rowid==1 row of the %_stat table.
**
** If successful, populate blob *pHint with the value read from the %_stat
** table and return SQLITE_OK. Otherwise, if an error occurs, return an
** SQLite error code.
*/
static int fts3IncrmergeHintLoad(Fts3Table *p, Blob *pHint){
  sqlite3_stmt *pSelect = 0;
  int rc;

  pHint->n = 0;
  rc = fts3SqlStmt(p, SQL_SELECT_STAT, &pSelect, 0);
  if( rc==SQLITE_OK ){
    int rc2;
    sqlite3_bind_int(pSelect, 1, FTS_STAT_INCRMERGEHINT);
    if( SQLITE_ROW==sqlite3_step(pSelect) ){
      const char *aHint = sqlite3_column_blob(pSelect, 0);
      int nHint = sqlite3_column_bytes(pSelect, 0);
      if( aHint ){
        blobGrowBuffer(pHint, nHint, &rc);
        if( rc==SQLITE_OK ){
          memcpy(pHint->a, aHint, nHint);
          pHint->n = nHint;
        }
      }
    }
    rc2 = sqlite3_reset(pSelect);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  return rc;
}

/*
** If *pRc is not SQLITE_OK when this function is called, it is a no-op.
** Otherwise, append an entry to the hint stored in blob *pHint. Each entry
** consists of two varints, the absolute level number of the input segments 
** and the number of input segments.
**
** If successful, leave *pRc set to SQLITE_OK and return. If an error occurs,
** set *pRc to an SQLite error code before returning.
*/
static void fts3IncrmergeHintPush(
  Blob *pHint,                    /* Hint blob to append to */
  i64 iAbsLevel,                  /* First varint to store in hint */
  int nInput,                     /* Second varint to store in hint */
  int *pRc                        /* IN/OUT: Error code */
){
  blobGrowBuffer(pHint, pHint->n + 2*FTS3_VARINT_MAX, pRc);
  if( *pRc==SQLITE_OK ){
    pHint->n += sqlite3Fts3PutVarint(&pHint->a[pHint->n], iAbsLevel);
    pHint->n += sqlite3Fts3PutVarint(&pHint->a[pHint->n], (i64)nInput);
  }
}

/*
** Read the last entry (most recently pushed) from the hint blob *pHint
** and then remove the entry. Write the two values read to *piAbsLevel and 
** *pnInput before returning.
**
** If no error occurs, return SQLITE_OK. If the hint blob in *pHint does
** not contain at least two valid varints, return SQLITE_CORRUPT_VTAB.
*/
static int fts3IncrmergeHintPop(Blob *pHint, i64 *piAbsLevel, int *pnInput){
  const int nHint = pHint->n;
  int i;

  i = pHint->n-2;
  while( i>0 && (pHint->a[i-1] & 0x80) ) i--;
  while( i>0 && (pHint->a[i-1] & 0x80) ) i--;

  pHint->n = i;
  i += sqlite3Fts3GetVarint(&pHint->a[i], piAbsLevel);
  i += fts3GetVarint32(&pHint->a[i], pnInput);
  if( i!=nHint ) return SQLITE_CORRUPT_VTAB;

  return SQLITE_OK;
}


/*
** Attempt an incremental merge that writes nMerge leaf blocks.
**
** Incremental merges happen nMin segments at a time. The segments 
** to be merged are the nMin oldest segments (the ones with the smallest 
** values for the _segdir.idx field) in the highest level that contains 
** at least nMin segments. Multiple merges might occur in an attempt to 
** write the quota of nMerge leaf blocks.
*/
SQLITE_PRIVATE int sqlite3Fts3Incrmerge(Fts3Table *p, int nMerge, int nMin){
  int rc;                         /* Return code */
  int nRem = nMerge;              /* Number of leaf pages yet to  be written */
  Fts3MultiSegReader *pCsr;       /* Cursor used to read input data */
  Fts3SegFilter *pFilter;         /* Filter used with cursor pCsr */
  IncrmergeWriter *pWriter;       /* Writer object */
  int nSeg = 0;                   /* Number of input segments */
  sqlite3_int64 iAbsLevel = 0;    /* Absolute level number to work on */
  Blob hint = {0, 0, 0};          /* Hint read from %_stat table */
  int bDirtyHint = 0;             /* True if blob 'hint' has been modified */

  /* Allocate space for the cursor, filter and writer objects */
  const int nAlloc = sizeof(*pCsr) + sizeof(*pFilter) + sizeof(*pWriter);
  pWriter = (IncrmergeWriter *)sqlite3_malloc(nAlloc);
  if( !pWriter ) return SQLITE_NOMEM;
  pFilter = (Fts3SegFilter *)&pWriter[1];
  pCsr = (Fts3MultiSegReader *)&pFilter[1];

  rc = fts3IncrmergeHintLoad(p, &hint);
  while( rc==SQLITE_OK && nRem>0 ){
    const i64 nMod = FTS3_SEGDIR_MAXLEVEL * p->nIndex;
    sqlite3_stmt *pFindLevel = 0; /* SQL used to determine iAbsLevel */
    int bUseHint = 0;             /* True if attempting to append */
    int iIdx = 0;                 /* Largest idx in level (iAbsLevel+1) */

    /* Search the %_segdir table for the absolute level with the smallest
    ** relative level number that contains at least nMin segments, if any.
    ** If one is found, set iAbsLevel to the absolute level number and
    ** nSeg to nMin. If no level with at least nMin segments can be found, 
    ** set nSeg to -1.
    */
    rc = fts3SqlStmt(p, SQL_FIND_MERGE_LEVEL, &pFindLevel, 0);
    sqlite3_bind_int(pFindLevel, 1, nMin);
    if( sqlite3_step(pFindLevel)==SQLITE_ROW ){
      iAbsLevel = sqlite3_column_int64(pFindLevel, 0);
      nSeg = nMin;
    }else{
      nSeg = -1;
    }
    rc = sqlite3_reset(pFindLevel);

    /* If the hint read from the %_stat table is not empty, check if the
    ** last entry in it specifies a relative level smaller than or equal
    ** to the level identified by the block above (if any). If so, this 
    ** iteration of the loop will work on merging at the hinted level.
    */
    if( rc==SQLITE_OK && hint.n ){
      int nHint = hint.n;
      sqlite3_int64 iHintAbsLevel = 0;      /* Hint level */
      int nHintSeg = 0;                     /* Hint number of segments */

      rc = fts3IncrmergeHintPop(&hint, &iHintAbsLevel, &nHintSeg);
      if( nSeg<0 || (iAbsLevel % nMod) >= (iHintAbsLevel % nMod) ){
        iAbsLevel = iHintAbsLevel;
        nSeg = nHintSeg;
        bUseHint = 1;
        bDirtyHint = 1;
      }else{
        /* This undoes the effect of the HintPop() above - so that no entry
        ** is removed from the hint blob.  */
        hint.n = nHint;
      }
    }

    /* If nSeg is less that zero, then there is no level with at least
    ** nMin segments and no hint in the %_stat table. No work to do.
    ** Exit early in this case.  */
    if( nSeg<0 ) break;

    /* Open a cursor to iterate through the contents of the oldest nSeg 
    ** indexes of absolute level iAbsLevel. If this cursor is opened using 
    ** the 'hint' parameters, it is possible that there are less than nSeg
    ** segments available in level iAbsLevel. In this case, no work is
    ** done on iAbsLevel - fall through to the next iteration of the loop 
    ** to start work on some other level.  */
    memset(pWriter, 0, nAlloc);
    pFilter->flags = FTS3_SEGMENT_REQUIRE_POS;

    if( rc==SQLITE_OK ){
      rc = fts3IncrmergeOutputIdx(p, iAbsLevel, &iIdx);
      assert( bUseHint==1 || bUseHint==0 );
      if( iIdx==0 || (bUseHint && iIdx==1) ){
        int bIgnore = 0;
        rc = fts3SegmentIsMaxLevel(p, iAbsLevel+1, &bIgnore);
        if( bIgnore ){
          pFilter->flags |= FTS3_SEGMENT_IGNORE_EMPTY;
        }
      }
    }

    if( rc==SQLITE_OK ){
      rc = fts3IncrmergeCsr(p, iAbsLevel, nSeg, pCsr);
    }
    if( SQLITE_OK==rc && pCsr->nSegment==nSeg
     && SQLITE_OK==(rc = sqlite3Fts3SegReaderStart(p, pCsr, pFilter))
     && SQLITE_ROW==(rc = sqlite3Fts3SegReaderStep(p, pCsr))
    ){
      if( bUseHint && iIdx>0 ){
        const char *zKey = pCsr->zTerm;
        int nKey = pCsr->nTerm;
        rc = fts3IncrmergeLoad(p, iAbsLevel, iIdx-1, zKey, nKey, pWriter);
      }else{
        rc = fts3IncrmergeWriter(p, iAbsLevel, iIdx, pCsr, pWriter);
      }

      if( rc==SQLITE_OK && pWriter->nLeafEst ){
        fts3LogMerge(nSeg, iAbsLevel);
        do {
          rc = fts3IncrmergeAppend(p, pWriter, pCsr);
          if( rc==SQLITE_OK ) rc = sqlite3Fts3SegReaderStep(p, pCsr);
          if( pWriter->nWork>=nRem && rc==SQLITE_ROW ) rc = SQLITE_OK;
        }while( rc==SQLITE_ROW );

        /* Update or delete the input segments */
        if( rc==SQLITE_OK ){
          nRem -= (1 + pWriter->nWork);
          rc = fts3IncrmergeChomp(p, iAbsLevel, pCsr, &nSeg);
          if( nSeg!=0 ){
            bDirtyHint = 1;
            fts3IncrmergeHintPush(&hint, iAbsLevel, nSeg, &rc);
          }
        }
      }

      if( nSeg!=0 ){
        pWriter->nLeafData = pWriter->nLeafData * -1;
      }
      fts3IncrmergeRelease(p, pWriter, &rc);
      if( nSeg==0 && pWriter->bNoLeafData==0 ){
        fts3PromoteSegments(p, iAbsLevel+1, pWriter->nLeafData);
      }
    }

    sqlite3Fts3SegReaderFinish(pCsr);
  }

  /* Write the hint values into the %_stat table for the next incr-merger */
  if( bDirtyHint && rc==SQLITE_OK ){
    rc = fts3IncrmergeHintStore(p, &hint);
  }

  sqlite3_free(pWriter);
  sqlite3_free(hint.a);
  return rc;
}

/*
** Convert the text beginning at *pz into an integer and return
** its value.  Advance *pz to point to the first character past
** the integer.
*/
static int fts3Getint(const char **pz){
  const char *z = *pz;
  int i = 0;
  while( (*z)>='0' && (*z)<='9' ) i = 10*i + *(z++) - '0';
  *pz = z;
  return i;
}

/*
** Process statements of the form:
**
**    INSERT INTO table(table) VALUES('merge=A,B');
**
** A and B are integers that decode to be the number of leaf pages
** written for the merge, and the minimum number of segments on a level
** before it will be selected for a merge, respectively.
*/
static int fts3DoIncrmerge(
  Fts3Table *p,                   /* FTS3 table handle */
  const char *zParam              /* Nul-terminated string containing "A,B" */
){
  int rc;
  int nMin = (FTS3_MERGE_COUNT / 2);
  int nMerge = 0;
  const char *z = zParam;

  /* Read the first integer value */
  nMerge = fts3Getint(&z);

  /* If the first integer value is followed by a ',',  read the second
  ** integer value. */
  if( z[0]==',' && z[1]!='\0' ){
    z++;
    nMin = fts3Getint(&z);
  }

  if( z[0]!='\0' || nMin<2 ){
    rc = SQLITE_ERROR;
  }else{
    rc = SQLITE_OK;
    if( !p->bHasStat ){
      assert( p->bFts4==0 );
      sqlite3Fts3CreateStatTable(&rc, p);
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts3Incrmerge(p, nMerge, nMin);
    }
    sqlite3Fts3SegmentsClose(p);
  }
  return rc;
}

/*
** Process statements of the form:
**
**    INSERT INTO table(table) VALUES('automerge=X');
**
** where X is an integer.  X==0 means to turn automerge off.  X!=0 means
** turn it on.  The setting is persistent.
*/
static int fts3DoAutoincrmerge(
  Fts3Table *p,                   /* FTS3 table handle */
  const char *zParam              /* Nul-terminated string containing boolean */
){
  int rc = SQLITE_OK;
  sqlite3_stmt *pStmt = 0;
  p->nAutoincrmerge = fts3Getint(&zParam);
  if( p->nAutoincrmerge==1 || p->nAutoincrmerge>FTS3_MERGE_COUNT ){
    p->nAutoincrmerge = 8;
  }
  if( !p->bHasStat ){
    assert( p->bFts4==0 );
    sqlite3Fts3CreateStatTable(&rc, p);
    if( rc ) return rc;
  }
  rc = fts3SqlStmt(p, SQL_REPLACE_STAT, &pStmt, 0);
  if( rc ) return rc;
  sqlite3_bind_int(pStmt, 1, FTS_STAT_AUTOINCRMERGE);
  sqlite3_bind_int(pStmt, 2, p->nAutoincrmerge);
  sqlite3_step(pStmt);
  rc = sqlite3_reset(pStmt);
  return rc;
}

/*
** Return a 64-bit checksum for the FTS index entry specified by the
** arguments to this function.
*/
static u64 fts3ChecksumEntry(
  const char *zTerm,              /* Pointer to buffer containing term */
  int nTerm,                      /* Size of zTerm in bytes */
  int iLangid,                    /* Language id for current row */
  int iIndex,                     /* Index (0..Fts3Table.nIndex-1) */
  i64 iDocid,                     /* Docid for current row. */
  int iCol,                       /* Column number */
  int iPos                        /* Position */
){
  int i;
  u64 ret = (u64)iDocid;

  ret += (ret<<3) + iLangid;
  ret += (ret<<3) + iIndex;
  ret += (ret<<3) + iCol;
  ret += (ret<<3) + iPos;
  for(i=0; i<nTerm; i++) ret += (ret<<3) + zTerm[i];

  return ret;
}

/*
** Return a checksum of all entries in the FTS index that correspond to
** language id iLangid. The checksum is calculated by XORing the checksums
** of each individual entry (see fts3ChecksumEntry()) together.
**
** If successful, the checksum value is returned and *pRc set to SQLITE_OK.
** Otherwise, if an error occurs, *pRc is set to an SQLite error code. The
** return value is undefined in this case.
*/
static u64 fts3ChecksumIndex(
  Fts3Table *p,                   /* FTS3 table handle */
  int iLangid,                    /* Language id to return cksum for */
  int iIndex,                     /* Index to cksum (0..p->nIndex-1) */
  int *pRc                        /* OUT: Return code */
){
  Fts3SegFilter filter;
  Fts3MultiSegReader csr;
  int rc;
  u64 cksum = 0;

  assert( *pRc==SQLITE_OK );

  memset(&filter, 0, sizeof(filter));
  memset(&csr, 0, sizeof(csr));
  filter.flags =  FTS3_SEGMENT_REQUIRE_POS|FTS3_SEGMENT_IGNORE_EMPTY;
  filter.flags |= FTS3_SEGMENT_SCAN;

  rc = sqlite3Fts3SegReaderCursor(
      p, iLangid, iIndex, FTS3_SEGCURSOR_ALL, 0, 0, 0, 1,&csr
  );
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts3SegReaderStart(p, &csr, &filter);
  }

  if( rc==SQLITE_OK ){
    while( SQLITE_ROW==(rc = sqlite3Fts3SegReaderStep(p, &csr)) ){
      char *pCsr = csr.aDoclist;
      char *pEnd = &pCsr[csr.nDoclist];

      i64 iDocid = 0;
      i64 iCol = 0;
      i64 iPos = 0;

      pCsr += sqlite3Fts3GetVarint(pCsr, &iDocid);
      while( pCsr<pEnd ){
        i64 iVal = 0;
        pCsr += sqlite3Fts3GetVarint(pCsr, &iVal);
        if( pCsr<pEnd ){
          if( iVal==0 || iVal==1 ){
            iCol = 0;
            iPos = 0;
            if( iVal ){
              pCsr += sqlite3Fts3GetVarint(pCsr, &iCol);
            }else{
              pCsr += sqlite3Fts3GetVarint(pCsr, &iVal);
              iDocid += iVal;
            }
          }else{
            iPos += (iVal - 2);
            cksum = cksum ^ fts3ChecksumEntry(
                csr.zTerm, csr.nTerm, iLangid, iIndex, iDocid,
                (int)iCol, (int)iPos
            );
          }
        }
      }
    }
  }
  sqlite3Fts3SegReaderFinish(&csr);

  *pRc = rc;
  return cksum;
}

/*
** Check if the contents of the FTS index match the current contents of the
** content table. If no error occurs and the contents do match, set *pbOk
** to true and return SQLITE_OK. Or if the contents do not match, set *pbOk
** to false before returning.
**
** If an error occurs (e.g. an OOM or IO error), return an SQLite error 
** code. The final value of *pbOk is undefined in this case.
*/
static int fts3IntegrityCheck(Fts3Table *p, int *pbOk){
  int rc = SQLITE_OK;             /* Return code */
  u64 cksum1 = 0;                 /* Checksum based on FTS index contents */
  u64 cksum2 = 0;                 /* Checksum based on %_content contents */
  sqlite3_stmt *pAllLangid = 0;   /* Statement to return all language-ids */

  /* This block calculates the checksum according to the FTS index. */
  rc = fts3SqlStmt(p, SQL_SELECT_ALL_LANGID, &pAllLangid, 0);
  if( rc==SQLITE_OK ){
    int rc2;
    sqlite3_bind_int(pAllLangid, 1, p->nIndex);
    while( rc==SQLITE_OK && sqlite3_step(pAllLangid)==SQLITE_ROW ){
      int iLangid = sqlite3_column_int(pAllLangid, 0);
      int i;
      for(i=0; i<p->nIndex; i++){
        cksum1 = cksum1 ^ fts3ChecksumIndex(p, iLangid, i, &rc);
      }
    }
    rc2 = sqlite3_reset(pAllLangid);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  /* This block calculates the checksum according to the %_content table */
  rc = fts3SqlStmt(p, SQL_SELECT_ALL_LANGID, &pAllLangid, 0);
  if( rc==SQLITE_OK ){
    sqlite3_tokenizer_module const *pModule = p->pTokenizer->pModule;
    sqlite3_stmt *pStmt = 0;
    char *zSql;
   
    zSql = sqlite3_mprintf("SELECT %s" , p->zReadExprlist);
    if( !zSql ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlite3_prepare_v2(p->db, zSql, -1, &pStmt, 0);
      sqlite3_free(zSql);
    }

    while( rc==SQLITE_OK && SQLITE_ROW==sqlite3_step(pStmt) ){
      i64 iDocid = sqlite3_column_int64(pStmt, 0);
      int iLang = langidFromSelect(p, pStmt);
      int iCol;

      for(iCol=0; rc==SQLITE_OK && iCol<p->nColumn; iCol++){
        if( p->abNotindexed[iCol]==0 ){
          const char *zText = (const char *)sqlite3_column_text(pStmt, iCol+1);
          int nText = sqlite3_column_bytes(pStmt, iCol+1);
          sqlite3_tokenizer_cursor *pT = 0;

          rc = sqlite3Fts3OpenTokenizer(p->pTokenizer, iLang, zText, nText,&pT);
          while( rc==SQLITE_OK ){
            char const *zToken;       /* Buffer containing token */
            int nToken = 0;           /* Number of bytes in token */
            int iDum1 = 0, iDum2 = 0; /* Dummy variables */
            int iPos = 0;             /* Position of token in zText */

            rc = pModule->xNext(pT, &zToken, &nToken, &iDum1, &iDum2, &iPos);
            if( rc==SQLITE_OK ){
              int i;
              cksum2 = cksum2 ^ fts3ChecksumEntry(
                  zToken, nToken, iLang, 0, iDocid, iCol, iPos
              );
              for(i=1; i<p->nIndex; i++){
                if( p->aIndex[i].nPrefix<=nToken ){
                  cksum2 = cksum2 ^ fts3ChecksumEntry(
                      zToken, p->aIndex[i].nPrefix, iLang, i, iDocid, iCol, iPos
                  );
                }
              }
            }
          }
          if( pT ) pModule->xClose(pT);
          if( rc==SQLITE_DONE ) rc = SQLITE_OK;
        }
      }
    }

    sqlite3_finalize(pStmt);
  }

  *pbOk = (cksum1==cksum2);
  return rc;
}

/*
** Run the integrity-check. If no error occurs and the current contents of
** the FTS index are correct, return SQLITE_OK. Or, if the contents of the
** FTS index are incorrect, return SQLITE_CORRUPT_VTAB.
**
** Or, if an error (e.g. an OOM or IO error) occurs, return an SQLite 
** error code.
**
** The integrity-check works as follows. For each token and indexed token
** prefix in the document set, a 64-bit checksum is calculated (by code
** in fts3ChecksumEntry()) based on the following:
**
**     + The index number (0 for the main index, 1 for the first prefix
**       index etc.),
**     + The token (or token prefix) text itself, 
**     + The language-id of the row it appears in,
**     + The docid of the row it appears in,
**     + The column it appears in, and
**     + The tokens position within that column.
**
** The checksums for all entries in the index are XORed together to create
** a single checksum for the entire index.
**
** The integrity-check code calculates the same checksum in two ways:
**
**     1. By scanning the contents of the FTS index, and 
**     2. By scanning and tokenizing the content table.
**
** If the two checksums are identical, the integrity-check is deemed to have
** passed.
*/
static int fts3DoIntegrityCheck(
  Fts3Table *p                    /* FTS3 table handle */
){
  int rc;
  int bOk = 0;
  rc = fts3IntegrityCheck(p, &bOk);
  if( rc==SQLITE_OK && bOk==0 ) rc = SQLITE_CORRUPT_VTAB;
  return rc;
}

/*
** Handle a 'special' INSERT of the form:
**
**   "INSERT INTO tbl(tbl) VALUES(<expr>)"
**
** Argument pVal contains the result of <expr>. Currently the only 
** meaningful value to insert is the text 'optimize'.
*/
static int fts3SpecialInsert(Fts3Table *p, sqlite3_value *pVal){
  int rc;                         /* Return Code */
  const char *zVal = (const char *)sqlite3_value_text(pVal);
  int nVal = sqlite3_value_bytes(pVal);

  if( !zVal ){
    return SQLITE_NOMEM;
  }else if( nVal==8 && 0==sqlite3_strnicmp(zVal, "optimize", 8) ){
    rc = fts3DoOptimize(p, 0);
  }else if( nVal==7 && 0==sqlite3_strnicmp(zVal, "rebuild", 7) ){
    rc = fts3DoRebuild(p);
  }else if( nVal==15 && 0==sqlite3_strnicmp(zVal, "integrity-check", 15) ){
    rc = fts3DoIntegrityCheck(p);
  }else if( nVal>6 && 0==sqlite3_strnicmp(zVal, "merge=", 6) ){
    rc = fts3DoIncrmerge(p, &zVal[6]);
  }else if( nVal>10 && 0==sqlite3_strnicmp(zVal, "automerge=", 10) ){
    rc = fts3DoAutoincrmerge(p, &zVal[10]);
#ifdef SQLITE_TEST
  }else if( nVal>9 && 0==sqlite3_strnicmp(zVal, "nodesize=", 9) ){
    p->nNodeSize = atoi(&zVal[9]);
    rc = SQLITE_OK;
  }else if( nVal>11 && 0==sqlite3_strnicmp(zVal, "maxpending=", 9) ){
    p->nMaxPendingData = atoi(&zVal[11]);
    rc = SQLITE_OK;
  }else if( nVal>21 && 0==sqlite3_strnicmp(zVal, "test-no-incr-doclist=", 21) ){
    p->bNoIncrDoclist = atoi(&zVal[21]);
    rc = SQLITE_OK;
#endif
  }else{
    rc = SQLITE_ERROR;
  }

  return rc;
}

#ifndef SQLITE_DISABLE_FTS4_DEFERRED
/*
** Delete all cached deferred doclists. Deferred doclists are cached
** (allocated) by the sqlite3Fts3CacheDeferredDoclists() function.
*/
SQLITE_PRIVATE void sqlite3Fts3FreeDeferredDoclists(Fts3Cursor *pCsr){
  Fts3DeferredToken *pDef;
  for(pDef=pCsr->pDeferred; pDef; pDef=pDef->pNext){
    fts3PendingListDelete(pDef->pList);
    pDef->pList = 0;
  }
}

/*
** Free all entries in the pCsr->pDeffered list. Entries are added to 
** this list using sqlite3Fts3DeferToken().
*/
SQLITE_PRIVATE void sqlite3Fts3FreeDeferredTokens(Fts3Cursor *pCsr){
  Fts3DeferredToken *pDef;
  Fts3DeferredToken *pNext;
  for(pDef=pCsr->pDeferred; pDef; pDef=pNext){
    pNext = pDef->pNext;
    fts3PendingListDelete(pDef->pList);
    sqlite3_free(pDef);
  }
  pCsr->pDeferred = 0;
}

/*
** Generate deferred-doclists for all tokens in the pCsr->pDeferred list
** based on the row that pCsr currently points to.
**
** A deferred-doclist is like any other doclist with position information
** included, except that it only contains entries for a single row of the
** table, not for all rows.
*/
SQLITE_PRIVATE int sqlite3Fts3CacheDeferredDoclists(Fts3Cursor *pCsr){
  int rc = SQLITE_OK;             /* Return code */
  if( pCsr->pDeferred ){
    int i;                        /* Used to iterate through table columns */
    sqlite3_int64 iDocid;         /* Docid of the row pCsr points to */
    Fts3DeferredToken *pDef;      /* Used to iterate through deferred tokens */
  
    Fts3Table *p = (Fts3Table *)pCsr->base.pVtab;
    sqlite3_tokenizer *pT = p->pTokenizer;
    sqlite3_tokenizer_module const *pModule = pT->pModule;
   
    assert( pCsr->isRequireSeek==0 );
    iDocid = sqlite3_column_int64(pCsr->pStmt, 0);
  
    for(i=0; i<p->nColumn && rc==SQLITE_OK; i++){
      if( p->abNotindexed[i]==0 ){
        const char *zText = (const char *)sqlite3_column_text(pCsr->pStmt, i+1);
        sqlite3_tokenizer_cursor *pTC = 0;

        rc = sqlite3Fts3OpenTokenizer(pT, pCsr->iLangid, zText, -1, &pTC);
        while( rc==SQLITE_OK ){
          char const *zToken;       /* Buffer containing token */
          int nToken = 0;           /* Number of bytes in token */
          int iDum1 = 0, iDum2 = 0; /* Dummy variables */
          int iPos = 0;             /* Position of token in zText */

          rc = pModule->xNext(pTC, &zToken, &nToken, &iDum1, &iDum2, &iPos);
          for(pDef=pCsr->pDeferred; pDef && rc==SQLITE_OK; pDef=pDef->pNext){
            Fts3PhraseToken *pPT = pDef->pToken;
            if( (pDef->iCol>=p->nColumn || pDef->iCol==i)
                && (pPT->bFirst==0 || iPos==0)
                && (pPT->n==nToken || (pPT->isPrefix && pPT->n<nToken))
                && (0==memcmp(zToken, pPT->z, pPT->n))
              ){
              fts3PendingListAppend(&pDef->pList, iDocid, i, iPos, &rc);
            }
          }
        }
        if( pTC ) pModule->xClose(pTC);
        if( rc==SQLITE_DONE ) rc = SQLITE_OK;
      }
    }

    for(pDef=pCsr->pDeferred; pDef && rc==SQLITE_OK; pDef=pDef->pNext){
      if( pDef->pList ){
        rc = fts3PendingListAppendVarint(&pDef->pList, 0);
      }
    }
  }

  return rc;
}

SQLITE_PRIVATE int sqlite3Fts3DeferredTokenList(
  Fts3DeferredToken *p, 
  char **ppData, 
  int *pnData
){
  char *pRet;
  int nSkip;
  sqlite3_int64 dummy;

  *ppData = 0;
  *pnData = 0;

  if( p->pList==0 ){
    return SQLITE_OK;
  }

  pRet = (char *)sqlite3_malloc(p->pList->nData);
  if( !pRet ) return SQLITE_NOMEM;

  nSkip = sqlite3Fts3GetVarint(p->pList->aData, &dummy);
  *pnData = p->pList->nData - nSkip;
  *ppData = pRet;
  
  memcpy(pRet, &p->pList->aData[nSkip], *pnData);
  return SQLITE_OK;
}

/*
** Add an entry for token pToken to the pCsr->pDeferred list.
*/
SQLITE_PRIVATE int sqlite3Fts3DeferToken(
  Fts3Cursor *pCsr,               /* Fts3 table cursor */
  Fts3PhraseToken *pToken,        /* Token to defer */
  int iCol                        /* Column that token must appear in (or -1) */
){
  Fts3DeferredToken *pDeferred;
  pDeferred = sqlite3_malloc(sizeof(*pDeferred));
  if( !pDeferred ){
    return SQLITE_NOMEM;
  }
  memset(pDeferred, 0, sizeof(*pDeferred));
  pDeferred->pToken = pToken;
  pDeferred->pNext = pCsr->pDeferred; 
  pDeferred->iCol = iCol;
  pCsr->pDeferred = pDeferred;

  assert( pToken->pDeferred==0 );
  pToken->pDeferred = pDeferred;

  return SQLITE_OK;
}
#endif

/*
** SQLite value pRowid contains the rowid of a row that may or may not be
** present in the FTS3 table. If it is, delete it and adjust the contents
** of subsiduary data structures accordingly.
*/
static int fts3DeleteByRowid(
  Fts3Table *p, 
  sqlite3_value *pRowid, 
  int *pnChng,                    /* IN/OUT: Decrement if row is deleted */
  u32 *aSzDel
){
  int rc = SQLITE_OK;             /* Return code */
  int bFound = 0;                 /* True if *pRowid really is in the table */

  fts3DeleteTerms(&rc, p, pRowid, aSzDel, &bFound);
  if( bFound && rc==SQLITE_OK ){
    int isEmpty = 0;              /* Deleting *pRowid leaves the table empty */
    rc = fts3IsEmpty(p, pRowid, &isEmpty);
    if( rc==SQLITE_OK ){
      if( isEmpty ){
        /* Deleting this row means the whole table is empty. In this case
        ** delete the contents of all three tables and throw away any
        ** data in the pendingTerms hash table.  */
        rc = fts3DeleteAll(p, 1);
        *pnChng = 0;
        memset(aSzDel, 0, sizeof(u32) * (p->nColumn+1) * 2);
      }else{
        *pnChng = *pnChng - 1;
        if( p->zContentTbl==0 ){
          fts3SqlExec(&rc, p, SQL_DELETE_CONTENT, &pRowid);
        }
        if( p->bHasDocsize ){
          fts3SqlExec(&rc, p, SQL_DELETE_DOCSIZE, &pRowid);
        }
      }
    }
  }

  return rc;
}

/*
** This function does the work for the xUpdate method of FTS3 virtual
** tables. The schema of the virtual table being:
**
**     CREATE TABLE <table name>( 
**       <user columns>,
**       <table name> HIDDEN, 
**       docid HIDDEN, 
**       <langid> HIDDEN
**     );
**
** 
*/
SQLITE_PRIVATE int sqlite3Fts3UpdateMethod(
  sqlite3_vtab *pVtab,            /* FTS3 vtab object */
  int nArg,                       /* Size of argument array */
  sqlite3_value **apVal,          /* Array of arguments */
  sqlite_int64 *pRowid            /* OUT: The affected (or effected) rowid */
){
  Fts3Table *p = (Fts3Table *)pVtab;
  int rc = SQLITE_OK;             /* Return Code */
  int isRemove = 0;               /* True for an UPDATE or DELETE */
  u32 *aSzIns = 0;                /* Sizes of inserted documents */
  u32 *aSzDel = 0;                /* Sizes of deleted documents */
  int nChng = 0;                  /* Net change in number of documents */
  int bInsertDone = 0;

  /* At this point it must be known if the %_stat table exists or not.
  ** So bHasStat may not be 2.  */
  assert( p->bHasStat==0 || p->bHasStat==1 );

  assert( p->pSegments==0 );
  assert( 
      nArg==1                     /* DELETE operations */
   || nArg==(2 + p->nColumn + 3)  /* INSERT or UPDATE operations */
  );

  /* Check for a "special" INSERT operation. One of the form:
  **
  **   INSERT INTO xyz(xyz) VALUES('command');
  */
  if( nArg>1 
   && sqlite3_value_type(apVal[0])==SQLITE_NULL 
   && sqlite3_value_type(apVal[p->nColumn+2])!=SQLITE_NULL 
  ){
    rc = fts3SpecialInsert(p, apVal[p->nColumn+2]);
    goto update_out;
  }

  if( nArg>1 && sqlite3_value_int(apVal[2 + p->nColumn + 2])<0 ){
    rc = SQLITE_CONSTRAINT;
    goto update_out;
  }

  /* Allocate space to hold the change in document sizes */
  aSzDel = sqlite3_malloc( sizeof(aSzDel[0])*(p->nColumn+1)*2 );
  if( aSzDel==0 ){
    rc = SQLITE_NOMEM;
    goto update_out;
  }
  aSzIns = &aSzDel[p->nColumn+1];
  memset(aSzDel, 0, sizeof(aSzDel[0])*(p->nColumn+1)*2);

  rc = fts3Writelock(p);
  if( rc!=SQLITE_OK ) goto update_out;

  /* If this is an INSERT operation, or an UPDATE that modifies the rowid
  ** value, then this operation requires constraint handling.
  **
  ** If the on-conflict mode is REPLACE, this means that the existing row
  ** should be deleted from the database before inserting the new row. Or,
  ** if the on-conflict mode is other than REPLACE, then this method must
  ** detect the conflict and return SQLITE_CONSTRAINT before beginning to
  ** modify the database file.
  */
  if( nArg>1 && p->zContentTbl==0 ){
    /* Find the value object that holds the new rowid value. */
    sqlite3_value *pNewRowid = apVal[3+p->nColumn];
    if( sqlite3_value_type(pNewRowid)==SQLITE_NULL ){
      pNewRowid = apVal[1];
    }

    if( sqlite3_value_type(pNewRowid)!=SQLITE_NULL && ( 
        sqlite3_value_type(apVal[0])==SQLITE_NULL
     || sqlite3_value_int64(apVal[0])!=sqlite3_value_int64(pNewRowid)
    )){
      /* The new rowid is not NULL (in this case the rowid will be
      ** automatically assigned and there is no chance of a conflict), and 
      ** the statement is either an INSERT or an UPDATE that modifies the
      ** rowid column. So if the conflict mode is REPLACE, then delete any
      ** existing row with rowid=pNewRowid. 
      **
      ** Or, if the conflict mode is not REPLACE, insert the new record into 
      ** the %_content table. If we hit the duplicate rowid constraint (or any
      ** other error) while doing so, return immediately.
      **
      ** This branch may also run if pNewRowid contains a value that cannot
      ** be losslessly converted to an integer. In this case, the eventual 
      ** call to fts3InsertData() (either just below or further on in this
      ** function) will return SQLITE_MISMATCH. If fts3DeleteByRowid is 
      ** invoked, it will delete zero rows (since no row will have
      ** docid=$pNewRowid if $pNewRowid is not an integer value).
      */
      if( sqlite3_vtab_on_conflict(p->db)==SQLITE_REPLACE ){
        rc = fts3DeleteByRowid(p, pNewRowid, &nChng, aSzDel);
      }else{
        rc = fts3InsertData(p, apVal, pRowid);
        bInsertDone = 1;
      }
    }
  }
  if( rc!=SQLITE_OK ){
    goto update_out;
  }

  /* If this is a DELETE or UPDATE operation, remove the old record. */
  if( sqlite3_value_type(apVal[0])!=SQLITE_NULL ){
    assert( sqlite3_value_type(apVal[0])==SQLITE_INTEGER );
    rc = fts3DeleteByRowid(p, apVal[0], &nChng, aSzDel);
    isRemove = 1;
  }
  
  /* If this is an INSERT or UPDATE operation, insert the new record. */
  if( nArg>1 && rc==SQLITE_OK ){
    int iLangid = sqlite3_value_int(apVal[2 + p->nColumn + 2]);
    if( bInsertDone==0 ){
      rc = fts3InsertData(p, apVal, pRowid);
      if( rc==SQLITE_CONSTRAINT && p->zContentTbl==0 ){
        rc = FTS_CORRUPT_VTAB;
      }
    }
    if( rc==SQLITE_OK && (!isRemove || *pRowid!=p->iPrevDocid ) ){
      rc = fts3PendingTermsDocid(p, iLangid, *pRowid);
    }
    if( rc==SQLITE_OK ){
      assert( p->iPrevDocid==*pRowid );
      rc = fts3InsertTerms(p, iLangid, apVal, aSzIns);
    }
    if( p->bHasDocsize ){
      fts3InsertDocsize(&rc, p, aSzIns);
    }
    nChng++;
  }

  if( p->bFts4 ){
    fts3UpdateDocTotals(&rc, p, aSzIns, aSzDel, nChng);
  }

 update_out:
  sqlite3_free(aSzDel);
  sqlite3Fts3SegmentsClose(p);
  return rc;
}

/* 
** Flush any data in the pending-terms hash table to disk. If successful,
** merge all segments in the database (including the new segment, if 
** there was any data to flush) into a single segment. 
*/
SQLITE_PRIVATE int sqlite3Fts3Optimize(Fts3Table *p){
  int rc;
  rc = sqlite3_exec(p->db, "SAVEPOINT fts3", 0, 0, 0);
  if( rc==SQLITE_OK ){
    rc = fts3DoOptimize(p, 1);
    if( rc==SQLITE_OK || rc==SQLITE_DONE ){
      int rc2 = sqlite3_exec(p->db, "RELEASE fts3", 0, 0, 0);
      if( rc2!=SQLITE_OK ) rc = rc2;
    }else{
      sqlite3_exec(p->db, "ROLLBACK TO fts3", 0, 0, 0);
      sqlite3_exec(p->db, "RELEASE fts3", 0, 0, 0);
    }
  }
  sqlite3Fts3SegmentsClose(p);
  return rc;
}

#endif

/************** End of fts3_write.c ******************************************/
/************** Begin file fts3_snippet.c ************************************/
/*
** 2009 Oct 23
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
*/

#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <string.h> */
/* #include <assert.h> */

/*
** Characters that may appear in the second argument to matchinfo().
*/
#define FTS3_MATCHINFO_NPHRASE   'p'        /* 1 value */
#define FTS3_MATCHINFO_NCOL      'c'        /* 1 value */
#define FTS3_MATCHINFO_NDOC      'n'        /* 1 value */
#define FTS3_MATCHINFO_AVGLENGTH 'a'        /* nCol values */
#define FTS3_MATCHINFO_LENGTH    'l'        /* nCol values */
#define FTS3_MATCHINFO_LCS       's'        /* nCol values */
#define FTS3_MATCHINFO_HITS      'x'        /* 3*nCol*nPhrase values */

/*
** The default value for the second argument to matchinfo(). 
*/
#define FTS3_MATCHINFO_DEFAULT   "pcx"


/*
** Used as an fts3ExprIterate() context when loading phrase doclists to
** Fts3Expr.aDoclist[]/nDoclist.
*/
typedef struct LoadDoclistCtx LoadDoclistCtx;
struct LoadDoclistCtx {
  Fts3Cursor *pCsr;               /* FTS3 Cursor */
  int nPhrase;                    /* Number of phrases seen so far */
  int nToken;                     /* Number of tokens seen so far */
};

/*
** The following types are used as part of the implementation of the 
** fts3BestSnippet() routine.
*/
typedef struct SnippetIter SnippetIter;
typedef struct SnippetPhrase SnippetPhrase;
typedef struct SnippetFragment SnippetFragment;

struct SnippetIter {
  Fts3Cursor *pCsr;               /* Cursor snippet is being generated from */
  int iCol;                       /* Extract snippet from this column */
  int nSnippet;                   /* Requested snippet length (in tokens) */
  int nPhrase;                    /* Number of phrases in query */
  SnippetPhrase *aPhrase;         /* Array of size nPhrase */
  int iCurrent;                   /* First token of current snippet */
};

struct SnippetPhrase {
  int nToken;                     /* Number of tokens in phrase */
  char *pList;                    /* Pointer to start of phrase position list */
  int iHead;                      /* Next value in position list */
  char *pHead;                    /* Position list data following iHead */
  int iTail;                      /* Next value in trailing position list */
  char *pTail;                    /* Position list data following iTail */
};

struct SnippetFragment {
  int iCol;                       /* Column snippet is extracted from */
  int iPos;                       /* Index of first token in snippet */
  u64 covered;                    /* Mask of query phrases covered */
  u64 hlmask;                     /* Mask of snippet terms to highlight */
};

/*
** This type is used as an fts3ExprIterate() context object while 
** accumulating the data returned by the matchinfo() function.
*/
typedef struct MatchInfo MatchInfo;
struct MatchInfo {
  Fts3Cursor *pCursor;            /* FTS3 Cursor */
  int nCol;                       /* Number of columns in table */
  int nPhrase;                    /* Number of matchable phrases in query */
  sqlite3_int64 nDoc;             /* Number of docs in database */
  u32 *aMatchinfo;                /* Pre-allocated buffer */
};



/*
** The snippet() and offsets() functions both return text values. An instance
** of the following structure is used to accumulate those values while the
** functions are running. See fts3StringAppend() for details.
*/
typedef struct StrBuffer StrBuffer;
struct StrBuffer {
  char *z;                        /* Pointer to buffer containing string */
  int n;                          /* Length of z in bytes (excl. nul-term) */
  int nAlloc;                     /* Allocated size of buffer z in bytes */
};


/*
** This function is used to help iterate through a position-list. A position
** list is a list of unique integers, sorted from smallest to largest. Each
** element of the list is represented by an FTS3 varint that takes the value
** of the difference between the current element and the previous one plus
** two. For example, to store the position-list:
**
**     4 9 113
**
** the three varints:
**
**     6 7 106
**
** are encoded.
**
** When this function is called, *pp points to the start of an element of
** the list. *piPos contains the value of the previous entry in the list.
** After it returns, *piPos contains the value of the next element of the
** list and *pp is advanced to the following varint.
*/
static void fts3GetDeltaPosition(char **pp, int *piPos){
  int iVal;
  *pp += fts3GetVarint32(*pp, &iVal);
  *piPos += (iVal-2);
}

/*
** Helper function for fts3ExprIterate() (see below).
*/
static int fts3ExprIterate2(
  Fts3Expr *pExpr,                /* Expression to iterate phrases of */
  int *piPhrase,                  /* Pointer to phrase counter */
  int (*x)(Fts3Expr*,int,void*),  /* Callback function to invoke for phrases */
  void *pCtx                      /* Second argument to pass to callback */
){
  int rc;                         /* Return code */
  int eType = pExpr->eType;       /* Type of expression node pExpr */

  if( eType!=FTSQUERY_PHRASE ){
    assert( pExpr->pLeft && pExpr->pRight );
    rc = fts3ExprIterate2(pExpr->pLeft, piPhrase, x, pCtx);
    if( rc==SQLITE_OK && eType!=FTSQUERY_NOT ){
      rc = fts3ExprIterate2(pExpr->pRight, piPhrase, x, pCtx);
    }
  }else{
    rc = x(pExpr, *piPhrase, pCtx);
    (*piPhrase)++;
  }
  return rc;
}

/*
** Iterate through all phrase nodes in an FTS3 query, except those that
** are part of a sub-tree that is the right-hand-side of a NOT operator.
** For each phrase node found, the supplied callback function is invoked.
**
** If the callback function returns anything other than SQLITE_OK, 
** the iteration is abandoned and the error code returned immediately.
** Otherwise, SQLITE_OK is returned after a callback has been made for
** all eligible phrase nodes.
*/
static int fts3ExprIterate(
  Fts3Expr *pExpr,                /* Expression to iterate phrases of */
  int (*x)(Fts3Expr*,int,void*),  /* Callback function to invoke for phrases */
  void *pCtx                      /* Second argument to pass to callback */
){
  int iPhrase = 0;                /* Variable used as the phrase counter */
  return fts3ExprIterate2(pExpr, &iPhrase, x, pCtx);
}

/*
** This is an fts3ExprIterate() callback used while loading the doclists
** for each phrase into Fts3Expr.aDoclist[]/nDoclist. See also
** fts3ExprLoadDoclists().
*/
static int fts3ExprLoadDoclistsCb(Fts3Expr *pExpr, int iPhrase, void *ctx){
  int rc = SQLITE_OK;
  Fts3Phrase *pPhrase = pExpr->pPhrase;
  LoadDoclistCtx *p = (LoadDoclistCtx *)ctx;

  UNUSED_PARAMETER(iPhrase);

  p->nPhrase++;
  p->nToken += pPhrase->nToken;

  return rc;
}

/*
** Load the doclists for each phrase in the query associated with FTS3 cursor
** pCsr. 
**
** If pnPhrase is not NULL, then *pnPhrase is set to the number of matchable 
** phrases in the expression (all phrases except those directly or 
** indirectly descended from the right-hand-side of a NOT operator). If 
** pnToken is not NULL, then it is set to the number of tokens in all
** matchable phrases of the expression.
*/
static int fts3ExprLoadDoclists(
  Fts3Cursor *pCsr,               /* Fts3 cursor for current query */
  int *pnPhrase,                  /* OUT: Number of phrases in query */
  int *pnToken                    /* OUT: Number of tokens in query */
){
  int rc;                         /* Return Code */
  LoadDoclistCtx sCtx = {0,0,0};  /* Context for fts3ExprIterate() */
  sCtx.pCsr = pCsr;
  rc = fts3ExprIterate(pCsr->pExpr, fts3ExprLoadDoclistsCb, (void *)&sCtx);
  if( pnPhrase ) *pnPhrase = sCtx.nPhrase;
  if( pnToken ) *pnToken = sCtx.nToken;
  return rc;
}

static int fts3ExprPhraseCountCb(Fts3Expr *pExpr, int iPhrase, void *ctx){
  (*(int *)ctx)++;
  UNUSED_PARAMETER(pExpr);
  UNUSED_PARAMETER(iPhrase);
  return SQLITE_OK;
}
static int fts3ExprPhraseCount(Fts3Expr *pExpr){
  int nPhrase = 0;
  (void)fts3ExprIterate(pExpr, fts3ExprPhraseCountCb, (void *)&nPhrase);
  return nPhrase;
}

/*
** Advance the position list iterator specified by the first two 
** arguments so that it points to the first element with a value greater
** than or equal to parameter iNext.
*/
static void fts3SnippetAdvance(char **ppIter, int *piIter, int iNext){
  char *pIter = *ppIter;
  if( pIter ){
    int iIter = *piIter;

    while( iIter<iNext ){
      if( 0==(*pIter & 0xFE) ){
        iIter = -1;
        pIter = 0;
        break;
      }
      fts3GetDeltaPosition(&pIter, &iIter);
    }

    *piIter = iIter;
    *ppIter = pIter;
  }
}

/*
** Advance the snippet iterator to the next candidate snippet.
*/
static int fts3SnippetNextCandidate(SnippetIter *pIter){
  int i;                          /* Loop counter */

  if( pIter->iCurrent<0 ){
    /* The SnippetIter object has just been initialized. The first snippet
    ** candidate always starts at offset 0 (even if this candidate has a
    ** score of 0.0).
    */
    pIter->iCurrent = 0;

    /* Advance the 'head' iterator of each phrase to the first offset that
    ** is greater than or equal to (iNext+nSnippet).
    */
    for(i=0; i<pIter->nPhrase; i++){
      SnippetPhrase *pPhrase = &pIter->aPhrase[i];
      fts3SnippetAdvance(&pPhrase->pHead, &pPhrase->iHead, pIter->nSnippet);
    }
  }else{
    int iStart;
    int iEnd = 0x7FFFFFFF;

    for(i=0; i<pIter->nPhrase; i++){
      SnippetPhrase *pPhrase = &pIter->aPhrase[i];
      if( pPhrase->pHead && pPhrase->iHead<iEnd ){
        iEnd = pPhrase->iHead;
      }
    }
    if( iEnd==0x7FFFFFFF ){
      return 1;
    }

    pIter->iCurrent = iStart = iEnd - pIter->nSnippet + 1;
    for(i=0; i<pIter->nPhrase; i++){
      SnippetPhrase *pPhrase = &pIter->aPhrase[i];
      fts3SnippetAdvance(&pPhrase->pHead, &pPhrase->iHead, iEnd+1);
      fts3SnippetAdvance(&pPhrase->pTail, &pPhrase->iTail, iStart);
    }
  }

  return 0;
}

/*
** Retrieve information about the current candidate snippet of snippet 
** iterator pIter.
*/
static void fts3SnippetDetails(
  SnippetIter *pIter,             /* Snippet iterator */
  u64 mCovered,                   /* Bitmask of phrases already covered */
  int *piToken,                   /* OUT: First token of proposed snippet */
  int *piScore,                   /* OUT: "Score" for this snippet */
  u64 *pmCover,                   /* OUT: Bitmask of phrases covered */
  u64 *pmHighlight                /* OUT: Bitmask of terms to highlight */
){
  int iStart = pIter->iCurrent;   /* First token of snippet */
  int iScore = 0;                 /* Score of this snippet */
  int i;                          /* Loop counter */
  u64 mCover = 0;                 /* Mask of phrases covered by this snippet */
  u64 mHighlight = 0;             /* Mask of tokens to highlight in snippet */

  for(i=0; i<pIter->nPhrase; i++){
    SnippetPhrase *pPhrase = &pIter->aPhrase[i];
    if( pPhrase->pTail ){
      char *pCsr = pPhrase->pTail;
      int iCsr = pPhrase->iTail;

      while( iCsr<(iStart+pIter->nSnippet) ){
        int j;
        u64 mPhrase = (u64)1 << i;
        u64 mPos = (u64)1 << (iCsr - iStart);
        assert( iCsr>=iStart );
        if( (mCover|mCovered)&mPhrase ){
          iScore++;
        }else{
          iScore += 1000;
        }
        mCover |= mPhrase;

        for(j=0; j<pPhrase->nToken; j++){
          mHighlight |= (mPos>>j);
        }

        if( 0==(*pCsr & 0x0FE) ) break;
        fts3GetDeltaPosition(&pCsr, &iCsr);
      }
    }
  }

  /* Set the output variables before returning. */
  *piToken = iStart;
  *piScore = iScore;
  *pmCover = mCover;
  *pmHighlight = mHighlight;
}

/*
** This function is an fts3ExprIterate() callback used by fts3BestSnippet().
** Each invocation populates an element of the SnippetIter.aPhrase[] array.
*/
static int fts3SnippetFindPositions(Fts3Expr *pExpr, int iPhrase, void *ctx){
  SnippetIter *p = (SnippetIter *)ctx;
  SnippetPhrase *pPhrase = &p->aPhrase[iPhrase];
  char *pCsr;
  int rc;

  pPhrase->nToken = pExpr->pPhrase->nToken;
  rc = sqlite3Fts3EvalPhrasePoslist(p->pCsr, pExpr, p->iCol, &pCsr);
  assert( rc==SQLITE_OK || pCsr==0 );
  if( pCsr ){
    int iFirst = 0;
    pPhrase->pList = pCsr;
    fts3GetDeltaPosition(&pCsr, &iFirst);
    assert( iFirst>=0 );
    pPhrase->pHead = pCsr;
    pPhrase->pTail = pCsr;
    pPhrase->iHead = iFirst;
    pPhrase->iTail = iFirst;
  }else{
    assert( rc!=SQLITE_OK || (
       pPhrase->pList==0 && pPhrase->pHead==0 && pPhrase->pTail==0 
    ));
  }

  return rc;
}

/*
** Select the fragment of text consisting of nFragment contiguous tokens 
** from column iCol that represent the "best" snippet. The best snippet
** is the snippet with the highest score, where scores are calculated
** by adding:
**
**   (a) +1 point for each occurrence of a matchable phrase in the snippet.
**
**   (b) +1000 points for the first occurrence of each matchable phrase in 
**       the snippet for which the corresponding mCovered bit is not set.
**
** The selected snippet parameters are stored in structure *pFragment before
** returning. The score of the selected snippet is stored in *piScore
** before returning.
*/
static int fts3BestSnippet(
  int nSnippet,                   /* Desired snippet length */
  Fts3Cursor *pCsr,               /* Cursor to create snippet for */
  int iCol,                       /* Index of column to create snippet from */
  u64 mCovered,                   /* Mask of phrases already covered */
  u64 *pmSeen,                    /* IN/OUT: Mask of phrases seen */
  SnippetFragment *pFragment,     /* OUT: Best snippet found */
  int *piScore                    /* OUT: Score of snippet pFragment */
){
  int rc;                         /* Return Code */
  int nList;                      /* Number of phrases in expression */
  SnippetIter sIter;              /* Iterates through snippet candidates */
  int nByte;                      /* Number of bytes of space to allocate */
  int iBestScore = -1;            /* Best snippet score found so far */
  int i;                          /* Loop counter */

  memset(&sIter, 0, sizeof(sIter));

  /* Iterate through the phrases in the expression to count them. The same
  ** callback makes sure the doclists are loaded for each phrase.
  */
  rc = fts3ExprLoadDoclists(pCsr, &nList, 0);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  /* Now that it is known how many phrases there are, allocate and zero
  ** the required space using malloc().
  */
  nByte = sizeof(SnippetPhrase) * nList;
  sIter.aPhrase = (SnippetPhrase *)sqlite3_malloc(nByte);
  if( !sIter.aPhrase ){
    return SQLITE_NOMEM;
  }
  memset(sIter.aPhrase, 0, nByte);

  /* Initialize the contents of the SnippetIter object. Then iterate through
  ** the set of phrases in the expression to populate the aPhrase[] array.
  */
  sIter.pCsr = pCsr;
  sIter.iCol = iCol;
  sIter.nSnippet = nSnippet;
  sIter.nPhrase = nList;
  sIter.iCurrent = -1;
  (void)fts3ExprIterate(pCsr->pExpr, fts3SnippetFindPositions, (void *)&sIter);

  /* Set the *pmSeen output variable. */
  for(i=0; i<nList; i++){
    if( sIter.aPhrase[i].pHead ){
      *pmSeen |= (u64)1 << i;
    }
  }

  /* Loop through all candidate snippets. Store the best snippet in 
  ** *pFragment. Store its associated 'score' in iBestScore.
  */
  pFragment->iCol = iCol;
  while( !fts3SnippetNextCandidate(&sIter) ){
    int iPos;
    int iScore;
    u64 mCover;
    u64 mHighlight;
    fts3SnippetDetails(&sIter, mCovered, &iPos, &iScore, &mCover, &mHighlight);
    assert( iScore>=0 );
    if( iScore>iBestScore ){
      pFragment->iPos = iPos;
      pFragment->hlmask = mHighlight;
      pFragment->covered = mCover;
      iBestScore = iScore;
    }
  }

  sqlite3_free(sIter.aPhrase);
  *piScore = iBestScore;
  return SQLITE_OK;
}


/*
** Append a string to the string-buffer passed as the first argument.
**
** If nAppend is negative, then the length of the string zAppend is
** determined using strlen().
*/
static int fts3StringAppend(
  StrBuffer *pStr,                /* Buffer to append to */
  const char *zAppend,            /* Pointer to data to append to buffer */
  int nAppend                     /* Size of zAppend in bytes (or -1) */
){
  if( nAppend<0 ){
    nAppend = (int)strlen(zAppend);
  }

  /* If there is insufficient space allocated at StrBuffer.z, use realloc()
  ** to grow the buffer until so that it is big enough to accomadate the
  ** appended data.
  */
  if( pStr->n+nAppend+1>=pStr->nAlloc ){
    int nAlloc = pStr->nAlloc+nAppend+100;
    char *zNew = sqlite3_realloc(pStr->z, nAlloc);
    if( !zNew ){
      return SQLITE_NOMEM;
    }
    pStr->z = zNew;
    pStr->nAlloc = nAlloc;
  }
  assert( pStr->z!=0 && (pStr->nAlloc >= pStr->n+nAppend+1) );

  /* Append the data to the string buffer. */
  memcpy(&pStr->z[pStr->n], zAppend, nAppend);
  pStr->n += nAppend;
  pStr->z[pStr->n] = '\0';

  return SQLITE_OK;
}

/*
** The fts3BestSnippet() function often selects snippets that end with a
** query term. That is, the final term of the snippet is always a term
** that requires highlighting. For example, if 'X' is a highlighted term
** and '.' is a non-highlighted term, BestSnippet() may select:
**
**     ........X.....X
**
** This function "shifts" the beginning of the snippet forward in the 
** document so that there are approximately the same number of 
** non-highlighted terms to the right of the final highlighted term as there
** are to the left of the first highlighted term. For example, to this:
**
**     ....X.....X....
**
** This is done as part of extracting the snippet text, not when selecting
** the snippet. Snippet selection is done based on doclists only, so there
** is no way for fts3BestSnippet() to know whether or not the document 
** actually contains terms that follow the final highlighted term. 
*/
static int fts3SnippetShift(
  Fts3Table *pTab,                /* FTS3 table snippet comes from */
  int iLangid,                    /* Language id to use in tokenizing */
  int nSnippet,                   /* Number of tokens desired for snippet */
  const char *zDoc,               /* Document text to extract snippet from */
  int nDoc,                       /* Size of buffer zDoc in bytes */
  int *piPos,                     /* IN/OUT: First token of snippet */
  u64 *pHlmask                    /* IN/OUT: Mask of tokens to highlight */
){
  u64 hlmask = *pHlmask;          /* Local copy of initial highlight-mask */

  if( hlmask ){
    int nLeft;                    /* Tokens to the left of first highlight */
    int nRight;                   /* Tokens to the right of last highlight */
    int nDesired;                 /* Ideal number of tokens to shift forward */

    for(nLeft=0; !(hlmask & ((u64)1 << nLeft)); nLeft++);
    for(nRight=0; !(hlmask & ((u64)1 << (nSnippet-1-nRight))); nRight++);
    nDesired = (nLeft-nRight)/2;

    /* Ideally, the start of the snippet should be pushed forward in the
    ** document nDesired tokens. This block checks if there are actually
    ** nDesired tokens to the right of the snippet. If so, *piPos and
    ** *pHlMask are updated to shift the snippet nDesired tokens to the
    ** right. Otherwise, the snippet is shifted by the number of tokens
    ** available.
    */
    if( nDesired>0 ){
      int nShift;                 /* Number of tokens to shift snippet by */
      int iCurrent = 0;           /* Token counter */
      int rc;                     /* Return Code */
      sqlite3_tokenizer_module *pMod;
      sqlite3_tokenizer_cursor *pC;
      pMod = (sqlite3_tokenizer_module *)pTab->pTokenizer->pModule;

      /* Open a cursor on zDoc/nDoc. Check if there are (nSnippet+nDesired)
      ** or more tokens in zDoc/nDoc.
      */
      rc = sqlite3Fts3OpenTokenizer(pTab->pTokenizer, iLangid, zDoc, nDoc, &pC);
      if( rc!=SQLITE_OK ){
        return rc;
      }
      while( rc==SQLITE_OK && iCurrent<(nSnippet+nDesired) ){
        const char *ZDUMMY; int DUMMY1 = 0, DUMMY2 = 0, DUMMY3 = 0;
        rc = pMod->xNext(pC, &ZDUMMY, &DUMMY1, &DUMMY2, &DUMMY3, &iCurrent);
      }
      pMod->xClose(pC);
      if( rc!=SQLITE_OK && rc!=SQLITE_DONE ){ return rc; }

      nShift = (rc==SQLITE_DONE)+iCurrent-nSnippet;
      assert( nShift<=nDesired );
      if( nShift>0 ){
        *piPos += nShift;
        *pHlmask = hlmask >> nShift;
      }
    }
  }
  return SQLITE_OK;
}

/*
** Extract the snippet text for fragment pFragment from cursor pCsr and
** append it to string buffer pOut.
*/
static int fts3SnippetText(
  Fts3Cursor *pCsr,               /* FTS3 Cursor */
  SnippetFragment *pFragment,     /* Snippet to extract */
  int iFragment,                  /* Fragment number */
  int isLast,                     /* True for final fragment in snippet */
  int nSnippet,                   /* Number of tokens in extracted snippet */
  const char *zOpen,              /* String inserted before highlighted term */
  const char *zClose,             /* String inserted after highlighted term */
  const char *zEllipsis,          /* String inserted between snippets */
  StrBuffer *pOut                 /* Write output here */
){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int rc;                         /* Return code */
  const char *zDoc;               /* Document text to extract snippet from */
  int nDoc;                       /* Size of zDoc in bytes */
  int iCurrent = 0;               /* Current token number of document */
  int iEnd = 0;                   /* Byte offset of end of current token */
  int isShiftDone = 0;            /* True after snippet is shifted */
  int iPos = pFragment->iPos;     /* First token of snippet */
  u64 hlmask = pFragment->hlmask; /* Highlight-mask for snippet */
  int iCol = pFragment->iCol+1;   /* Query column to extract text from */
  sqlite3_tokenizer_module *pMod; /* Tokenizer module methods object */
  sqlite3_tokenizer_cursor *pC;   /* Tokenizer cursor open on zDoc/nDoc */
  
  zDoc = (const char *)sqlite3_column_text(pCsr->pStmt, iCol);
  if( zDoc==0 ){
    if( sqlite3_column_type(pCsr->pStmt, iCol)!=SQLITE_NULL ){
      return SQLITE_NOMEM;
    }
    return SQLITE_OK;
  }
  nDoc = sqlite3_column_bytes(pCsr->pStmt, iCol);

  /* Open a token cursor on the document. */
  pMod = (sqlite3_tokenizer_module *)pTab->pTokenizer->pModule;
  rc = sqlite3Fts3OpenTokenizer(pTab->pTokenizer, pCsr->iLangid, zDoc,nDoc,&pC);
  if( rc!=SQLITE_OK ){
    return rc;
  }

  while( rc==SQLITE_OK ){
    const char *ZDUMMY;           /* Dummy argument used with tokenizer */
    int DUMMY1 = -1;              /* Dummy argument used with tokenizer */
    int iBegin = 0;               /* Offset in zDoc of start of token */
    int iFin = 0;                 /* Offset in zDoc of end of token */
    int isHighlight = 0;          /* True for highlighted terms */

    /* Variable DUMMY1 is initialized to a negative value above. Elsewhere
    ** in the FTS code the variable that the third argument to xNext points to
    ** is initialized to zero before the first (*but not necessarily
    ** subsequent*) call to xNext(). This is done for a particular application
    ** that needs to know whether or not the tokenizer is being used for
    ** snippet generation or for some other purpose.
    **
    ** Extreme care is required when writing code to depend on this
    ** initialization. It is not a documented part of the tokenizer interface.
    ** If a tokenizer is used directly by any code outside of FTS, this
    ** convention might not be respected.  */
    rc = pMod->xNext(pC, &ZDUMMY, &DUMMY1, &iBegin, &iFin, &iCurrent);
    if( rc!=SQLITE_OK ){
      if( rc==SQLITE_DONE ){
        /* Special case - the last token of the snippet is also the last token
        ** of the column. Append any punctuation that occurred between the end
        ** of the previous token and the end of the document to the output. 
        ** Then break out of the loop. */
        rc = fts3StringAppend(pOut, &zDoc[iEnd], -1);
      }
      break;
    }
    if( iCurrent<iPos ){ continue; }

    if( !isShiftDone ){
      int n = nDoc - iBegin;
      rc = fts3SnippetShift(
          pTab, pCsr->iLangid, nSnippet, &zDoc[iBegin], n, &iPos, &hlmask
      );
      isShiftDone = 1;

      /* Now that the shift has been done, check if the initial "..." are
      ** required. They are required if (a) this is not the first fragment,
      ** or (b) this fragment does not begin at position 0 of its column. 
      */
      if( rc==SQLITE_OK && (iPos>0 || iFragment>0) ){
        rc = fts3StringAppend(pOut, zEllipsis, -1);
      }
      if( rc!=SQLITE_OK || iCurrent<iPos ) continue;
    }

    if( iCurrent>=(iPos+nSnippet) ){
      if( isLast ){
        rc = fts3StringAppend(pOut, zEllipsis, -1);
      }
      break;
    }

    /* Set isHighlight to true if this term should be highlighted. */
    isHighlight = (hlmask & ((u64)1 << (iCurrent-iPos)))!=0;

    if( iCurrent>iPos ) rc = fts3StringAppend(pOut, &zDoc[iEnd], iBegin-iEnd);
    if( rc==SQLITE_OK && isHighlight ) rc = fts3StringAppend(pOut, zOpen, -1);
    if( rc==SQLITE_OK ) rc = fts3StringAppend(pOut, &zDoc[iBegin], iFin-iBegin);
    if( rc==SQLITE_OK && isHighlight ) rc = fts3StringAppend(pOut, zClose, -1);

    iEnd = iFin;
  }

  pMod->xClose(pC);
  return rc;
}


/*
** This function is used to count the entries in a column-list (a 
** delta-encoded list of term offsets within a single column of a single 
** row). When this function is called, *ppCollist should point to the
** beginning of the first varint in the column-list (the varint that
** contains the position of the first matching term in the column data).
** Before returning, *ppCollist is set to point to the first byte after
** the last varint in the column-list (either the 0x00 signifying the end
** of the position-list, or the 0x01 that precedes the column number of
** the next column in the position-list).
**
** The number of elements in the column-list is returned.
*/
static int fts3ColumnlistCount(char **ppCollist){
  char *pEnd = *ppCollist;
  char c = 0;
  int nEntry = 0;

  /* A column-list is terminated by either a 0x01 or 0x00. */
  while( 0xFE & (*pEnd | c) ){
    c = *pEnd++ & 0x80;
    if( !c ) nEntry++;
  }

  *ppCollist = pEnd;
  return nEntry;
}

/*
** fts3ExprIterate() callback used to collect the "global" matchinfo stats
** for a single query. 
**
** fts3ExprIterate() callback to load the 'global' elements of a
** FTS3_MATCHINFO_HITS matchinfo array. The global stats are those elements 
** of the matchinfo array that are constant for all rows returned by the 
** current query.
**
** Argument pCtx is actually a pointer to a struct of type MatchInfo. This
** function populates Matchinfo.aMatchinfo[] as follows:
**
**   for(iCol=0; iCol<nCol; iCol++){
**     aMatchinfo[3*iPhrase*nCol + 3*iCol + 1] = X;
**     aMatchinfo[3*iPhrase*nCol + 3*iCol + 2] = Y;
**   }
**
** where X is the number of matches for phrase iPhrase is column iCol of all
** rows of the table. Y is the number of rows for which column iCol contains
** at least one instance of phrase iPhrase.
**
** If the phrase pExpr consists entirely of deferred tokens, then all X and
** Y values are set to nDoc, where nDoc is the number of documents in the 
** file system. This is done because the full-text index doclist is required
** to calculate these values properly, and the full-text index doclist is
** not available for deferred tokens.
*/
static int fts3ExprGlobalHitsCb(
  Fts3Expr *pExpr,                /* Phrase expression node */
  int iPhrase,                    /* Phrase number (numbered from zero) */
  void *pCtx                      /* Pointer to MatchInfo structure */
){
  MatchInfo *p = (MatchInfo *)pCtx;
  return sqlite3Fts3EvalPhraseStats(
      p->pCursor, pExpr, &p->aMatchinfo[3*iPhrase*p->nCol]
  );
}

/*
** fts3ExprIterate() callback used to collect the "local" part of the
** FTS3_MATCHINFO_HITS array. The local stats are those elements of the 
** array that are different for each row returned by the query.
*/
static int fts3ExprLocalHitsCb(
  Fts3Expr *pExpr,                /* Phrase expression node */
  int iPhrase,                    /* Phrase number */
  void *pCtx                      /* Pointer to MatchInfo structure */
){
  int rc = SQLITE_OK;
  MatchInfo *p = (MatchInfo *)pCtx;
  int iStart = iPhrase * p->nCol * 3;
  int i;

  for(i=0; i<p->nCol && rc==SQLITE_OK; i++){
    char *pCsr;
    rc = sqlite3Fts3EvalPhrasePoslist(p->pCursor, pExpr, i, &pCsr);
    if( pCsr ){
      p->aMatchinfo[iStart+i*3] = fts3ColumnlistCount(&pCsr);
    }else{
      p->aMatchinfo[iStart+i*3] = 0;
    }
  }

  return rc;
}

static int fts3MatchinfoCheck(
  Fts3Table *pTab, 
  char cArg,
  char **pzErr
){
  if( (cArg==FTS3_MATCHINFO_NPHRASE)
   || (cArg==FTS3_MATCHINFO_NCOL)
   || (cArg==FTS3_MATCHINFO_NDOC && pTab->bFts4)
   || (cArg==FTS3_MATCHINFO_AVGLENGTH && pTab->bFts4)
   || (cArg==FTS3_MATCHINFO_LENGTH && pTab->bHasDocsize)
   || (cArg==FTS3_MATCHINFO_LCS)
   || (cArg==FTS3_MATCHINFO_HITS)
  ){
    return SQLITE_OK;
  }
  *pzErr = sqlite3_mprintf("unrecognized matchinfo request: %c", cArg);
  return SQLITE_ERROR;
}

static int fts3MatchinfoSize(MatchInfo *pInfo, char cArg){
  int nVal;                       /* Number of integers output by cArg */

  switch( cArg ){
    case FTS3_MATCHINFO_NDOC:
    case FTS3_MATCHINFO_NPHRASE: 
    case FTS3_MATCHINFO_NCOL: 
      nVal = 1;
      break;

    case FTS3_MATCHINFO_AVGLENGTH:
    case FTS3_MATCHINFO_LENGTH:
    case FTS3_MATCHINFO_LCS:
      nVal = pInfo->nCol;
      break;

    default:
      assert( cArg==FTS3_MATCHINFO_HITS );
      nVal = pInfo->nCol * pInfo->nPhrase * 3;
      break;
  }

  return nVal;
}

static int fts3MatchinfoSelectDoctotal(
  Fts3Table *pTab,
  sqlite3_stmt **ppStmt,
  sqlite3_int64 *pnDoc,
  const char **paLen
){
  sqlite3_stmt *pStmt;
  const char *a;
  sqlite3_int64 nDoc;

  if( !*ppStmt ){
    int rc = sqlite3Fts3SelectDoctotal(pTab, ppStmt);
    if( rc!=SQLITE_OK ) return rc;
  }
  pStmt = *ppStmt;
  assert( sqlite3_data_count(pStmt)==1 );

  a = sqlite3_column_blob(pStmt, 0);
  a += sqlite3Fts3GetVarint(a, &nDoc);
  if( nDoc==0 ) return FTS_CORRUPT_VTAB;
  *pnDoc = (u32)nDoc;

  if( paLen ) *paLen = a;
  return SQLITE_OK;
}

/*
** An instance of the following structure is used to store state while 
** iterating through a multi-column position-list corresponding to the
** hits for a single phrase on a single row in order to calculate the
** values for a matchinfo() FTS3_MATCHINFO_LCS request.
*/
typedef struct LcsIterator LcsIterator;
struct LcsIterator {
  Fts3Expr *pExpr;                /* Pointer to phrase expression */
  int iPosOffset;                 /* Tokens count up to end of this phrase */
  char *pRead;                    /* Cursor used to iterate through aDoclist */
  int iPos;                       /* Current position */
};

/* 
** If LcsIterator.iCol is set to the following value, the iterator has
** finished iterating through all offsets for all columns.
*/
#define LCS_ITERATOR_FINISHED 0x7FFFFFFF;

static int fts3MatchinfoLcsCb(
  Fts3Expr *pExpr,                /* Phrase expression node */
  int iPhrase,                    /* Phrase number (numbered from zero) */
  void *pCtx                      /* Pointer to MatchInfo structure */
){
  LcsIterator *aIter = (LcsIterator *)pCtx;
  aIter[iPhrase].pExpr = pExpr;
  return SQLITE_OK;
}

/*
** Advance the iterator passed as an argument to the next position. Return
** 1 if the iterator is at EOF or if it now points to the start of the
** position list for the next column.
*/
static int fts3LcsIteratorAdvance(LcsIterator *pIter){
  char *pRead = pIter->pRead;
  sqlite3_int64 iRead;
  int rc = 0;

  pRead += sqlite3Fts3GetVarint(pRead, &iRead);
  if( iRead==0 || iRead==1 ){
    pRead = 0;
    rc = 1;
  }else{
    pIter->iPos += (int)(iRead-2);
  }

  pIter->pRead = pRead;
  return rc;
}
  
/*
** This function implements the FTS3_MATCHINFO_LCS matchinfo() flag. 
**
** If the call is successful, the longest-common-substring lengths for each
** column are written into the first nCol elements of the pInfo->aMatchinfo[] 
** array before returning. SQLITE_OK is returned in this case.
**
** Otherwise, if an error occurs, an SQLite error code is returned and the
** data written to the first nCol elements of pInfo->aMatchinfo[] is 
** undefined.
*/
static int fts3MatchinfoLcs(Fts3Cursor *pCsr, MatchInfo *pInfo){
  LcsIterator *aIter;
  int i;
  int iCol;
  int nToken = 0;

  /* Allocate and populate the array of LcsIterator objects. The array
  ** contains one element for each matchable phrase in the query.
  **/
  aIter = sqlite3_malloc(sizeof(LcsIterator) * pCsr->nPhrase);
  if( !aIter ) return SQLITE_NOMEM;
  memset(aIter, 0, sizeof(LcsIterator) * pCsr->nPhrase);
  (void)fts3ExprIterate(pCsr->pExpr, fts3MatchinfoLcsCb, (void*)aIter);

  for(i=0; i<pInfo->nPhrase; i++){
    LcsIterator *pIter = &aIter[i];
    nToken -= pIter->pExpr->pPhrase->nToken;
    pIter->iPosOffset = nToken;
  }

  for(iCol=0; iCol<pInfo->nCol; iCol++){
    int nLcs = 0;                 /* LCS value for this column */
    int nLive = 0;                /* Number of iterators in aIter not at EOF */

    for(i=0; i<pInfo->nPhrase; i++){
      int rc;
      LcsIterator *pIt = &aIter[i];
      rc = sqlite3Fts3EvalPhrasePoslist(pCsr, pIt->pExpr, iCol, &pIt->pRead);
      if( rc!=SQLITE_OK ) return rc;
      if( pIt->pRead ){
        pIt->iPos = pIt->iPosOffset;
        fts3LcsIteratorAdvance(&aIter[i]);
        nLive++;
      }
    }

    while( nLive>0 ){
      LcsIterator *pAdv = 0;      /* The iterator to advance by one position */
      int nThisLcs = 0;           /* LCS for the current iterator positions */

      for(i=0; i<pInfo->nPhrase; i++){
        LcsIterator *pIter = &aIter[i];
        if( pIter->pRead==0 ){
          /* This iterator is already at EOF for this column. */
          nThisLcs = 0;
        }else{
          if( pAdv==0 || pIter->iPos<pAdv->iPos ){
            pAdv = pIter;
          }
          if( nThisLcs==0 || pIter->iPos==pIter[-1].iPos ){
            nThisLcs++;
          }else{
            nThisLcs = 1;
          }
          if( nThisLcs>nLcs ) nLcs = nThisLcs;
        }
      }
      if( fts3LcsIteratorAdvance(pAdv) ) nLive--;
    }

    pInfo->aMatchinfo[iCol] = nLcs;
  }

  sqlite3_free(aIter);
  return SQLITE_OK;
}

/*
** Populate the buffer pInfo->aMatchinfo[] with an array of integers to
** be returned by the matchinfo() function. Argument zArg contains the 
** format string passed as the second argument to matchinfo (or the
** default value "pcx" if no second argument was specified). The format
** string has already been validated and the pInfo->aMatchinfo[] array
** is guaranteed to be large enough for the output.
**
** If bGlobal is true, then populate all fields of the matchinfo() output.
** If it is false, then assume that those fields that do not change between
** rows (i.e. FTS3_MATCHINFO_NPHRASE, NCOL, NDOC, AVGLENGTH and part of HITS)
** have already been populated.
**
** Return SQLITE_OK if successful, or an SQLite error code if an error 
** occurs. If a value other than SQLITE_OK is returned, the state the
** pInfo->aMatchinfo[] buffer is left in is undefined.
*/
static int fts3MatchinfoValues(
  Fts3Cursor *pCsr,               /* FTS3 cursor object */
  int bGlobal,                    /* True to grab the global stats */
  MatchInfo *pInfo,               /* Matchinfo context object */
  const char *zArg                /* Matchinfo format string */
){
  int rc = SQLITE_OK;
  int i;
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  sqlite3_stmt *pSelect = 0;

  for(i=0; rc==SQLITE_OK && zArg[i]; i++){

    switch( zArg[i] ){
      case FTS3_MATCHINFO_NPHRASE:
        if( bGlobal ) pInfo->aMatchinfo[0] = pInfo->nPhrase;
        break;

      case FTS3_MATCHINFO_NCOL:
        if( bGlobal ) pInfo->aMatchinfo[0] = pInfo->nCol;
        break;
        
      case FTS3_MATCHINFO_NDOC:
        if( bGlobal ){
          sqlite3_int64 nDoc = 0;
          rc = fts3MatchinfoSelectDoctotal(pTab, &pSelect, &nDoc, 0);
          pInfo->aMatchinfo[0] = (u32)nDoc;
        }
        break;

      case FTS3_MATCHINFO_AVGLENGTH: 
        if( bGlobal ){
          sqlite3_int64 nDoc;     /* Number of rows in table */
          const char *a;          /* Aggregate column length array */

          rc = fts3MatchinfoSelectDoctotal(pTab, &pSelect, &nDoc, &a);
          if( rc==SQLITE_OK ){
            int iCol;
            for(iCol=0; iCol<pInfo->nCol; iCol++){
              u32 iVal;
              sqlite3_int64 nToken;
              a += sqlite3Fts3GetVarint(a, &nToken);
              iVal = (u32)(((u32)(nToken&0xffffffff)+nDoc/2)/nDoc);
              pInfo->aMatchinfo[iCol] = iVal;
            }
          }
        }
        break;

      case FTS3_MATCHINFO_LENGTH: {
        sqlite3_stmt *pSelectDocsize = 0;
        rc = sqlite3Fts3SelectDocsize(pTab, pCsr->iPrevId, &pSelectDocsize);
        if( rc==SQLITE_OK ){
          int iCol;
          const char *a = sqlite3_column_blob(pSelectDocsize, 0);
          for(iCol=0; iCol<pInfo->nCol; iCol++){
            sqlite3_int64 nToken;
            a += sqlite3Fts3GetVarint(a, &nToken);
            pInfo->aMatchinfo[iCol] = (u32)nToken;
          }
        }
        sqlite3_reset(pSelectDocsize);
        break;
      }

      case FTS3_MATCHINFO_LCS:
        rc = fts3ExprLoadDoclists(pCsr, 0, 0);
        if( rc==SQLITE_OK ){
          rc = fts3MatchinfoLcs(pCsr, pInfo);
        }
        break;

      default: {
        Fts3Expr *pExpr;
        assert( zArg[i]==FTS3_MATCHINFO_HITS );
        pExpr = pCsr->pExpr;
        rc = fts3ExprLoadDoclists(pCsr, 0, 0);
        if( rc!=SQLITE_OK ) break;
        if( bGlobal ){
          if( pCsr->pDeferred ){
            rc = fts3MatchinfoSelectDoctotal(pTab, &pSelect, &pInfo->nDoc, 0);
            if( rc!=SQLITE_OK ) break;
          }
          rc = fts3ExprIterate(pExpr, fts3ExprGlobalHitsCb,(void*)pInfo);
          if( rc!=SQLITE_OK ) break;
        }
        (void)fts3ExprIterate(pExpr, fts3ExprLocalHitsCb,(void*)pInfo);
        break;
      }
    }

    pInfo->aMatchinfo += fts3MatchinfoSize(pInfo, zArg[i]);
  }

  sqlite3_reset(pSelect);
  return rc;
}


/*
** Populate pCsr->aMatchinfo[] with data for the current row. The 
** 'matchinfo' data is an array of 32-bit unsigned integers (C type u32).
*/
static int fts3GetMatchinfo(
  Fts3Cursor *pCsr,               /* FTS3 Cursor object */
  const char *zArg                /* Second argument to matchinfo() function */
){
  MatchInfo sInfo;
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int rc = SQLITE_OK;
  int bGlobal = 0;                /* Collect 'global' stats as well as local */

  memset(&sInfo, 0, sizeof(MatchInfo));
  sInfo.pCursor = pCsr;
  sInfo.nCol = pTab->nColumn;

  /* If there is cached matchinfo() data, but the format string for the 
  ** cache does not match the format string for this request, discard 
  ** the cached data. */
  if( pCsr->zMatchinfo && strcmp(pCsr->zMatchinfo, zArg) ){
    assert( pCsr->aMatchinfo );
    sqlite3_free(pCsr->aMatchinfo);
    pCsr->zMatchinfo = 0;
    pCsr->aMatchinfo = 0;
  }

  /* If Fts3Cursor.aMatchinfo[] is NULL, then this is the first time the
  ** matchinfo function has been called for this query. In this case 
  ** allocate the array used to accumulate the matchinfo data and
  ** initialize those elements that are constant for every row.
  */
  if( pCsr->aMatchinfo==0 ){
    int nMatchinfo = 0;           /* Number of u32 elements in match-info */
    int nArg;                     /* Bytes in zArg */
    int i;                        /* Used to iterate through zArg */

    /* Determine the number of phrases in the query */
    pCsr->nPhrase = fts3ExprPhraseCount(pCsr->pExpr);
    sInfo.nPhrase = pCsr->nPhrase;

    /* Determine the number of integers in the buffer returned by this call. */
    for(i=0; zArg[i]; i++){
      nMatchinfo += fts3MatchinfoSize(&sInfo, zArg[i]);
    }

    /* Allocate space for Fts3Cursor.aMatchinfo[] and Fts3Cursor.zMatchinfo. */
    nArg = (int)strlen(zArg);
    pCsr->aMatchinfo = (u32 *)sqlite3_malloc(sizeof(u32)*nMatchinfo + nArg + 1);
    if( !pCsr->aMatchinfo ) return SQLITE_NOMEM;

    pCsr->zMatchinfo = (char *)&pCsr->aMatchinfo[nMatchinfo];
    pCsr->nMatchinfo = nMatchinfo;
    memcpy(pCsr->zMatchinfo, zArg, nArg+1);
    memset(pCsr->aMatchinfo, 0, sizeof(u32)*nMatchinfo);
    pCsr->isMatchinfoNeeded = 1;
    bGlobal = 1;
  }

  sInfo.aMatchinfo = pCsr->aMatchinfo;
  sInfo.nPhrase = pCsr->nPhrase;
  if( pCsr->isMatchinfoNeeded ){
    rc = fts3MatchinfoValues(pCsr, bGlobal, &sInfo, zArg);
    pCsr->isMatchinfoNeeded = 0;
  }

  return rc;
}

/*
** Implementation of snippet() function.
*/
SQLITE_PRIVATE void sqlite3Fts3Snippet(
  sqlite3_context *pCtx,          /* SQLite function call context */
  Fts3Cursor *pCsr,               /* Cursor object */
  const char *zStart,             /* Snippet start text - "<b>" */
  const char *zEnd,               /* Snippet end text - "</b>" */
  const char *zEllipsis,          /* Snippet ellipsis text - "<b>...</b>" */
  int iCol,                       /* Extract snippet from this column */
  int nToken                      /* Approximate number of tokens in snippet */
){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int rc = SQLITE_OK;
  int i;
  StrBuffer res = {0, 0, 0};

  /* The returned text includes up to four fragments of text extracted from
  ** the data in the current row. The first iteration of the for(...) loop
  ** below attempts to locate a single fragment of text nToken tokens in 
  ** size that contains at least one instance of all phrases in the query
  ** expression that appear in the current row. If such a fragment of text
  ** cannot be found, the second iteration of the loop attempts to locate
  ** a pair of fragments, and so on.
  */
  int nSnippet = 0;               /* Number of fragments in this snippet */
  SnippetFragment aSnippet[4];    /* Maximum of 4 fragments per snippet */
  int nFToken = -1;               /* Number of tokens in each fragment */

  if( !pCsr->pExpr ){
    sqlite3_result_text(pCtx, "", 0, SQLITE_STATIC);
    return;
  }

  for(nSnippet=1; 1; nSnippet++){

    int iSnip;                    /* Loop counter 0..nSnippet-1 */
    u64 mCovered = 0;             /* Bitmask of phrases covered by snippet */
    u64 mSeen = 0;                /* Bitmask of phrases seen by BestSnippet() */

    if( nToken>=0 ){
      nFToken = (nToken+nSnippet-1) / nSnippet;
    }else{
      nFToken = -1 * nToken;
    }

    for(iSnip=0; iSnip<nSnippet; iSnip++){
      int iBestScore = -1;        /* Best score of columns checked so far */
      int iRead;                  /* Used to iterate through columns */
      SnippetFragment *pFragment = &aSnippet[iSnip];

      memset(pFragment, 0, sizeof(*pFragment));

      /* Loop through all columns of the table being considered for snippets.
      ** If the iCol argument to this function was negative, this means all
      ** columns of the FTS3 table. Otherwise, only column iCol is considered.
      */
      for(iRead=0; iRead<pTab->nColumn; iRead++){
        SnippetFragment sF = {0, 0, 0, 0};
        int iS;
        if( iCol>=0 && iRead!=iCol ) continue;

        /* Find the best snippet of nFToken tokens in column iRead. */
        rc = fts3BestSnippet(nFToken, pCsr, iRead, mCovered, &mSeen, &sF, &iS);
        if( rc!=SQLITE_OK ){
          goto snippet_out;
        }
        if( iS>iBestScore ){
          *pFragment = sF;
          iBestScore = iS;
        }
      }

      mCovered |= pFragment->covered;
    }

    /* If all query phrases seen by fts3BestSnippet() are present in at least
    ** one of the nSnippet snippet fragments, break out of the loop.
    */
    assert( (mCovered&mSeen)==mCovered );
    if( mSeen==mCovered || nSnippet==SizeofArray(aSnippet) ) break;
  }

  assert( nFToken>0 );

  for(i=0; i<nSnippet && rc==SQLITE_OK; i++){
    rc = fts3SnippetText(pCsr, &aSnippet[i], 
        i, (i==nSnippet-1), nFToken, zStart, zEnd, zEllipsis, &res
    );
  }

 snippet_out:
  sqlite3Fts3SegmentsClose(pTab);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(pCtx, rc);
    sqlite3_free(res.z);
  }else{
    sqlite3_result_text(pCtx, res.z, -1, sqlite3_free);
  }
}


typedef struct TermOffset TermOffset;
typedef struct TermOffsetCtx TermOffsetCtx;

struct TermOffset {
  char *pList;                    /* Position-list */
  int iPos;                       /* Position just read from pList */
  int iOff;                       /* Offset of this term from read positions */
};

struct TermOffsetCtx {
  Fts3Cursor *pCsr;
  int iCol;                       /* Column of table to populate aTerm for */
  int iTerm;
  sqlite3_int64 iDocid;
  TermOffset *aTerm;
};

/*
** This function is an fts3ExprIterate() callback used by sqlite3Fts3Offsets().
*/
static int fts3ExprTermOffsetInit(Fts3Expr *pExpr, int iPhrase, void *ctx){
  TermOffsetCtx *p = (TermOffsetCtx *)ctx;
  int nTerm;                      /* Number of tokens in phrase */
  int iTerm;                      /* For looping through nTerm phrase terms */
  char *pList;                    /* Pointer to position list for phrase */
  int iPos = 0;                   /* First position in position-list */
  int rc;

  UNUSED_PARAMETER(iPhrase);
  rc = sqlite3Fts3EvalPhrasePoslist(p->pCsr, pExpr, p->iCol, &pList);
  nTerm = pExpr->pPhrase->nToken;
  if( pList ){
    fts3GetDeltaPosition(&pList, &iPos);
    assert( iPos>=0 );
  }

  for(iTerm=0; iTerm<nTerm; iTerm++){
    TermOffset *pT = &p->aTerm[p->iTerm++];
    pT->iOff = nTerm-iTerm-1;
    pT->pList = pList;
    pT->iPos = iPos;
  }

  return rc;
}

/*
** Implementation of offsets() function.
*/
SQLITE_PRIVATE void sqlite3Fts3Offsets(
  sqlite3_context *pCtx,          /* SQLite function call context */
  Fts3Cursor *pCsr                /* Cursor object */
){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  sqlite3_tokenizer_module const *pMod = pTab->pTokenizer->pModule;
  int rc;                         /* Return Code */
  int nToken;                     /* Number of tokens in query */
  int iCol;                       /* Column currently being processed */
  StrBuffer res = {0, 0, 0};      /* Result string */
  TermOffsetCtx sCtx;             /* Context for fts3ExprTermOffsetInit() */

  if( !pCsr->pExpr ){
    sqlite3_result_text(pCtx, "", 0, SQLITE_STATIC);
    return;
  }

  memset(&sCtx, 0, sizeof(sCtx));
  assert( pCsr->isRequireSeek==0 );

  /* Count the number of terms in the query */
  rc = fts3ExprLoadDoclists(pCsr, 0, &nToken);
  if( rc!=SQLITE_OK ) goto offsets_out;

  /* Allocate the array of TermOffset iterators. */
  sCtx.aTerm = (TermOffset *)sqlite3_malloc(sizeof(TermOffset)*nToken);
  if( 0==sCtx.aTerm ){
    rc = SQLITE_NOMEM;
    goto offsets_out;
  }
  sCtx.iDocid = pCsr->iPrevId;
  sCtx.pCsr = pCsr;

  /* Loop through the table columns, appending offset information to 
  ** string-buffer res for each column.
  */
  for(iCol=0; iCol<pTab->nColumn; iCol++){
    sqlite3_tokenizer_cursor *pC; /* Tokenizer cursor */
    const char *ZDUMMY;           /* Dummy argument used with xNext() */
    int NDUMMY = 0;               /* Dummy argument used with xNext() */
    int iStart = 0;
    int iEnd = 0;
    int iCurrent = 0;
    const char *zDoc;
    int nDoc;

    /* Initialize the contents of sCtx.aTerm[] for column iCol. There is 
    ** no way that this operation can fail, so the return code from
    ** fts3ExprIterate() can be discarded.
    */
    sCtx.iCol = iCol;
    sCtx.iTerm = 0;
    (void)fts3ExprIterate(pCsr->pExpr, fts3ExprTermOffsetInit, (void *)&sCtx);

    /* Retreive the text stored in column iCol. If an SQL NULL is stored 
    ** in column iCol, jump immediately to the next iteration of the loop.
    ** If an OOM occurs while retrieving the data (this can happen if SQLite
    ** needs to transform the data from utf-16 to utf-8), return SQLITE_NOMEM 
    ** to the caller. 
    */
    zDoc = (const char *)sqlite3_column_text(pCsr->pStmt, iCol+1);
    nDoc = sqlite3_column_bytes(pCsr->pStmt, iCol+1);
    if( zDoc==0 ){
      if( sqlite3_column_type(pCsr->pStmt, iCol+1)==SQLITE_NULL ){
        continue;
      }
      rc = SQLITE_NOMEM;
      goto offsets_out;
    }

    /* Initialize a tokenizer iterator to iterate through column iCol. */
    rc = sqlite3Fts3OpenTokenizer(pTab->pTokenizer, pCsr->iLangid,
        zDoc, nDoc, &pC
    );
    if( rc!=SQLITE_OK ) goto offsets_out;

    rc = pMod->xNext(pC, &ZDUMMY, &NDUMMY, &iStart, &iEnd, &iCurrent);
    while( rc==SQLITE_OK ){
      int i;                      /* Used to loop through terms */
      int iMinPos = 0x7FFFFFFF;   /* Position of next token */
      TermOffset *pTerm = 0;      /* TermOffset associated with next token */

      for(i=0; i<nToken; i++){
        TermOffset *pT = &sCtx.aTerm[i];
        if( pT->pList && (pT->iPos-pT->iOff)<iMinPos ){
          iMinPos = pT->iPos-pT->iOff;
          pTerm = pT;
        }
      }

      if( !pTerm ){
        /* All offsets for this column have been gathered. */
        rc = SQLITE_DONE;
      }else{
        assert( iCurrent<=iMinPos );
        if( 0==(0xFE&*pTerm->pList) ){
          pTerm->pList = 0;
        }else{
          fts3GetDeltaPosition(&pTerm->pList, &pTerm->iPos);
        }
        while( rc==SQLITE_OK && iCurrent<iMinPos ){
          rc = pMod->xNext(pC, &ZDUMMY, &NDUMMY, &iStart, &iEnd, &iCurrent);
        }
        if( rc==SQLITE_OK ){
          char aBuffer[64];
          sqlite3_snprintf(sizeof(aBuffer), aBuffer, 
              "%d %d %d %d ", iCol, pTerm-sCtx.aTerm, iStart, iEnd-iStart
          );
          rc = fts3StringAppend(&res, aBuffer, -1);
        }else if( rc==SQLITE_DONE && pTab->zContentTbl==0 ){
          rc = FTS_CORRUPT_VTAB;
        }
      }
    }
    if( rc==SQLITE_DONE ){
      rc = SQLITE_OK;
    }

    pMod->xClose(pC);
    if( rc!=SQLITE_OK ) goto offsets_out;
  }

 offsets_out:
  sqlite3_free(sCtx.aTerm);
  assert( rc!=SQLITE_DONE );
  sqlite3Fts3SegmentsClose(pTab);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(pCtx,  rc);
    sqlite3_free(res.z);
  }else{
    sqlite3_result_text(pCtx, res.z, res.n-1, sqlite3_free);
  }
  return;
}

/*
** Implementation of matchinfo() function.
*/
SQLITE_PRIVATE void sqlite3Fts3Matchinfo(
  sqlite3_context *pContext,      /* Function call context */
  Fts3Cursor *pCsr,               /* FTS3 table cursor */
  const char *zArg                /* Second arg to matchinfo() function */
){
  Fts3Table *pTab = (Fts3Table *)pCsr->base.pVtab;
  int rc;
  int i;
  const char *zFormat;

  if( zArg ){
    for(i=0; zArg[i]; i++){
      char *zErr = 0;
      if( fts3MatchinfoCheck(pTab, zArg[i], &zErr) ){
        sqlite3_result_error(pContext, zErr, -1);
        sqlite3_free(zErr);
        return;
      }
    }
    zFormat = zArg;
  }else{
    zFormat = FTS3_MATCHINFO_DEFAULT;
  }

  if( !pCsr->pExpr ){
    sqlite3_result_blob(pContext, "", 0, SQLITE_STATIC);
    return;
  }

  /* Retrieve matchinfo() data. */
  rc = fts3GetMatchinfo(pCsr, zFormat);
  sqlite3Fts3SegmentsClose(pTab);

  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(pContext, rc);
  }else{
    int n = pCsr->nMatchinfo * sizeof(u32);
    sqlite3_result_blob(pContext, pCsr->aMatchinfo, n, SQLITE_TRANSIENT);
  }
}

#endif

/************** End of fts3_snippet.c ****************************************/
/************** Begin file fts3_unicode.c ************************************/
/*
** 2012 May 24
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** Implementation of the "unicode" full-text-search tokenizer.
*/

#ifndef SQLITE_DISABLE_FTS3_UNICODE

#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)

/* #include <assert.h> */
/* #include <stdlib.h> */
/* #include <stdio.h> */
/* #include <string.h> */


/*
** The following two macros - READ_UTF8 and WRITE_UTF8 - have been copied
** from the sqlite3 source file utf.c. If this file is compiled as part
** of the amalgamation, they are not required.
*/
#ifndef SQLITE_AMALGAMATION

static const unsigned char sqlite3Utf8Trans1[] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x00, 0x00,
};

#define READ_UTF8(zIn, zTerm, c)                           \
  c = *(zIn++);                                            \
  if( c>=0xc0 ){                                           \
    c = sqlite3Utf8Trans1[c-0xc0];                         \
    while( zIn!=zTerm && (*zIn & 0xc0)==0x80 ){            \
      c = (c<<6) + (0x3f & *(zIn++));                      \
    }                                                      \
    if( c<0x80                                             \
        || (c&0xFFFFF800)==0xD800                          \
        || (c&0xFFFFFFFE)==0xFFFE ){  c = 0xFFFD; }        \
  }

#define WRITE_UTF8(zOut, c) {                          \
  if( c<0x00080 ){                                     \
    *zOut++ = (u8)(c&0xFF);                            \
  }                                                    \
  else if( c<0x00800 ){                                \
    *zOut++ = 0xC0 + (u8)((c>>6)&0x1F);                \
    *zOut++ = 0x80 + (u8)(c & 0x3F);                   \
  }                                                    \
  else if( c<0x10000 ){                                \
    *zOut++ = 0xE0 + (u8)((c>>12)&0x0F);               \
    *zOut++ = 0x80 + (u8)((c>>6) & 0x3F);              \
    *zOut++ = 0x80 + (u8)(c & 0x3F);                   \
  }else{                                               \
    *zOut++ = 0xF0 + (u8)((c>>18) & 0x07);             \
    *zOut++ = 0x80 + (u8)((c>>12) & 0x3F);             \
    *zOut++ = 0x80 + (u8)((c>>6) & 0x3F);              \
    *zOut++ = 0x80 + (u8)(c & 0x3F);                   \
  }                                                    \
}

#endif /* ifndef SQLITE_AMALGAMATION */

typedef struct unicode_tokenizer unicode_tokenizer;
typedef struct unicode_cursor unicode_cursor;

struct unicode_tokenizer {
  sqlite3_tokenizer base;
  int bRemoveDiacritic;
  int nException;
  int *aiException;
};

struct unicode_cursor {
  sqlite3_tokenizer_cursor base;
  const unsigned char *aInput;    /* Input text being tokenized */
  int nInput;                     /* Size of aInput[] in bytes */
  int iOff;                       /* Current offset within aInput[] */
  int iToken;                     /* Index of next token to be returned */
  char *zToken;                   /* storage for current token */
  int nAlloc;                     /* space allocated at zToken */
};


/*
** Destroy a tokenizer allocated by unicodeCreate().
*/
static int unicodeDestroy(sqlite3_tokenizer *pTokenizer){
  if( pTokenizer ){
    unicode_tokenizer *p = (unicode_tokenizer *)pTokenizer;
    sqlite3_free(p->aiException);
    sqlite3_free(p);
  }
  return SQLITE_OK;
}

/*
** As part of a tokenchars= or separators= option, the CREATE VIRTUAL TABLE
** statement has specified that the tokenizer for this table shall consider
** all characters in string zIn/nIn to be separators (if bAlnum==0) or
** token characters (if bAlnum==1).
**
** For each codepoint in the zIn/nIn string, this function checks if the
** sqlite3FtsUnicodeIsalnum() function already returns the desired result.
** If so, no action is taken. Otherwise, the codepoint is added to the 
** unicode_tokenizer.aiException[] array. For the purposes of tokenization,
** the return value of sqlite3FtsUnicodeIsalnum() is inverted for all
** codepoints in the aiException[] array.
**
** If a standalone diacritic mark (one that sqlite3FtsUnicodeIsdiacritic()
** identifies as a diacritic) occurs in the zIn/nIn string it is ignored.
** It is not possible to change the behavior of the tokenizer with respect
** to these codepoints.
*/
static int unicodeAddExceptions(
  unicode_tokenizer *p,           /* Tokenizer to add exceptions to */
  int bAlnum,                     /* Replace Isalnum() return value with this */
  const char *zIn,                /* Array of characters to make exceptions */
  int nIn                         /* Length of z in bytes */
){
  const unsigned char *z = (const unsigned char *)zIn;
  const unsigned char *zTerm = &z[nIn];
  int iCode;
  int nEntry = 0;

  assert( bAlnum==0 || bAlnum==1 );

  while( z<zTerm ){
    READ_UTF8(z, zTerm, iCode);
    assert( (sqlite3FtsUnicodeIsalnum(iCode) & 0xFFFFFFFE)==0 );
    if( sqlite3FtsUnicodeIsalnum(iCode)!=bAlnum 
     && sqlite3FtsUnicodeIsdiacritic(iCode)==0 
    ){
      nEntry++;
    }
  }

  if( nEntry ){
    int *aNew;                    /* New aiException[] array */
    int nNew;                     /* Number of valid entries in array aNew[] */

    aNew = sqlite3_realloc(p->aiException, (p->nException+nEntry)*sizeof(int));
    if( aNew==0 ) return SQLITE_NOMEM;
    nNew = p->nException;

    z = (const unsigned char *)zIn;
    while( z<zTerm ){
      READ_UTF8(z, zTerm, iCode);
      if( sqlite3FtsUnicodeIsalnum(iCode)!=bAlnum 
       && sqlite3FtsUnicodeIsdiacritic(iCode)==0
      ){
        int i, j;
        for(i=0; i<nNew && aNew[i]<iCode; i++);
        for(j=nNew; j>i; j--) aNew[j] = aNew[j-1];
        aNew[i] = iCode;
        nNew++;
      }
    }
    p->aiException = aNew;
    p->nException = nNew;
  }

  return SQLITE_OK;
}

/*
** Return true if the p->aiException[] array contains the value iCode.
*/
static int unicodeIsException(unicode_tokenizer *p, int iCode){
  if( p->nException>0 ){
    int *a = p->aiException;
    int iLo = 0;
    int iHi = p->nException-1;

    while( iHi>=iLo ){
      int iTest = (iHi + iLo) / 2;
      if( iCode==a[iTest] ){
        return 1;
      }else if( iCode>a[iTest] ){
        iLo = iTest+1;
      }else{
        iHi = iTest-1;
      }
    }
  }

  return 0;
}

/*
** Return true if, for the purposes of tokenization, codepoint iCode is
** considered a token character (not a separator).
*/
static int unicodeIsAlnum(unicode_tokenizer *p, int iCode){
  assert( (sqlite3FtsUnicodeIsalnum(iCode) & 0xFFFFFFFE)==0 );
  return sqlite3FtsUnicodeIsalnum(iCode) ^ unicodeIsException(p, iCode);
}

/*
** Create a new tokenizer instance.
*/
static int unicodeCreate(
  int nArg,                       /* Size of array argv[] */
  const char * const *azArg,      /* Tokenizer creation arguments */
  sqlite3_tokenizer **pp          /* OUT: New tokenizer handle */
){
  unicode_tokenizer *pNew;        /* New tokenizer object */
  int i;
  int rc = SQLITE_OK;

  pNew = (unicode_tokenizer *) sqlite3_malloc(sizeof(unicode_tokenizer));
  if( pNew==NULL ) return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(unicode_tokenizer));
  pNew->bRemoveDiacritic = 1;

  for(i=0; rc==SQLITE_OK && i<nArg; i++){
    const char *z = azArg[i];
    int n = (int)strlen(z);

    if( n==19 && memcmp("remove_diacritics=1", z, 19)==0 ){
      pNew->bRemoveDiacritic = 1;
    }
    else if( n==19 && memcmp("remove_diacritics=0", z, 19)==0 ){
      pNew->bRemoveDiacritic = 0;
    }
    else if( n>=11 && memcmp("tokenchars=", z, 11)==0 ){
      rc = unicodeAddExceptions(pNew, 1, &z[11], n-11);
    }
    else if( n>=11 && memcmp("separators=", z, 11)==0 ){
      rc = unicodeAddExceptions(pNew, 0, &z[11], n-11);
    }
    else{
      /* Unrecognized argument */
      rc  = SQLITE_ERROR;
    }
  }

  if( rc!=SQLITE_OK ){
    unicodeDestroy((sqlite3_tokenizer *)pNew);
    pNew = 0;
  }
  *pp = (sqlite3_tokenizer *)pNew;
  return rc;
}

/*
** Prepare to begin tokenizing a particular string.  The input
** string to be tokenized is pInput[0..nBytes-1].  A cursor
** used to incrementally tokenize this string is returned in 
** *ppCursor.
*/
static int unicodeOpen(
  sqlite3_tokenizer *p,           /* The tokenizer */
  const char *aInput,             /* Input string */
  int nInput,                     /* Size of string aInput in bytes */
  sqlite3_tokenizer_cursor **pp   /* OUT: New cursor object */
){
  unicode_cursor *pCsr;

  pCsr = (unicode_cursor *)sqlite3_malloc(sizeof(unicode_cursor));
  if( pCsr==0 ){
    return SQLITE_NOMEM;
  }
  memset(pCsr, 0, sizeof(unicode_cursor));

  pCsr->aInput = (const unsigned char *)aInput;
  if( aInput==0 ){
    pCsr->nInput = 0;
  }else if( nInput<0 ){
    pCsr->nInput = (int)strlen(aInput);
  }else{
    pCsr->nInput = nInput;
  }

  *pp = &pCsr->base;
  UNUSED_PARAMETER(p);
  return SQLITE_OK;
}

/*
** Close a tokenization cursor previously opened by a call to
** simpleOpen() above.
*/
static int unicodeClose(sqlite3_tokenizer_cursor *pCursor){
  unicode_cursor *pCsr = (unicode_cursor *) pCursor;
  sqlite3_free(pCsr->zToken);
  sqlite3_free(pCsr);
  return SQLITE_OK;
}

/*
** Extract the next token from a tokenization cursor.  The cursor must
** have been opened by a prior call to simpleOpen().
*/
static int unicodeNext(
  sqlite3_tokenizer_cursor *pC,   /* Cursor returned by simpleOpen */
  const char **paToken,           /* OUT: Token text */
  int *pnToken,                   /* OUT: Number of bytes at *paToken */
  int *piStart,                   /* OUT: Starting offset of token */
  int *piEnd,                     /* OUT: Ending offset of token */
  int *piPos                      /* OUT: Position integer of token */
){
  unicode_cursor *pCsr = (unicode_cursor *)pC;
  unicode_tokenizer *p = ((unicode_tokenizer *)pCsr->base.pTokenizer);
  int iCode = 0;
  char *zOut;
  const unsigned char *z = &pCsr->aInput[pCsr->iOff];
  const unsigned char *zStart = z;
  const unsigned char *zEnd;
  const unsigned char *zTerm = &pCsr->aInput[pCsr->nInput];

  /* Scan past any delimiter characters before the start of the next token.
  ** Return SQLITE_DONE early if this takes us all the way to the end of 
  ** the input.  */
  while( z<zTerm ){
    READ_UTF8(z, zTerm, iCode);
    if( unicodeIsAlnum(p, iCode) ) break;
    zStart = z;
  }
  if( zStart>=zTerm ) return SQLITE_DONE;

  zOut = pCsr->zToken;
  do {
    int iOut;

    /* Grow the output buffer if required. */
    if( (zOut-pCsr->zToken)>=(pCsr->nAlloc-4) ){
      char *zNew = sqlite3_realloc(pCsr->zToken, pCsr->nAlloc+64);
      if( !zNew ) return SQLITE_NOMEM;
      zOut = &zNew[zOut - pCsr->zToken];
      pCsr->zToken = zNew;
      pCsr->nAlloc += 64;
    }

    /* Write the folded case of the last character read to the output */
    zEnd = z;
    iOut = sqlite3FtsUnicodeFold(iCode, p->bRemoveDiacritic);
    if( iOut ){
      WRITE_UTF8(zOut, iOut);
    }

    /* If the cursor is not at EOF, read the next character */
    if( z>=zTerm ) break;
    READ_UTF8(z, zTerm, iCode);
  }while( unicodeIsAlnum(p, iCode) 
       || sqlite3FtsUnicodeIsdiacritic(iCode)
  );

  /* Set the output variables and return. */
  pCsr->iOff = (int)(z - pCsr->aInput);
  *paToken = pCsr->zToken;
  *pnToken = (int)(zOut - pCsr->zToken);
  *piStart = (int)(zStart - pCsr->aInput);
  *piEnd = (int)(zEnd - pCsr->aInput);
  *piPos = pCsr->iToken++;
  return SQLITE_OK;
}

/*
** Set *ppModule to a pointer to the sqlite3_tokenizer_module 
** structure for the unicode tokenizer.
*/
SQLITE_PRIVATE void sqlite3Fts3UnicodeTokenizer(sqlite3_tokenizer_module const **ppModule){
  static const sqlite3_tokenizer_module module = {
    0,
    unicodeCreate,
    unicodeDestroy,
    unicodeOpen,
    unicodeClose,
    unicodeNext,
    0,
  };
  *ppModule = &module;
}

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */
#endif /* ifndef SQLITE_DISABLE_FTS3_UNICODE */

/************** End of fts3_unicode.c ****************************************/
/************** Begin file fts3_unicode2.c ***********************************/
/*
** 2012 May 25
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
*/

/*
** DO NOT EDIT THIS MACHINE GENERATED FILE.
*/

#ifndef SQLITE_DISABLE_FTS3_UNICODE
#if defined(SQLITE_ENABLE_FTS3) || defined(SQLITE_ENABLE_FTS4)

/* #include <assert.h> */

/*
** Return true if the argument corresponds to a unicode codepoint
** classified as either a letter or a number. Otherwise false.
**
** The results are undefined if the value passed to this function
** is less than zero.
*/
SQLITE_PRIVATE int sqlite3FtsUnicodeIsalnum(int c){
  /* Each unsigned integer in the following array corresponds to a contiguous
  ** range of unicode codepoints that are not either letters or numbers (i.e.
  ** codepoints for which this function should return 0).
  **
  ** The most significant 22 bits in each 32-bit value contain the first 
  ** codepoint in the range. The least significant 10 bits are used to store
  ** the size of the range (always at least 1). In other words, the value 
  ** ((C<<22) + N) represents a range of N codepoints starting with codepoint 
  ** C. It is not possible to represent a range larger than 1023 codepoints 
  ** using this format.
  */
  static const unsigned int aEntry[] = {
    0x00000030, 0x0000E807, 0x00016C06, 0x0001EC2F, 0x0002AC07,
    0x0002D001, 0x0002D803, 0x0002EC01, 0x0002FC01, 0x00035C01,
    0x0003DC01, 0x000B0804, 0x000B480E, 0x000B9407, 0x000BB401,
    0x000BBC81, 0x000DD401, 0x000DF801, 0x000E1002, 0x000E1C01,
    0x000FD801, 0x00120808, 0x00156806, 0x00162402, 0x00163C01,
    0x00164437, 0x0017CC02, 0x00180005, 0x00181816, 0x00187802,
    0x00192C15, 0x0019A804, 0x0019C001, 0x001B5001, 0x001B580F,
    0x001B9C07, 0x001BF402, 0x001C000E, 0x001C3C01, 0x001C4401,
    0x001CC01B, 0x001E980B, 0x001FAC09, 0x001FD804, 0x00205804,
    0x00206C09, 0x00209403, 0x0020A405, 0x0020C00F, 0x00216403,
    0x00217801, 0x0023901B, 0x00240004, 0x0024E803, 0x0024F812,
    0x00254407, 0x00258804, 0x0025C001, 0x00260403, 0x0026F001,
    0x0026F807, 0x00271C02, 0x00272C03, 0x00275C01, 0x00278802,
    0x0027C802, 0x0027E802, 0x00280403, 0x0028F001, 0x0028F805,
    0x00291C02, 0x00292C03, 0x00294401, 0x0029C002, 0x0029D401,
    0x002A0403, 0x002AF001, 0x002AF808, 0x002B1C03, 0x002B2C03,
    0x002B8802, 0x002BC002, 0x002C0403, 0x002CF001, 0x002CF807,
    0x002D1C02, 0x002D2C03, 0x002D5802, 0x002D8802, 0x002DC001,
    0x002E0801, 0x002EF805, 0x002F1803, 0x002F2804, 0x002F5C01,
    0x002FCC08, 0x00300403, 0x0030F807, 0x00311803, 0x00312804,
    0x00315402, 0x00318802, 0x0031FC01, 0x00320802, 0x0032F001,
    0x0032F807, 0x00331803, 0x00332804, 0x00335402, 0x00338802,
    0x00340802, 0x0034F807, 0x00351803, 0x00352804, 0x00355C01,
    0x00358802, 0x0035E401, 0x00360802, 0x00372801, 0x00373C06,
    0x00375801, 0x00376008, 0x0037C803, 0x0038C401, 0x0038D007,
    0x0038FC01, 0x00391C09, 0x00396802, 0x003AC401, 0x003AD006,
    0x003AEC02, 0x003B2006, 0x003C041F, 0x003CD00C, 0x003DC417,
    0x003E340B, 0x003E6424, 0x003EF80F, 0x003F380D, 0x0040AC14,
    0x00412806, 0x00415804, 0x00417803, 0x00418803, 0x00419C07,
    0x0041C404, 0x0042080C, 0x00423C01, 0x00426806, 0x0043EC01,
    0x004D740C, 0x004E400A, 0x00500001, 0x0059B402, 0x005A0001,
    0x005A6C02, 0x005BAC03, 0x005C4803, 0x005CC805, 0x005D4802,
    0x005DC802, 0x005ED023, 0x005F6004, 0x005F7401, 0x0060000F,
    0x0062A401, 0x0064800C, 0x0064C00C, 0x00650001, 0x00651002,
    0x0066C011, 0x00672002, 0x00677822, 0x00685C05, 0x00687802,
    0x0069540A, 0x0069801D, 0x0069FC01, 0x006A8007, 0x006AA006,
    0x006C0005, 0x006CD011, 0x006D6823, 0x006E0003, 0x006E840D,
    0x006F980E, 0x006FF004, 0x00709014, 0x0070EC05, 0x0071F802,
    0x00730008, 0x00734019, 0x0073B401, 0x0073C803, 0x00770027,
    0x0077F004, 0x007EF401, 0x007EFC03, 0x007F3403, 0x007F7403,
    0x007FB403, 0x007FF402, 0x00800065, 0x0081A806, 0x0081E805,
    0x00822805, 0x0082801A, 0x00834021, 0x00840002, 0x00840C04,
    0x00842002, 0x00845001, 0x00845803, 0x00847806, 0x00849401,
    0x00849C01, 0x0084A401, 0x0084B801, 0x0084E802, 0x00850005,
    0x00852804, 0x00853C01, 0x00864264, 0x00900027, 0x0091000B,
    0x0092704E, 0x00940200, 0x009C0475, 0x009E53B9, 0x00AD400A,
    0x00B39406, 0x00B3BC03, 0x00B3E404, 0x00B3F802, 0x00B5C001,
    0x00B5FC01, 0x00B7804F, 0x00B8C00C, 0x00BA001A, 0x00BA6C59,
    0x00BC00D6, 0x00BFC00C, 0x00C00005, 0x00C02019, 0x00C0A807,
    0x00C0D802, 0x00C0F403, 0x00C26404, 0x00C28001, 0x00C3EC01,
    0x00C64002, 0x00C6580A, 0x00C70024, 0x00C8001F, 0x00C8A81E,
    0x00C94001, 0x00C98020, 0x00CA2827, 0x00CB003F, 0x00CC0100,
    0x01370040, 0x02924037, 0x0293F802, 0x02983403, 0x0299BC10,
    0x029A7C01, 0x029BC008, 0x029C0017, 0x029C8002, 0x029E2402,
    0x02A00801, 0x02A01801, 0x02A02C01, 0x02A08C09, 0x02A0D804,
    0x02A1D004, 0x02A20002, 0x02A2D011, 0x02A33802, 0x02A38012,
    0x02A3E003, 0x02A4980A, 0x02A51C0D, 0x02A57C01, 0x02A60004,
    0x02A6CC1B, 0x02A77802, 0x02A8A40E, 0x02A90C01, 0x02A93002,
    0x02A97004, 0x02A9DC03, 0x02A9EC01, 0x02AAC001, 0x02AAC803,
    0x02AADC02, 0x02AAF802, 0x02AB0401, 0x02AB7802, 0x02ABAC07,
    0x02ABD402, 0x02AF8C0B, 0x03600001, 0x036DFC02, 0x036FFC02,
    0x037FFC01, 0x03EC7801, 0x03ECA401, 0x03EEC810, 0x03F4F802,
    0x03F7F002, 0x03F8001A, 0x03F88007, 0x03F8C023, 0x03F95013,
    0x03F9A004, 0x03FBFC01, 0x03FC040F, 0x03FC6807, 0x03FCEC06,
    0x03FD6C0B, 0x03FF8007, 0x03FFA007, 0x03FFE405, 0x04040003,
    0x0404DC09, 0x0405E411, 0x0406400C, 0x0407402E, 0x040E7C01,
    0x040F4001, 0x04215C01, 0x04247C01, 0x0424FC01, 0x04280403,
    0x04281402, 0x04283004, 0x0428E003, 0x0428FC01, 0x04294009,
    0x0429FC01, 0x042CE407, 0x04400003, 0x0440E016, 0x04420003,
    0x0442C012, 0x04440003, 0x04449C0E, 0x04450004, 0x04460003,
    0x0446CC0E, 0x04471404, 0x045AAC0D, 0x0491C004, 0x05BD442E,
    0x05BE3C04, 0x074000F6, 0x07440027, 0x0744A4B5, 0x07480046,
    0x074C0057, 0x075B0401, 0x075B6C01, 0x075BEC01, 0x075C5401,
    0x075CD401, 0x075D3C01, 0x075DBC01, 0x075E2401, 0x075EA401,
    0x075F0C01, 0x07BBC002, 0x07C0002C, 0x07C0C064, 0x07C2800F,
    0x07C2C40E, 0x07C3040F, 0x07C3440F, 0x07C4401F, 0x07C4C03C,
    0x07C5C02B, 0x07C7981D, 0x07C8402B, 0x07C90009, 0x07C94002,
    0x07CC0021, 0x07CCC006, 0x07CCDC46, 0x07CE0014, 0x07CE8025,
    0x07CF1805, 0x07CF8011, 0x07D0003F, 0x07D10001, 0x07D108B6,
    0x07D3E404, 0x07D4003E, 0x07D50004, 0x07D54018, 0x07D7EC46,
    0x07D9140B, 0x07DA0046, 0x07DC0074, 0x38000401, 0x38008060,
    0x380400F0,
  };
  static const unsigned int aAscii[4] = {
    0xFFFFFFFF, 0xFC00FFFF, 0xF8000001, 0xF8000001,
  };

  if( c<128 ){
    return ( (aAscii[c >> 5] & (1 << (c & 0x001F)))==0 );
  }else if( c<(1<<22) ){
    unsigned int key = (((unsigned int)c)<<10) | 0x000003FF;
    int iRes = 0;
    int iHi = sizeof(aEntry)/sizeof(aEntry[0]) - 1;
    int iLo = 0;
    while( iHi>=iLo ){
      int iTest = (iHi + iLo) / 2;
      if( key >= aEntry[iTest] ){
        iRes = iTest;
        iLo = iTest+1;
      }else{
        iHi = iTest-1;
      }
    }
    assert( aEntry[0]<key );
    assert( key>=aEntry[iRes] );
    return (((unsigned int)c) >= ((aEntry[iRes]>>10) + (aEntry[iRes]&0x3FF)));
  }
  return 1;
}


/*
** If the argument is a codepoint corresponding to a lowercase letter
** in the ASCII range with a diacritic added, return the codepoint
** of the ASCII letter only. For example, if passed 235 - "LATIN
** SMALL LETTER E WITH DIAERESIS" - return 65 ("LATIN SMALL LETTER
** E"). The resuls of passing a codepoint that corresponds to an
** uppercase letter are undefined.
*/
static int remove_diacritic(int c){
  unsigned short aDia[] = {
        0,  1797,  1848,  1859,  1891,  1928,  1940,  1995, 
     2024,  2040,  2060,  2110,  2168,  2206,  2264,  2286, 
     2344,  2383,  2472,  2488,  2516,  2596,  2668,  2732, 
     2782,  2842,  2894,  2954,  2984,  3000,  3028,  3336, 
     3456,  3696,  3712,  3728,  3744,  3896,  3912,  3928, 
     3968,  4008,  4040,  4106,  4138,  4170,  4202,  4234, 
     4266,  4296,  4312,  4344,  4408,  4424,  4472,  4504, 
     6148,  6198,  6264,  6280,  6360,  6429,  6505,  6529, 
    61448, 61468, 61534, 61592, 61642, 61688, 61704, 61726, 
    61784, 61800, 61836, 61880, 61914, 61948, 61998, 62122, 
    62154, 62200, 62218, 62302, 62364, 62442, 62478, 62536, 
    62554, 62584, 62604, 62640, 62648, 62656, 62664, 62730, 
    62924, 63050, 63082, 63274, 63390, 
  };
  char aChar[] = {
    '\0', 'a',  'c',  'e',  'i',  'n',  'o',  'u',  'y',  'y',  'a',  'c',  
    'd',  'e',  'e',  'g',  'h',  'i',  'j',  'k',  'l',  'n',  'o',  'r',  
    's',  't',  'u',  'u',  'w',  'y',  'z',  'o',  'u',  'a',  'i',  'o',  
    'u',  'g',  'k',  'o',  'j',  'g',  'n',  'a',  'e',  'i',  'o',  'r',  
    'u',  's',  't',  'h',  'a',  'e',  'o',  'y',  '\0', '\0', '\0', '\0', 
    '\0', '\0', '\0', '\0', 'a',  'b',  'd',  'd',  'e',  'f',  'g',  'h',  
    'h',  'i',  'k',  'l',  'l',  'm',  'n',  'p',  'r',  'r',  's',  't',  
    'u',  'v',  'w',  'w',  'x',  'y',  'z',  'h',  't',  'w',  'y',  'a',  
    'e',  'i',  'o',  'u',  'y',  
  };

  unsigned int key = (((unsigned int)c)<<3) | 0x00000007;
  int iRes = 0;
  int iHi = sizeof(aDia)/sizeof(aDia[0]) - 1;
  int iLo = 0;
  while( iHi>=iLo ){
    int iTest = (iHi + iLo) / 2;
    if( key >= aDia[iTest] ){
      iRes = iTest;
      iLo = iTest+1;
    }else{
      iHi = iTest-1;
    }
  }
  assert( key>=aDia[iRes] );
  return ((c > (aDia[iRes]>>3) + (aDia[iRes]&0x07)) ? c : (int)aChar[iRes]);
}


/*
** Return true if the argument interpreted as a unicode codepoint
** is a diacritical modifier character.
*/
SQLITE_PRIVATE int sqlite3FtsUnicodeIsdiacritic(int c){
  unsigned int mask0 = 0x08029FDF;
  unsigned int mask1 = 0x000361F8;
  if( c<768 || c>817 ) return 0;
  return (c < 768+32) ?
      (mask0 & (1 << (c-768))) :
      (mask1 & (1 << (c-768-32)));
}


/*
** Interpret the argument as a unicode codepoint. If the codepoint
** is an upper case character that has a lower case equivalent,
** return the codepoint corresponding to the lower case version.
** Otherwise, return a copy of the argument.
**
** The results are undefined if the value passed to this function
** is less than zero.
*/
SQLITE_PRIVATE int sqlite3FtsUnicodeFold(int c, int bRemoveDiacritic){
  /* Each entry in the following array defines a rule for folding a range
  ** of codepoints to lower case. The rule applies to a range of nRange
  ** codepoints starting at codepoint iCode.
  **
  ** If the least significant bit in flags is clear, then the rule applies
  ** to all nRange codepoints (i.e. all nRange codepoints are upper case and
  ** need to be folded). Or, if it is set, then the rule only applies to
  ** every second codepoint in the range, starting with codepoint C.
  **
  ** The 7 most significant bits in flags are an index into the aiOff[]
  ** array. If a specific codepoint C does require folding, then its lower
  ** case equivalent is ((C + aiOff[flags>>1]) & 0xFFFF).
  **
  ** The contents of this array are generated by parsing the CaseFolding.txt
  ** file distributed as part of the "Unicode Character Database". See
  ** http://www.unicode.org for details.
  */
  static const struct TableEntry {
    unsigned short iCode;
    unsigned char flags;
    unsigned char nRange;
  } aEntry[] = {
    {65, 14, 26},          {181, 64, 1},          {192, 14, 23},
    {216, 14, 7},          {256, 1, 48},          {306, 1, 6},
    {313, 1, 16},          {330, 1, 46},          {376, 116, 1},
    {377, 1, 6},           {383, 104, 1},         {385, 50, 1},
    {386, 1, 4},           {390, 44, 1},          {391, 0, 1},
    {393, 42, 2},          {395, 0, 1},           {398, 32, 1},
    {399, 38, 1},          {400, 40, 1},          {401, 0, 1},
    {403, 42, 1},          {404, 46, 1},          {406, 52, 1},
    {407, 48, 1},          {408, 0, 1},           {412, 52, 1},
    {413, 54, 1},          {415, 56, 1},          {416, 1, 6},
    {422, 60, 1},          {423, 0, 1},           {425, 60, 1},
    {428, 0, 1},           {430, 60, 1},          {431, 0, 1},
    {433, 58, 2},          {435, 1, 4},           {439, 62, 1},
    {440, 0, 1},           {444, 0, 1},           {452, 2, 1},
    {453, 0, 1},           {455, 2, 1},           {456, 0, 1},
    {458, 2, 1},           {459, 1, 18},          {478, 1, 18},
    {497, 2, 1},           {498, 1, 4},           {502, 122, 1},
    {503, 134, 1},         {504, 1, 40},          {544, 110, 1},
    {546, 1, 18},          {570, 70, 1},          {571, 0, 1},
    {573, 108, 1},         {574, 68, 1},          {577, 0, 1},
    {579, 106, 1},         {580, 28, 1},          {581, 30, 1},
    {582, 1, 10},          {837, 36, 1},          {880, 1, 4},
    {886, 0, 1},           {902, 18, 1},          {904, 16, 3},
    {908, 26, 1},          {910, 24, 2},          {913, 14, 17},
    {931, 14, 9},          {962, 0, 1},           {975, 4, 1},
    {976, 140, 1},         {977, 142, 1},         {981, 146, 1},
    {982, 144, 1},         {984, 1, 24},          {1008, 136, 1},
    {1009, 138, 1},        {1012, 130, 1},        {1013, 128, 1},
    {1015, 0, 1},          {1017, 152, 1},        {1018, 0, 1},
    {1021, 110, 3},        {1024, 34, 16},        {1040, 14, 32},
    {1120, 1, 34},         {1162, 1, 54},         {1216, 6, 1},
    {1217, 1, 14},         {1232, 1, 88},         {1329, 22, 38},
    {4256, 66, 38},        {4295, 66, 1},         {4301, 66, 1},
    {7680, 1, 150},        {7835, 132, 1},        {7838, 96, 1},
    {7840, 1, 96},         {7944, 150, 8},        {7960, 150, 6},
    {7976, 150, 8},        {7992, 150, 8},        {8008, 150, 6},
    {8025, 151, 8},        {8040, 150, 8},        {8072, 150, 8},
    {8088, 150, 8},        {8104, 150, 8},        {8120, 150, 2},
    {8122, 126, 2},        {8124, 148, 1},        {8126, 100, 1},
    {8136, 124, 4},        {8140, 148, 1},        {8152, 150, 2},
    {8154, 120, 2},        {8168, 150, 2},        {8170, 118, 2},
    {8172, 152, 1},        {8184, 112, 2},        {8186, 114, 2},
    {8188, 148, 1},        {8486, 98, 1},         {8490, 92, 1},
    {8491, 94, 1},         {8498, 12, 1},         {8544, 8, 16},
    {8579, 0, 1},          {9398, 10, 26},        {11264, 22, 47},
    {11360, 0, 1},         {11362, 88, 1},        {11363, 102, 1},
    {11364, 90, 1},        {11367, 1, 6},         {11373, 84, 1},
    {11374, 86, 1},        {11375, 80, 1},        {11376, 82, 1},
    {11378, 0, 1},         {11381, 0, 1},         {11390, 78, 2},
    {11392, 1, 100},       {11499, 1, 4},         {11506, 0, 1},
    {42560, 1, 46},        {42624, 1, 24},        {42786, 1, 14},
    {42802, 1, 62},        {42873, 1, 4},         {42877, 76, 1},
    {42878, 1, 10},        {42891, 0, 1},         {42893, 74, 1},
    {42896, 1, 4},         {42912, 1, 10},        {42922, 72, 1},
    {65313, 14, 26},       
  };
  static const unsigned short aiOff[] = {
   1,     2,     8,     15,    16,    26,    28,    32,    
   37,    38,    40,    48,    63,    64,    69,    71,    
   79,    80,    116,   202,   203,   205,   206,   207,   
   209,   210,   211,   213,   214,   217,   218,   219,   
   775,   7264,  10792, 10795, 23228, 23256, 30204, 54721, 
   54753, 54754, 54756, 54787, 54793, 54809, 57153, 57274, 
   57921, 58019, 58363, 61722, 65268, 65341, 65373, 65406, 
   65408, 65410, 65415, 65424, 65436, 65439, 65450, 65462, 
   65472, 65476, 65478, 65480, 65482, 65488, 65506, 65511, 
   65514, 65521, 65527, 65528, 65529, 
  };

  int ret = c;

  assert( c>=0 );
  assert( sizeof(unsigned short)==2 && sizeof(unsigned char)==1 );

  if( c<128 ){
    if( c>='A' && c<='Z' ) ret = c + ('a' - 'A');
  }else if( c<65536 ){
    int iHi = sizeof(aEntry)/sizeof(aEntry[0]) - 1;
    int iLo = 0;
    int iRes = -1;

    while( iHi>=iLo ){
      int iTest = (iHi + iLo) / 2;
      int cmp = (c - aEntry[iTest].iCode);
      if( cmp>=0 ){
        iRes = iTest;
        iLo = iTest+1;
      }else{
        iHi = iTest-1;
      }
    }
    assert( iRes<0 || c>=aEntry[iRes].iCode );

    if( iRes>=0 ){
      const struct TableEntry *p = &aEntry[iRes];
      if( c<(p->iCode + p->nRange) && 0==(0x01 & p->flags & (p->iCode ^ c)) ){
        ret = (c + (aiOff[p->flags>>1])) & 0x0000FFFF;
        assert( ret>0 );
      }
    }

    if( bRemoveDiacritic ) ret = remove_diacritic(ret);
  }
  
  else if( c>=66560 && c<66600 ){
    ret = c + 40;
  }

  return ret;
}
#endif /* defined(SQLITE_ENABLE_FTS3) || defined(SQLITE_ENABLE_FTS4) */
#endif /* !defined(SQLITE_DISABLE_FTS3_UNICODE) */

/************** End of fts3_unicode2.c ***************************************/
/************** Begin file rtree.c *******************************************/
/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code for implementations of the r-tree and r*-tree
** algorithms packaged as an SQLite virtual table module.
*/

/*
** Database Format of R-Tree Tables
** --------------------------------
**
** The data structure for a single virtual r-tree table is stored in three 
** native SQLite tables declared as follows. In each case, the '%' character
** in the table name is replaced with the user-supplied name of the r-tree
** table.
**
**   CREATE TABLE %_node(nodeno INTEGER PRIMARY KEY, data BLOB)
**   CREATE TABLE %_parent(nodeno INTEGER PRIMARY KEY, parentnode INTEGER)
**   CREATE TABLE %_rowid(rowid INTEGER PRIMARY KEY, nodeno INTEGER)
**
** The data for each node of the r-tree structure is stored in the %_node
** table. For each node that is not the root node of the r-tree, there is
** an entry in the %_parent table associating the node with its parent.
** And for each row of data in the table, there is an entry in the %_rowid
** table that maps from the entries rowid to the id of the node that it
** is stored on.
**
** The root node of an r-tree always exists, even if the r-tree table is
** empty. The nodeno of the root node is always 1. All other nodes in the
** table must be the same size as the root node. The content of each node
** is formatted as follows:
**
**   1. If the node is the root node (node 1), then the first 2 bytes
**      of the node contain the tree depth as a big-endian integer.
**      For non-root nodes, the first 2 bytes are left unused.
**
**   2. The next 2 bytes contain the number of entries currently 
**      stored in the node.
**
**   3. The remainder of the node contains the node entries. Each entry
**      consists of a single 8-byte integer followed by an even number
**      of 4-byte coordinates. For leaf nodes the integer is the rowid
**      of a record. For internal nodes it is the node number of a
**      child page.
*/

#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_RTREE)

#ifndef SQLITE_CORE
  SQLITE_EXTENSION_INIT1
#else
#endif

/* #include <string.h> */
/* #include <assert.h> */
/* #include <stdio.h> */

#ifndef SQLITE_AMALGAMATION
#include "sqlite3rtree.h"
typedef sqlite3_int64 i64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
#endif

/*  The following macro is used to suppress compiler warnings.
*/
#ifndef UNUSED_PARAMETER
# define UNUSED_PARAMETER(x) (void)(x)
#endif

typedef struct Rtree Rtree;
typedef struct RtreeCursor RtreeCursor;
typedef struct RtreeNode RtreeNode;
typedef struct RtreeCell RtreeCell;
typedef struct RtreeConstraint RtreeConstraint;
typedef struct RtreeMatchArg RtreeMatchArg;
typedef struct RtreeGeomCallback RtreeGeomCallback;
typedef union RtreeCoord RtreeCoord;
typedef struct RtreeSearchPoint RtreeSearchPoint;

/* The rtree may have between 1 and RTREE_MAX_DIMENSIONS dimensions. */
#define RTREE_MAX_DIMENSIONS 5

/* Size of hash table Rtree.aHash. This hash table is not expected to
** ever contain very many entries, so a fixed number of buckets is 
** used.
*/
#define HASHSIZE 97

/* The xBestIndex method of this virtual table requires an estimate of
** the number of rows in the virtual table to calculate the costs of
** various strategies. If possible, this estimate is loaded from the
** sqlite_stat1 table (with RTREE_MIN_ROWEST as a hard-coded minimum).
** Otherwise, if no sqlite_stat1 entry is available, use 
** RTREE_DEFAULT_ROWEST.
*/
#define RTREE_DEFAULT_ROWEST 1048576
#define RTREE_MIN_ROWEST         100

/* 
** An rtree virtual-table object.
*/
struct Rtree {
  sqlite3_vtab base;          /* Base class.  Must be first */
  sqlite3 *db;                /* Host database connection */
  int iNodeSize;              /* Size in bytes of each node in the node table */
  u8 nDim;                    /* Number of dimensions */
  u8 eCoordType;              /* RTREE_COORD_REAL32 or RTREE_COORD_INT32 */
  u8 nBytesPerCell;           /* Bytes consumed per cell */
  int iDepth;                 /* Current depth of the r-tree structure */
  char *zDb;                  /* Name of database containing r-tree table */
  char *zName;                /* Name of r-tree table */ 
  int nBusy;                  /* Current number of users of this structure */
  i64 nRowEst;                /* Estimated number of rows in this table */

  /* List of nodes removed during a CondenseTree operation. List is
  ** linked together via the pointer normally used for hash chains -
  ** RtreeNode.pNext. RtreeNode.iNode stores the depth of the sub-tree 
  ** headed by the node (leaf nodes have RtreeNode.iNode==0).
  */
  RtreeNode *pDeleted;
  int iReinsertHeight;        /* Height of sub-trees Reinsert() has run on */

  /* Statements to read/write/delete a record from xxx_node */
  sqlite3_stmt *pReadNode;
  sqlite3_stmt *pWriteNode;
  sqlite3_stmt *pDeleteNode;

  /* Statements to read/write/delete a record from xxx_rowid */
  sqlite3_stmt *pReadRowid;
  sqlite3_stmt *pWriteRowid;
  sqlite3_stmt *pDeleteRowid;

  /* Statements to read/write/delete a record from xxx_parent */
  sqlite3_stmt *pReadParent;
  sqlite3_stmt *pWriteParent;
  sqlite3_stmt *pDeleteParent;

  RtreeNode *aHash[HASHSIZE]; /* Hash table of in-memory nodes. */ 
};

/* Possible values for Rtree.eCoordType: */
#define RTREE_COORD_REAL32 0
#define RTREE_COORD_INT32  1

/*
** If SQLITE_RTREE_INT_ONLY is defined, then this virtual table will
** only deal with integer coordinates.  No floating point operations
** will be done.
*/
#ifdef SQLITE_RTREE_INT_ONLY
  typedef sqlite3_int64 RtreeDValue;       /* High accuracy coordinate */
  typedef int RtreeValue;                  /* Low accuracy coordinate */
# define RTREE_ZERO 0
#else
  typedef double RtreeDValue;              /* High accuracy coordinate */
  typedef float RtreeValue;                /* Low accuracy coordinate */
# define RTREE_ZERO 0.0
#endif

/*
** When doing a search of an r-tree, instances of the following structure
** record intermediate results from the tree walk.
**
** The id is always a node-id.  For iLevel>=1 the id is the node-id of
** the node that the RtreeSearchPoint represents.  When iLevel==0, however,
** the id is of the parent node and the cell that RtreeSearchPoint
** represents is the iCell-th entry in the parent node.
*/
struct RtreeSearchPoint {
  RtreeDValue rScore;    /* The score for this node.  Smallest goes first. */
  sqlite3_int64 id;      /* Node ID */
  u8 iLevel;             /* 0=entries.  1=leaf node.  2+ for higher */
  u8 eWithin;            /* PARTLY_WITHIN or FULLY_WITHIN */
  u8 iCell;              /* Cell index within the node */
};

/*
** The minimum number of cells allowed for a node is a third of the 
** maximum. In Gutman's notation:
**
**     m = M/3
**
** If an R*-tree "Reinsert" operation is required, the same number of
** cells are removed from the overfull node and reinserted into the tree.
*/
#define RTREE_MINCELLS(p) ((((p)->iNodeSize-4)/(p)->nBytesPerCell)/3)
#define RTREE_REINSERT(p) RTREE_MINCELLS(p)
#define RTREE_MAXCELLS 51

/*
** The smallest possible node-size is (512-64)==448 bytes. And the largest
** supported cell size is 48 bytes (8 byte rowid + ten 4 byte coordinates).
** Therefore all non-root nodes must contain at least 3 entries. Since 
** 2^40 is greater than 2^64, an r-tree structure always has a depth of
** 40 or less.
*/
#define RTREE_MAX_DEPTH 40


/*
** Number of entries in the cursor RtreeNode cache.  The first entry is
** used to cache the RtreeNode for RtreeCursor.sPoint.  The remaining
** entries cache the RtreeNode for the first elements of the priority queue.
*/
#define RTREE_CACHE_SZ  5

/* 
** An rtree cursor object.
*/
struct RtreeCursor {
  sqlite3_vtab_cursor base;         /* Base class.  Must be first */
  u8 atEOF;                         /* True if at end of search */
  u8 bPoint;                        /* True if sPoint is valid */
  int iStrategy;                    /* Copy of idxNum search parameter */
  int nConstraint;                  /* Number of entries in aConstraint */
  RtreeConstraint *aConstraint;     /* Search constraints. */
  int nPointAlloc;                  /* Number of slots allocated for aPoint[] */
  int nPoint;                       /* Number of slots used in aPoint[] */
  int mxLevel;                      /* iLevel value for root of the tree */
  RtreeSearchPoint *aPoint;         /* Priority queue for search points */
  RtreeSearchPoint sPoint;          /* Cached next search point */
  RtreeNode *aNode[RTREE_CACHE_SZ]; /* Rtree node cache */
  u32 anQueue[RTREE_MAX_DEPTH+1];   /* Number of queued entries by iLevel */
};

/* Return the Rtree of a RtreeCursor */
#define RTREE_OF_CURSOR(X)   ((Rtree*)((X)->base.pVtab))

/*
** A coordinate can be either a floating point number or a integer.  All
** coordinates within a single R-Tree are always of the same time.
*/
union RtreeCoord {
  RtreeValue f;      /* Floating point value */
  int i;             /* Integer value */
  u32 u;             /* Unsigned for byte-order conversions */
};

/*
** The argument is an RtreeCoord. Return the value stored within the RtreeCoord
** formatted as a RtreeDValue (double or int64). This macro assumes that local
** variable pRtree points to the Rtree structure associated with the
** RtreeCoord.
*/
#ifdef SQLITE_RTREE_INT_ONLY
# define DCOORD(coord) ((RtreeDValue)coord.i)
#else
# define DCOORD(coord) (                           \
    (pRtree->eCoordType==RTREE_COORD_REAL32) ?      \
      ((double)coord.f) :                           \
      ((double)coord.i)                             \
  )
#endif

/*
** A search constraint.
*/
struct RtreeConstraint {
  int iCoord;                     /* Index of constrained coordinate */
  int op;                         /* Constraining operation */
  union {
    RtreeDValue rValue;             /* Constraint value. */
    int (*xGeom)(sqlite3_rtree_geometry*,int,RtreeDValue*,int*);
    int (*xQueryFunc)(sqlite3_rtree_query_info*);
  } u;
  sqlite3_rtree_query_info *pInfo;  /* xGeom and xQueryFunc argument */
};

/* Possible values for RtreeConstraint.op */
#define RTREE_EQ    0x41  /* A */
#define RTREE_LE    0x42  /* B */
#define RTREE_LT    0x43  /* C */
#define RTREE_GE    0x44  /* D */
#define RTREE_GT    0x45  /* E */
#define RTREE_MATCH 0x46  /* F: Old-style sqlite3_rtree_geometry_callback() */
#define RTREE_QUERY 0x47  /* G: New-style sqlite3_rtree_query_callback() */


/* 
** An rtree structure node.
*/
struct RtreeNode {
  RtreeNode *pParent;         /* Parent node */
  i64 iNode;                  /* The node number */
  int nRef;                   /* Number of references to this node */
  int isDirty;                /* True if the node needs to be written to disk */
  u8 *zData;                  /* Content of the node, as should be on disk */
  RtreeNode *pNext;           /* Next node in this hash collision chain */
};

/* Return the number of cells in a node  */
#define NCELL(pNode) readInt16(&(pNode)->zData[2])

/* 
** A single cell from a node, deserialized
*/
struct RtreeCell {
  i64 iRowid;                                 /* Node or entry ID */
  RtreeCoord aCoord[RTREE_MAX_DIMENSIONS*2];  /* Bounding box coordinates */
};


/*
** This object becomes the sqlite3_user_data() for the SQL functions
** that are created by sqlite3_rtree_geometry_callback() and
** sqlite3_rtree_query_callback() and which appear on the right of MATCH
** operators in order to constrain a search.
**
** xGeom and xQueryFunc are the callback functions.  Exactly one of 
** xGeom and xQueryFunc fields is non-NULL, depending on whether the
** SQL function was created using sqlite3_rtree_geometry_callback() or
** sqlite3_rtree_query_callback().
** 
** This object is deleted automatically by the destructor mechanism in
** sqlite3_create_function_v2().
*/
struct RtreeGeomCallback {
  int (*xGeom)(sqlite3_rtree_geometry*, int, RtreeDValue*, int*);
  int (*xQueryFunc)(sqlite3_rtree_query_info*);
  void (*xDestructor)(void*);
  void *pContext;
};


/*
** Value for the first field of every RtreeMatchArg object. The MATCH
** operator tests that the first field of a blob operand matches this
** value to avoid operating on invalid blobs (which could cause a segfault).
*/
#define RTREE_GEOMETRY_MAGIC 0x891245AB

/*
** An instance of this structure (in the form of a BLOB) is returned by
** the SQL functions that sqlite3_rtree_geometry_callback() and
** sqlite3_rtree_query_callback() create, and is read as the right-hand
** operand to the MATCH operator of an R-Tree.
*/
struct RtreeMatchArg {
  u32 magic;                  /* Always RTREE_GEOMETRY_MAGIC */
  RtreeGeomCallback cb;       /* Info about the callback functions */
  int nParam;                 /* Number of parameters to the SQL function */
  RtreeDValue aParam[1];      /* Values for parameters to the SQL function */
};

#ifndef MAX
# define MAX(x,y) ((x) < (y) ? (y) : (x))
#endif
#ifndef MIN
# define MIN(x,y) ((x) > (y) ? (y) : (x))
#endif

/*
** Functions to deserialize a 16 bit integer, 32 bit real number and
** 64 bit integer. The deserialized value is returned.
*/
static int readInt16(u8 *p){
  return (p[0]<<8) + p[1];
}
static void readCoord(u8 *p, RtreeCoord *pCoord){
  u32 i = (
    (((u32)p[0]) << 24) + 
    (((u32)p[1]) << 16) + 
    (((u32)p[2]) <<  8) + 
    (((u32)p[3]) <<  0)
  );
  *(u32 *)pCoord = i;
}
static i64 readInt64(u8 *p){
  return (
    (((i64)p[0]) << 56) + 
    (((i64)p[1]) << 48) + 
    (((i64)p[2]) << 40) + 
    (((i64)p[3]) << 32) + 
    (((i64)p[4]) << 24) + 
    (((i64)p[5]) << 16) + 
    (((i64)p[6]) <<  8) + 
    (((i64)p[7]) <<  0)
  );
}

/*
** Functions to serialize a 16 bit integer, 32 bit real number and
** 64 bit integer. The value returned is the number of bytes written
** to the argument buffer (always 2, 4 and 8 respectively).
*/
static int writeInt16(u8 *p, int i){
  p[0] = (i>> 8)&0xFF;
  p[1] = (i>> 0)&0xFF;
  return 2;
}
static int writeCoord(u8 *p, RtreeCoord *pCoord){
  u32 i;
  assert( sizeof(RtreeCoord)==4 );
  assert( sizeof(u32)==4 );
  i = *(u32 *)pCoord;
  p[0] = (i>>24)&0xFF;
  p[1] = (i>>16)&0xFF;
  p[2] = (i>> 8)&0xFF;
  p[3] = (i>> 0)&0xFF;
  return 4;
}
static int writeInt64(u8 *p, i64 i){
  p[0] = (i>>56)&0xFF;
  p[1] = (i>>48)&0xFF;
  p[2] = (i>>40)&0xFF;
  p[3] = (i>>32)&0xFF;
  p[4] = (i>>24)&0xFF;
  p[5] = (i>>16)&0xFF;
  p[6] = (i>> 8)&0xFF;
  p[7] = (i>> 0)&0xFF;
  return 8;
}

/*
** Increment the reference count of node p.
*/
static void nodeReference(RtreeNode *p){
  if( p ){
    p->nRef++;
  }
}

/*
** Clear the content of node p (set all bytes to 0x00).
*/
static void nodeZero(Rtree *pRtree, RtreeNode *p){
  memset(&p->zData[2], 0, pRtree->iNodeSize-2);
  p->isDirty = 1;
}

/*
** Given a node number iNode, return the corresponding key to use
** in the Rtree.aHash table.
*/
static int nodeHash(i64 iNode){
  return iNode % HASHSIZE;
}

/*
** Search the node hash table for node iNode. If found, return a pointer
** to it. Otherwise, return 0.
*/
static RtreeNode *nodeHashLookup(Rtree *pRtree, i64 iNode){
  RtreeNode *p;
  for(p=pRtree->aHash[nodeHash(iNode)]; p && p->iNode!=iNode; p=p->pNext);
  return p;
}

/*
** Add node pNode to the node hash table.
*/
static void nodeHashInsert(Rtree *pRtree, RtreeNode *pNode){
  int iHash;
  assert( pNode->pNext==0 );
  iHash = nodeHash(pNode->iNode);
  pNode->pNext = pRtree->aHash[iHash];
  pRtree->aHash[iHash] = pNode;
}

/*
** Remove node pNode from the node hash table.
*/
static void nodeHashDelete(Rtree *pRtree, RtreeNode *pNode){
  RtreeNode **pp;
  if( pNode->iNode!=0 ){
    pp = &pRtree->aHash[nodeHash(pNode->iNode)];
    for( ; (*pp)!=pNode; pp = &(*pp)->pNext){ assert(*pp); }
    *pp = pNode->pNext;
    pNode->pNext = 0;
  }
}

/*
** Allocate and return new r-tree node. Initially, (RtreeNode.iNode==0),
** indicating that node has not yet been assigned a node number. It is
** assigned a node number when nodeWrite() is called to write the
** node contents out to the database.
*/
static RtreeNode *nodeNew(Rtree *pRtree, RtreeNode *pParent){
  RtreeNode *pNode;
  pNode = (RtreeNode *)sqlite3_malloc(sizeof(RtreeNode) + pRtree->iNodeSize);
  if( pNode ){
    memset(pNode, 0, sizeof(RtreeNode) + pRtree->iNodeSize);
    pNode->zData = (u8 *)&pNode[1];
    pNode->nRef = 1;
    pNode->pParent = pParent;
    pNode->isDirty = 1;
    nodeReference(pParent);
  }
  return pNode;
}

/*
** Obtain a reference to an r-tree node.
*/
static int nodeAcquire(
  Rtree *pRtree,             /* R-tree structure */
  i64 iNode,                 /* Node number to load */
  RtreeNode *pParent,        /* Either the parent node or NULL */
  RtreeNode **ppNode         /* OUT: Acquired node */
){
  int rc;
  int rc2 = SQLITE_OK;
  RtreeNode *pNode;

  /* Check if the requested node is already in the hash table. If so,
  ** increase its reference count and return it.
  */
  if( (pNode = nodeHashLookup(pRtree, iNode)) ){
    assert( !pParent || !pNode->pParent || pNode->pParent==pParent );
    if( pParent && !pNode->pParent ){
      nodeReference(pParent);
      pNode->pParent = pParent;
    }
    pNode->nRef++;
    *ppNode = pNode;
    return SQLITE_OK;
  }

  sqlite3_bind_int64(pRtree->pReadNode, 1, iNode);
  rc = sqlite3_step(pRtree->pReadNode);
  if( rc==SQLITE_ROW ){
    const u8 *zBlob = sqlite3_column_blob(pRtree->pReadNode, 0);
    if( pRtree->iNodeSize==sqlite3_column_bytes(pRtree->pReadNode, 0) ){
      pNode = (RtreeNode *)sqlite3_malloc(sizeof(RtreeNode)+pRtree->iNodeSize);
      if( !pNode ){
        rc2 = SQLITE_NOMEM;
      }else{
        pNode->pParent = pParent;
        pNode->zData = (u8 *)&pNode[1];
        pNode->nRef = 1;
        pNode->iNode = iNode;
        pNode->isDirty = 0;
        pNode->pNext = 0;
        memcpy(pNode->zData, zBlob, pRtree->iNodeSize);
        nodeReference(pParent);
      }
    }
  }
  rc = sqlite3_reset(pRtree->pReadNode);
  if( rc==SQLITE_OK ) rc = rc2;

  /* If the root node was just loaded, set pRtree->iDepth to the height
  ** of the r-tree structure. A height of zero means all data is stored on
  ** the root node. A height of one means the children of the root node
  ** are the leaves, and so on. If the depth as specified on the root node
  ** is greater than RTREE_MAX_DEPTH, the r-tree structure must be corrupt.
  */
  if( pNode && iNode==1 ){
    pRtree->iDepth = readInt16(pNode->zData);
    if( pRtree->iDepth>RTREE_MAX_DEPTH ){
      rc = SQLITE_CORRUPT_VTAB;
    }
  }

  /* If no error has occurred so far, check if the "number of entries"
  ** field on the node is too large. If so, set the return code to 
  ** SQLITE_CORRUPT_VTAB.
  */
  if( pNode && rc==SQLITE_OK ){
    if( NCELL(pNode)>((pRtree->iNodeSize-4)/pRtree->nBytesPerCell) ){
      rc = SQLITE_CORRUPT_VTAB;
    }
  }

  if( rc==SQLITE_OK ){
    if( pNode!=0 ){
      nodeHashInsert(pRtree, pNode);
    }else{
      rc = SQLITE_CORRUPT_VTAB;
    }
    *ppNode = pNode;
  }else{
    sqlite3_free(pNode);
    *ppNode = 0;
  }

  return rc;
}

/*
** Overwrite cell iCell of node pNode with the contents of pCell.
*/
static void nodeOverwriteCell(
  Rtree *pRtree,             /* The overall R-Tree */
  RtreeNode *pNode,          /* The node into which the cell is to be written */
  RtreeCell *pCell,          /* The cell to write */
  int iCell                  /* Index into pNode into which pCell is written */
){
  int ii;
  u8 *p = &pNode->zData[4 + pRtree->nBytesPerCell*iCell];
  p += writeInt64(p, pCell->iRowid);
  for(ii=0; ii<(pRtree->nDim*2); ii++){
    p += writeCoord(p, &pCell->aCoord[ii]);
  }
  pNode->isDirty = 1;
}

/*
** Remove the cell with index iCell from node pNode.
*/
static void nodeDeleteCell(Rtree *pRtree, RtreeNode *pNode, int iCell){
  u8 *pDst = &pNode->zData[4 + pRtree->nBytesPerCell*iCell];
  u8 *pSrc = &pDst[pRtree->nBytesPerCell];
  int nByte = (NCELL(pNode) - iCell - 1) * pRtree->nBytesPerCell;
  memmove(pDst, pSrc, nByte);
  writeInt16(&pNode->zData[2], NCELL(pNode)-1);
  pNode->isDirty = 1;
}

/*
** Insert the contents of cell pCell into node pNode. If the insert
** is successful, return SQLITE_OK.
**
** If there is not enough free space in pNode, return SQLITE_FULL.
*/
static int nodeInsertCell(
  Rtree *pRtree,                /* The overall R-Tree */
  RtreeNode *pNode,             /* Write new cell into this node */
  RtreeCell *pCell              /* The cell to be inserted */
){
  int nCell;                    /* Current number of cells in pNode */
  int nMaxCell;                 /* Maximum number of cells for pNode */

  nMaxCell = (pRtree->iNodeSize-4)/pRtree->nBytesPerCell;
  nCell = NCELL(pNode);

  assert( nCell<=nMaxCell );
  if( nCell<nMaxCell ){
    nodeOverwriteCell(pRtree, pNode, pCell, nCell);
    writeInt16(&pNode->zData[2], nCell+1);
    pNode->isDirty = 1;
  }

  return (nCell==nMaxCell);
}

/*
** If the node is dirty, write it out to the database.
*/
static int nodeWrite(Rtree *pRtree, RtreeNode *pNode){
  int rc = SQLITE_OK;
  if( pNode->isDirty ){
    sqlite3_stmt *p = pRtree->pWriteNode;
    if( pNode->iNode ){
      sqlite3_bind_int64(p, 1, pNode->iNode);
    }else{
      sqlite3_bind_null(p, 1);
    }
    sqlite3_bind_blob(p, 2, pNode->zData, pRtree->iNodeSize, SQLITE_STATIC);
    sqlite3_step(p);
    pNode->isDirty = 0;
    rc = sqlite3_reset(p);
    if( pNode->iNode==0 && rc==SQLITE_OK ){
      pNode->iNode = sqlite3_last_insert_rowid(pRtree->db);
      nodeHashInsert(pRtree, pNode);
    }
  }
  return rc;
}

/*
** Release a reference to a node. If the node is dirty and the reference
** count drops to zero, the node data is written to the database.
*/
static int nodeRelease(Rtree *pRtree, RtreeNode *pNode){
  int rc = SQLITE_OK;
  if( pNode ){
    assert( pNode->nRef>0 );
    pNode->nRef--;
    if( pNode->nRef==0 ){
      if( pNode->iNode==1 ){
        pRtree->iDepth = -1;
      }
      if( pNode->pParent ){
        rc = nodeRelease(pRtree, pNode->pParent);
      }
      if( rc==SQLITE_OK ){
        rc = nodeWrite(pRtree, pNode);
      }
      nodeHashDelete(pRtree, pNode);
      sqlite3_free(pNode);
    }
  }
  return rc;
}

/*
** Return the 64-bit integer value associated with cell iCell of
** node pNode. If pNode is a leaf node, this is a rowid. If it is
** an internal node, then the 64-bit integer is a child page number.
*/
static i64 nodeGetRowid(
  Rtree *pRtree,       /* The overall R-Tree */
  RtreeNode *pNode,    /* The node from which to extract the ID */
  int iCell            /* The cell index from which to extract the ID */
){
  assert( iCell<NCELL(pNode) );
  return readInt64(&pNode->zData[4 + pRtree->nBytesPerCell*iCell]);
}

/*
** Return coordinate iCoord from cell iCell in node pNode.
*/
static void nodeGetCoord(
  Rtree *pRtree,               /* The overall R-Tree */
  RtreeNode *pNode,            /* The node from which to extract a coordinate */
  int iCell,                   /* The index of the cell within the node */
  int iCoord,                  /* Which coordinate to extract */
  RtreeCoord *pCoord           /* OUT: Space to write result to */
){
  readCoord(&pNode->zData[12 + pRtree->nBytesPerCell*iCell + 4*iCoord], pCoord);
}

/*
** Deserialize cell iCell of node pNode. Populate the structure pointed
** to by pCell with the results.
*/
static void nodeGetCell(
  Rtree *pRtree,               /* The overall R-Tree */
  RtreeNode *pNode,            /* The node containing the cell to be read */
  int iCell,                   /* Index of the cell within the node */
  RtreeCell *pCell             /* OUT: Write the cell contents here */
){
  u8 *pData;
  u8 *pEnd;
  RtreeCoord *pCoord;
  pCell->iRowid = nodeGetRowid(pRtree, pNode, iCell);
  pData = pNode->zData + (12 + pRtree->nBytesPerCell*iCell);
  pEnd = pData + pRtree->nDim*8;
  pCoord = pCell->aCoord;
  for(; pData<pEnd; pData+=4, pCoord++){
    readCoord(pData, pCoord);
  }
}


/* Forward declaration for the function that does the work of
** the virtual table module xCreate() and xConnect() methods.
*/
static int rtreeInit(
  sqlite3 *, void *, int, const char *const*, sqlite3_vtab **, char **, int
);

/* 
** Rtree virtual table module xCreate method.
*/
static int rtreeCreate(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  return rtreeInit(db, pAux, argc, argv, ppVtab, pzErr, 1);
}

/* 
** Rtree virtual table module xConnect method.
*/
static int rtreeConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  return rtreeInit(db, pAux, argc, argv, ppVtab, pzErr, 0);
}

/*
** Increment the r-tree reference count.
*/
static void rtreeReference(Rtree *pRtree){
  pRtree->nBusy++;
}

/*
** Decrement the r-tree reference count. When the reference count reaches
** zero the structure is deleted.
*/
static void rtreeRelease(Rtree *pRtree){
  pRtree->nBusy--;
  if( pRtree->nBusy==0 ){
    sqlite3_finalize(pRtree->pReadNode);
    sqlite3_finalize(pRtree->pWriteNode);
    sqlite3_finalize(pRtree->pDeleteNode);
    sqlite3_finalize(pRtree->pReadRowid);
    sqlite3_finalize(pRtree->pWriteRowid);
    sqlite3_finalize(pRtree->pDeleteRowid);
    sqlite3_finalize(pRtree->pReadParent);
    sqlite3_finalize(pRtree->pWriteParent);
    sqlite3_finalize(pRtree->pDeleteParent);
    sqlite3_free(pRtree);
  }
}

/* 
** Rtree virtual table module xDisconnect method.
*/
static int rtreeDisconnect(sqlite3_vtab *pVtab){
  rtreeRelease((Rtree *)pVtab);
  return SQLITE_OK;
}

/* 
** Rtree virtual table module xDestroy method.
*/
static int rtreeDestroy(sqlite3_vtab *pVtab){
  Rtree *pRtree = (Rtree *)pVtab;
  int rc;
  char *zCreate = sqlite3_mprintf(
    "DROP TABLE '%q'.'%q_node';"
    "DROP TABLE '%q'.'%q_rowid';"
    "DROP TABLE '%q'.'%q_parent';",
    pRtree->zDb, pRtree->zName, 
    pRtree->zDb, pRtree->zName,
    pRtree->zDb, pRtree->zName
  );
  if( !zCreate ){
    rc = SQLITE_NOMEM;
  }else{
    rc = sqlite3_exec(pRtree->db, zCreate, 0, 0, 0);
    sqlite3_free(zCreate);
  }
  if( rc==SQLITE_OK ){
    rtreeRelease(pRtree);
  }

  return rc;
}

/* 
** Rtree virtual table module xOpen method.
*/
static int rtreeOpen(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCursor){
  int rc = SQLITE_NOMEM;
  RtreeCursor *pCsr;

  pCsr = (RtreeCursor *)sqlite3_malloc(sizeof(RtreeCursor));
  if( pCsr ){
    memset(pCsr, 0, sizeof(RtreeCursor));
    pCsr->base.pVtab = pVTab;
    rc = SQLITE_OK;
  }
  *ppCursor = (sqlite3_vtab_cursor *)pCsr;

  return rc;
}


/*
** Free the RtreeCursor.aConstraint[] array and its contents.
*/
static void freeCursorConstraints(RtreeCursor *pCsr){
  if( pCsr->aConstraint ){
    int i;                        /* Used to iterate through constraint array */
    for(i=0; i<pCsr->nConstraint; i++){
      sqlite3_rtree_query_info *pInfo = pCsr->aConstraint[i].pInfo;
      if( pInfo ){
        if( pInfo->xDelUser ) pInfo->xDelUser(pInfo->pUser);
        sqlite3_free(pInfo);
      }
    }
    sqlite3_free(pCsr->aConstraint);
    pCsr->aConstraint = 0;
  }
}

/* 
** Rtree virtual table module xClose method.
*/
static int rtreeClose(sqlite3_vtab_cursor *cur){
  Rtree *pRtree = (Rtree *)(cur->pVtab);
  int ii;
  RtreeCursor *pCsr = (RtreeCursor *)cur;
  freeCursorConstraints(pCsr);
  sqlite3_free(pCsr->aPoint);
  for(ii=0; ii<RTREE_CACHE_SZ; ii++) nodeRelease(pRtree, pCsr->aNode[ii]);
  sqlite3_free(pCsr);
  return SQLITE_OK;
}

/*
** Rtree virtual table module xEof method.
**
** Return non-zero if the cursor does not currently point to a valid 
** record (i.e if the scan has finished), or zero otherwise.
*/
static int rtreeEof(sqlite3_vtab_cursor *cur){
  RtreeCursor *pCsr = (RtreeCursor *)cur;
  return pCsr->atEOF;
}

/*
** Convert raw bits from the on-disk RTree record into a coordinate value.
** The on-disk format is big-endian and needs to be converted for little-
** endian platforms.  The on-disk record stores integer coordinates if
** eInt is true and it stores 32-bit floating point records if eInt is
** false.  a[] is the four bytes of the on-disk record to be decoded.
** Store the results in "r".
**
** There are three versions of this macro, one each for little-endian and
** big-endian processors and a third generic implementation.  The endian-
** specific implementations are much faster and are preferred if the
** processor endianness is known at compile-time.  The SQLITE_BYTEORDER
** macro is part of sqliteInt.h and hence the endian-specific
** implementation will only be used if this module is compiled as part
** of the amalgamation.
*/
#if defined(SQLITE_BYTEORDER) && SQLITE_BYTEORDER==1234
#define RTREE_DECODE_COORD(eInt, a, r) {                        \
    RtreeCoord c;    /* Coordinate decoded */                   \
    memcpy(&c.u,a,4);                                           \
    c.u = ((c.u>>24)&0xff)|((c.u>>8)&0xff00)|                   \
          ((c.u&0xff)<<24)|((c.u&0xff00)<<8);                   \
    r = eInt ? (sqlite3_rtree_dbl)c.i : (sqlite3_rtree_dbl)c.f; \
}
#elif defined(SQLITE_BYTEORDER) && SQLITE_BYTEORDER==4321
#define RTREE_DECODE_COORD(eInt, a, r) {                        \
    RtreeCoord c;    /* Coordinate decoded */                   \
    memcpy(&c.u,a,4);                                           \
    r = eInt ? (sqlite3_rtree_dbl)c.i : (sqlite3_rtree_dbl)c.f; \
}
#else
#define RTREE_DECODE_COORD(eInt, a, r) {                        \
    RtreeCoord c;    /* Coordinate decoded */                   \
    c.u = ((u32)a[0]<<24) + ((u32)a[1]<<16)                     \
           +((u32)a[2]<<8) + a[3];                              \
    r = eInt ? (sqlite3_rtree_dbl)c.i : (sqlite3_rtree_dbl)c.f; \
}
#endif

/*
** Check the RTree node or entry given by pCellData and p against the MATCH
** constraint pConstraint.  
*/
static int rtreeCallbackConstraint(
  RtreeConstraint *pConstraint,  /* The constraint to test */
  int eInt,                      /* True if RTree holding integer coordinates */
  u8 *pCellData,                 /* Raw cell content */
  RtreeSearchPoint *pSearch,     /* Container of this cell */
  sqlite3_rtree_dbl *prScore,    /* OUT: score for the cell */
  int *peWithin                  /* OUT: visibility of the cell */
){
  int i;                                                /* Loop counter */
  sqlite3_rtree_query_info *pInfo = pConstraint->pInfo; /* Callback info */
  int nCoord = pInfo->nCoord;                           /* No. of coordinates */
  int rc;                                             /* Callback return code */
  sqlite3_rtree_dbl aCoord[RTREE_MAX_DIMENSIONS*2];   /* Decoded coordinates */

  assert( pConstraint->op==RTREE_MATCH || pConstraint->op==RTREE_QUERY );
  assert( nCoord==2 || nCoord==4 || nCoord==6 || nCoord==8 || nCoord==10 );

  if( pConstraint->op==RTREE_QUERY && pSearch->iLevel==1 ){
    pInfo->iRowid = readInt64(pCellData);
  }
  pCellData += 8;
  for(i=0; i<nCoord; i++, pCellData += 4){
    RTREE_DECODE_COORD(eInt, pCellData, aCoord[i]);
  }
  if( pConstraint->op==RTREE_MATCH ){
    rc = pConstraint->u.xGeom((sqlite3_rtree_geometry*)pInfo,
                              nCoord, aCoord, &i);
    if( i==0 ) *peWithin = NOT_WITHIN;
    *prScore = RTREE_ZERO;
  }else{
    pInfo->aCoord = aCoord;
    pInfo->iLevel = pSearch->iLevel - 1;
    pInfo->rScore = pInfo->rParentScore = pSearch->rScore;
    pInfo->eWithin = pInfo->eParentWithin = pSearch->eWithin;
    rc = pConstraint->u.xQueryFunc(pInfo);
    if( pInfo->eWithin<*peWithin ) *peWithin = pInfo->eWithin;
    if( pInfo->rScore<*prScore || *prScore<RTREE_ZERO ){
      *prScore = pInfo->rScore;
    }
  }
  return rc;
}

/* 
** Check the internal RTree node given by pCellData against constraint p.
** If this constraint cannot be satisfied by any child within the node,
** set *peWithin to NOT_WITHIN.
*/
static void rtreeNonleafConstraint(
  RtreeConstraint *p,        /* The constraint to test */
  int eInt,                  /* True if RTree holds integer coordinates */
  u8 *pCellData,             /* Raw cell content as appears on disk */
  int *peWithin              /* Adjust downward, as appropriate */
){
  sqlite3_rtree_dbl val;     /* Coordinate value convert to a double */

  /* p->iCoord might point to either a lower or upper bound coordinate
  ** in a coordinate pair.  But make pCellData point to the lower bound.
  */
  pCellData += 8 + 4*(p->iCoord&0xfe);

  assert(p->op==RTREE_LE || p->op==RTREE_LT || p->op==RTREE_GE 
      || p->op==RTREE_GT || p->op==RTREE_EQ );
  switch( p->op ){
    case RTREE_LE:
    case RTREE_LT:
    case RTREE_EQ:
      RTREE_DECODE_COORD(eInt, pCellData, val);
      /* val now holds the lower bound of the coordinate pair */
      if( p->u.rValue>=val ) return;
      if( p->op!=RTREE_EQ ) break;  /* RTREE_LE and RTREE_LT end here */
      /* Fall through for the RTREE_EQ case */

    default: /* RTREE_GT or RTREE_GE,  or fallthrough of RTREE_EQ */
      pCellData += 4;
      RTREE_DECODE_COORD(eInt, pCellData, val);
      /* val now holds the upper bound of the coordinate pair */
      if( p->u.rValue<=val ) return;
  }
  *peWithin = NOT_WITHIN;
}

/*
** Check the leaf RTree cell given by pCellData against constraint p.
** If this constraint is not satisfied, set *peWithin to NOT_WITHIN.
** If the constraint is satisfied, leave *peWithin unchanged.
**
** The constraint is of the form:  xN op $val
**
** The op is given by p->op.  The xN is p->iCoord-th coordinate in
** pCellData.  $val is given by p->u.rValue.
*/
static void rtreeLeafConstraint(
  RtreeConstraint *p,        /* The constraint to test */
  int eInt,                  /* True if RTree holds integer coordinates */
  u8 *pCellData,             /* Raw cell content as appears on disk */
  int *peWithin              /* Adjust downward, as appropriate */
){
  RtreeDValue xN;      /* Coordinate value converted to a double */

  assert(p->op==RTREE_LE || p->op==RTREE_LT || p->op==RTREE_GE 
      || p->op==RTREE_GT || p->op==RTREE_EQ );
  pCellData += 8 + p->iCoord*4;
  RTREE_DECODE_COORD(eInt, pCellData, xN);
  switch( p->op ){
    case RTREE_LE: if( xN <= p->u.rValue ) return;  break;
    case RTREE_LT: if( xN <  p->u.rValue ) return;  break;
    case RTREE_GE: if( xN >= p->u.rValue ) return;  break;
    case RTREE_GT: if( xN >  p->u.rValue ) return;  break;
    default:       if( xN == p->u.rValue ) return;  break;
  }
  *peWithin = NOT_WITHIN;
}

/*
** One of the cells in node pNode is guaranteed to have a 64-bit 
** integer value equal to iRowid. Return the index of this cell.
*/
static int nodeRowidIndex(
  Rtree *pRtree, 
  RtreeNode *pNode, 
  i64 iRowid,
  int *piIndex
){
  int ii;
  int nCell = NCELL(pNode);
  assert( nCell<200 );
  for(ii=0; ii<nCell; ii++){
    if( nodeGetRowid(pRtree, pNode, ii)==iRowid ){
      *piIndex = ii;
      return SQLITE_OK;
    }
  }
  return SQLITE_CORRUPT_VTAB;
}

/*
** Return the index of the cell containing a pointer to node pNode
** in its parent. If pNode is the root node, return -1.
*/
static int nodeParentIndex(Rtree *pRtree, RtreeNode *pNode, int *piIndex){
  RtreeNode *pParent = pNode->pParent;
  if( pParent ){
    return nodeRowidIndex(pRtree, pParent, pNode->iNode, piIndex);
  }
  *piIndex = -1;
  return SQLITE_OK;
}

/*
** Compare two search points.  Return negative, zero, or positive if the first
** is less than, equal to, or greater than the second.
**
** The rScore is the primary key.  Smaller rScore values come first.
** If the rScore is a tie, then use iLevel as the tie breaker with smaller
** iLevel values coming first.  In this way, if rScore is the same for all
** SearchPoints, then iLevel becomes the deciding factor and the result
** is a depth-first search, which is the desired default behavior.
*/
static int rtreeSearchPointCompare(
  const RtreeSearchPoint *pA,
  const RtreeSearchPoint *pB
){
  if( pA->rScore<pB->rScore ) return -1;
  if( pA->rScore>pB->rScore ) return +1;
  if( pA->iLevel<pB->iLevel ) return -1;
  if( pA->iLevel>pB->iLevel ) return +1;
  return 0;
}

/*
** Interchange to search points in a cursor.
*/
static void rtreeSearchPointSwap(RtreeCursor *p, int i, int j){
  RtreeSearchPoint t = p->aPoint[i];
  assert( i<j );
  p->aPoint[i] = p->aPoint[j];
  p->aPoint[j] = t;
  i++; j++;
  if( i<RTREE_CACHE_SZ ){
    if( j>=RTREE_CACHE_SZ ){
      nodeRelease(RTREE_OF_CURSOR(p), p->aNode[i]);
      p->aNode[i] = 0;
    }else{
      RtreeNode *pTemp = p->aNode[i];
      p->aNode[i] = p->aNode[j];
      p->aNode[j] = pTemp;
    }
  }
}

/*
** Return the search point with the lowest current score.
*/
static RtreeSearchPoint *rtreeSearchPointFirst(RtreeCursor *pCur){
  return pCur->bPoint ? &pCur->sPoint : pCur->nPoint ? pCur->aPoint : 0;
}

/*
** Get the RtreeNode for the search point with the lowest score.
*/
static RtreeNode *rtreeNodeOfFirstSearchPoint(RtreeCursor *pCur, int *pRC){
  sqlite3_int64 id;
  int ii = 1 - pCur->bPoint;
  assert( ii==0 || ii==1 );
  assert( pCur->bPoint || pCur->nPoint );
  if( pCur->aNode[ii]==0 ){
    assert( pRC!=0 );
    id = ii ? pCur->aPoint[0].id : pCur->sPoint.id;
    *pRC = nodeAcquire(RTREE_OF_CURSOR(pCur), id, 0, &pCur->aNode[ii]);
  }
  return pCur->aNode[ii];
}

/*
** Push a new element onto the priority queue
*/
static RtreeSearchPoint *rtreeEnqueue(
  RtreeCursor *pCur,    /* The cursor */
  RtreeDValue rScore,   /* Score for the new search point */
  u8 iLevel             /* Level for the new search point */
){
  int i, j;
  RtreeSearchPoint *pNew;
  if( pCur->nPoint>=pCur->nPointAlloc ){
    int nNew = pCur->nPointAlloc*2 + 8;
    pNew = sqlite3_realloc(pCur->aPoint, nNew*sizeof(pCur->aPoint[0]));
    if( pNew==0 ) return 0;
    pCur->aPoint = pNew;
    pCur->nPointAlloc = nNew;
  }
  i = pCur->nPoint++;
  pNew = pCur->aPoint + i;
  pNew->rScore = rScore;
  pNew->iLevel = iLevel;
  assert( iLevel>=0 && iLevel<=RTREE_MAX_DEPTH );
  while( i>0 ){
    RtreeSearchPoint *pParent;
    j = (i-1)/2;
    pParent = pCur->aPoint + j;
    if( rtreeSearchPointCompare(pNew, pParent)>=0 ) break;
    rtreeSearchPointSwap(pCur, j, i);
    i = j;
    pNew = pParent;
  }
  return pNew;
}

/*
** Allocate a new RtreeSearchPoint and return a pointer to it.  Return
** NULL if malloc fails.
*/
static RtreeSearchPoint *rtreeSearchPointNew(
  RtreeCursor *pCur,    /* The cursor */
  RtreeDValue rScore,   /* Score for the new search point */
  u8 iLevel             /* Level for the new search point */
){
  RtreeSearchPoint *pNew, *pFirst;
  pFirst = rtreeSearchPointFirst(pCur);
  pCur->anQueue[iLevel]++;
  if( pFirst==0
   || pFirst->rScore>rScore 
   || (pFirst->rScore==rScore && pFirst->iLevel>iLevel)
  ){
    if( pCur->bPoint ){
      int ii;
      pNew = rtreeEnqueue(pCur, rScore, iLevel);
      if( pNew==0 ) return 0;
      ii = (int)(pNew - pCur->aPoint) + 1;
      if( ii<RTREE_CACHE_SZ ){
        assert( pCur->aNode[ii]==0 );
        pCur->aNode[ii] = pCur->aNode[0];
       }else{
        nodeRelease(RTREE_OF_CURSOR(pCur), pCur->aNode[0]);
      }
      pCur->aNode[0] = 0;
      *pNew = pCur->sPoint;
    }
    pCur->sPoint.rScore = rScore;
    pCur->sPoint.iLevel = iLevel;
    pCur->bPoint = 1;
    return &pCur->sPoint;
  }else{
    return rtreeEnqueue(pCur, rScore, iLevel);
  }
}

#if 0
/* Tracing routines for the RtreeSearchPoint queue */
static void tracePoint(RtreeSearchPoint *p, int idx, RtreeCursor *pCur){
  if( idx<0 ){ printf(" s"); }else{ printf("%2d", idx); }
  printf(" %d.%05lld.%02d %g %d",
    p->iLevel, p->id, p->iCell, p->rScore, p->eWithin
  );
  idx++;
  if( idx<RTREE_CACHE_SZ ){
    printf(" %p\n", pCur->aNode[idx]);
  }else{
    printf("\n");
  }
}
static void traceQueue(RtreeCursor *pCur, const char *zPrefix){
  int ii;
  printf("=== %9s ", zPrefix);
  if( pCur->bPoint ){
    tracePoint(&pCur->sPoint, -1, pCur);
  }
  for(ii=0; ii<pCur->nPoint; ii++){
    if( ii>0 || pCur->bPoint ) printf("              ");
    tracePoint(&pCur->aPoint[ii], ii, pCur);
  }
}
# define RTREE_QUEUE_TRACE(A,B) traceQueue(A,B)
#else
# define RTREE_QUEUE_TRACE(A,B)   /* no-op */
#endif

/* Remove the search point with the lowest current score.
*/
static void rtreeSearchPointPop(RtreeCursor *p){
  int i, j, k, n;
  i = 1 - p->bPoint;
  assert( i==0 || i==1 );
  if( p->aNode[i] ){
    nodeRelease(RTREE_OF_CURSOR(p), p->aNode[i]);
    p->aNode[i] = 0;
  }
  if( p->bPoint ){
    p->anQueue[p->sPoint.iLevel]--;
    p->bPoint = 0;
  }else if( p->nPoint ){
    p->anQueue[p->aPoint[0].iLevel]--;
    n = --p->nPoint;
    p->aPoint[0] = p->aPoint[n];
    if( n<RTREE_CACHE_SZ-1 ){
      p->aNode[1] = p->aNode[n+1];
      p->aNode[n+1] = 0;
    }
    i = 0;
    while( (j = i*2+1)<n ){
      k = j+1;
      if( k<n && rtreeSearchPointCompare(&p->aPoint[k], &p->aPoint[j])<0 ){
        if( rtreeSearchPointCompare(&p->aPoint[k], &p->aPoint[i])<0 ){
          rtreeSearchPointSwap(p, i, k);
          i = k;
        }else{
          break;
        }
      }else{
        if( rtreeSearchPointCompare(&p->aPoint[j], &p->aPoint[i])<0 ){
          rtreeSearchPointSwap(p, i, j);
          i = j;
        }else{
          break;
        }
      }
    }
  }
}


/*
** Continue the search on cursor pCur until the front of the queue
** contains an entry suitable for returning as a result-set row,
** or until the RtreeSearchPoint queue is empty, indicating that the
** query has completed.
*/
static int rtreeStepToLeaf(RtreeCursor *pCur){
  RtreeSearchPoint *p;
  Rtree *pRtree = RTREE_OF_CURSOR(pCur);
  RtreeNode *pNode;
  int eWithin;
  int rc = SQLITE_OK;
  int nCell;
  int nConstraint = pCur->nConstraint;
  int ii;
  int eInt;
  RtreeSearchPoint x;

  eInt = pRtree->eCoordType==RTREE_COORD_INT32;
  while( (p = rtreeSearchPointFirst(pCur))!=0 && p->iLevel>0 ){
    pNode = rtreeNodeOfFirstSearchPoint(pCur, &rc);
    if( rc ) return rc;
    nCell = NCELL(pNode);
    assert( nCell<200 );
    while( p->iCell<nCell ){
      sqlite3_rtree_dbl rScore = (sqlite3_rtree_dbl)-1;
      u8 *pCellData = pNode->zData + (4+pRtree->nBytesPerCell*p->iCell);
      eWithin = FULLY_WITHIN;
      for(ii=0; ii<nConstraint; ii++){
        RtreeConstraint *pConstraint = pCur->aConstraint + ii;
        if( pConstraint->op>=RTREE_MATCH ){
          rc = rtreeCallbackConstraint(pConstraint, eInt, pCellData, p,
                                       &rScore, &eWithin);
          if( rc ) return rc;
        }else if( p->iLevel==1 ){
          rtreeLeafConstraint(pConstraint, eInt, pCellData, &eWithin);
        }else{
          rtreeNonleafConstraint(pConstraint, eInt, pCellData, &eWithin);
        }
        if( eWithin==NOT_WITHIN ) break;
      }
      p->iCell++;
      if( eWithin==NOT_WITHIN ) continue;
      x.iLevel = p->iLevel - 1;
      if( x.iLevel ){
        x.id = readInt64(pCellData);
        x.iCell = 0;
      }else{
        x.id = p->id;
        x.iCell = p->iCell - 1;
      }
      if( p->iCell>=nCell ){
        RTREE_QUEUE_TRACE(pCur, "POP-S:");
        rtreeSearchPointPop(pCur);
      }
      if( rScore<RTREE_ZERO ) rScore = RTREE_ZERO;
      p = rtreeSearchPointNew(pCur, rScore, x.iLevel);
      if( p==0 ) return SQLITE_NOMEM;
      p->eWithin = eWithin;
      p->id = x.id;
      p->iCell = x.iCell;
      RTREE_QUEUE_TRACE(pCur, "PUSH-S:");
      break;
    }
    if( p->iCell>=nCell ){
      RTREE_QUEUE_TRACE(pCur, "POP-Se:");
      rtreeSearchPointPop(pCur);
    }
  }
  pCur->atEOF = p==0;
  return SQLITE_OK;
}

/* 
** Rtree virtual table module xNext method.
*/
static int rtreeNext(sqlite3_vtab_cursor *pVtabCursor){
  RtreeCursor *pCsr = (RtreeCursor *)pVtabCursor;
  int rc = SQLITE_OK;

  /* Move to the next entry that matches the configured constraints. */
  RTREE_QUEUE_TRACE(pCsr, "POP-Nx:");
  rtreeSearchPointPop(pCsr);
  rc = rtreeStepToLeaf(pCsr);
  return rc;
}

/* 
** Rtree virtual table module xRowid method.
*/
static int rtreeRowid(sqlite3_vtab_cursor *pVtabCursor, sqlite_int64 *pRowid){
  RtreeCursor *pCsr = (RtreeCursor *)pVtabCursor;
  RtreeSearchPoint *p = rtreeSearchPointFirst(pCsr);
  int rc = SQLITE_OK;
  RtreeNode *pNode = rtreeNodeOfFirstSearchPoint(pCsr, &rc);
  if( rc==SQLITE_OK && p ){
    *pRowid = nodeGetRowid(RTREE_OF_CURSOR(pCsr), pNode, p->iCell);
  }
  return rc;
}

/* 
** Rtree virtual table module xColumn method.
*/
static int rtreeColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i){
  Rtree *pRtree = (Rtree *)cur->pVtab;
  RtreeCursor *pCsr = (RtreeCursor *)cur;
  RtreeSearchPoint *p = rtreeSearchPointFirst(pCsr);
  RtreeCoord c;
  int rc = SQLITE_OK;
  RtreeNode *pNode = rtreeNodeOfFirstSearchPoint(pCsr, &rc);

  if( rc ) return rc;
  if( p==0 ) return SQLITE_OK;
  if( i==0 ){
    sqlite3_result_int64(ctx, nodeGetRowid(pRtree, pNode, p->iCell));
  }else{
    if( rc ) return rc;
    nodeGetCoord(pRtree, pNode, p->iCell, i-1, &c);
#ifndef SQLITE_RTREE_INT_ONLY
    if( pRtree->eCoordType==RTREE_COORD_REAL32 ){
      sqlite3_result_double(ctx, c.f);
    }else
#endif
    {
      assert( pRtree->eCoordType==RTREE_COORD_INT32 );
      sqlite3_result_int(ctx, c.i);
    }
  }
  return SQLITE_OK;
}

/* 
** Use nodeAcquire() to obtain the leaf node containing the record with 
** rowid iRowid. If successful, set *ppLeaf to point to the node and
** return SQLITE_OK. If there is no such record in the table, set
** *ppLeaf to 0 and return SQLITE_OK. If an error occurs, set *ppLeaf
** to zero and return an SQLite error code.
*/
static int findLeafNode(
  Rtree *pRtree,              /* RTree to search */
  i64 iRowid,                 /* The rowid searching for */
  RtreeNode **ppLeaf,         /* Write the node here */
  sqlite3_int64 *piNode       /* Write the node-id here */
){
  int rc;
  *ppLeaf = 0;
  sqlite3_bind_int64(pRtree->pReadRowid, 1, iRowid);
  if( sqlite3_step(pRtree->pReadRowid)==SQLITE_ROW ){
    i64 iNode = sqlite3_column_int64(pRtree->pReadRowid, 0);
    if( piNode ) *piNode = iNode;
    rc = nodeAcquire(pRtree, iNode, 0, ppLeaf);
    sqlite3_reset(pRtree->pReadRowid);
  }else{
    rc = sqlite3_reset(pRtree->pReadRowid);
  }
  return rc;
}

/*
** This function is called to configure the RtreeConstraint object passed
** as the second argument for a MATCH constraint. The value passed as the
** first argument to this function is the right-hand operand to the MATCH
** operator.
*/
static int deserializeGeometry(sqlite3_value *pValue, RtreeConstraint *pCons){
  RtreeMatchArg *pBlob;              /* BLOB returned by geometry function */
  sqlite3_rtree_query_info *pInfo;   /* Callback information */
  int nBlob;                         /* Size of the geometry function blob */
  int nExpected;                     /* Expected size of the BLOB */

  /* Check that value is actually a blob. */
  if( sqlite3_value_type(pValue)!=SQLITE_BLOB ) return SQLITE_ERROR;

  /* Check that the blob is roughly the right size. */
  nBlob = sqlite3_value_bytes(pValue);
  if( nBlob<(int)sizeof(RtreeMatchArg) 
   || ((nBlob-sizeof(RtreeMatchArg))%sizeof(RtreeDValue))!=0
  ){
    return SQLITE_ERROR;
  }

  pInfo = (sqlite3_rtree_query_info*)sqlite3_malloc( sizeof(*pInfo)+nBlob );
  if( !pInfo ) return SQLITE_NOMEM;
  memset(pInfo, 0, sizeof(*pInfo));
  pBlob = (RtreeMatchArg*)&pInfo[1];

  memcpy(pBlob, sqlite3_value_blob(pValue), nBlob);
  nExpected = (int)(sizeof(RtreeMatchArg) +
                    (pBlob->nParam-1)*sizeof(RtreeDValue));
  if( pBlob->magic!=RTREE_GEOMETRY_MAGIC || nBlob!=nExpected ){
    sqlite3_free(pInfo);
    return SQLITE_ERROR;
  }
  pInfo->pContext = pBlob->cb.pContext;
  pInfo->nParam = pBlob->nParam;
  pInfo->aParam = pBlob->aParam;

  if( pBlob->cb.xGeom ){
    pCons->u.xGeom = pBlob->cb.xGeom;
  }else{
    pCons->op = RTREE_QUERY;
    pCons->u.xQueryFunc = pBlob->cb.xQueryFunc;
  }
  pCons->pInfo = pInfo;
  return SQLITE_OK;
}

/* 
** Rtree virtual table module xFilter method.
*/
static int rtreeFilter(
  sqlite3_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, sqlite3_value **argv
){
  Rtree *pRtree = (Rtree *)pVtabCursor->pVtab;
  RtreeCursor *pCsr = (RtreeCursor *)pVtabCursor;
  RtreeNode *pRoot = 0;
  int ii;
  int rc = SQLITE_OK;
  int iCell = 0;

  rtreeReference(pRtree);

  /* Reset the cursor to the same state as rtreeOpen() leaves it in. */
  freeCursorConstraints(pCsr);
  sqlite3_free(pCsr->aPoint);
  memset(pCsr, 0, sizeof(RtreeCursor));
  pCsr->base.pVtab = (sqlite3_vtab*)pRtree;

  pCsr->iStrategy = idxNum;
  if( idxNum==1 ){
    /* Special case - lookup by rowid. */
    RtreeNode *pLeaf;        /* Leaf on which the required cell resides */
    RtreeSearchPoint *p;     /* Search point for the the leaf */
    i64 iRowid = sqlite3_value_int64(argv[0]);
    i64 iNode = 0;
    rc = findLeafNode(pRtree, iRowid, &pLeaf, &iNode);
    if( rc==SQLITE_OK && pLeaf!=0 ){
      p = rtreeSearchPointNew(pCsr, RTREE_ZERO, 0);
      assert( p!=0 );  /* Always returns pCsr->sPoint */
      pCsr->aNode[0] = pLeaf;
      p->id = iNode;
      p->eWithin = PARTLY_WITHIN;
      rc = nodeRowidIndex(pRtree, pLeaf, iRowid, &iCell);
      p->iCell = iCell;
      RTREE_QUEUE_TRACE(pCsr, "PUSH-F1:");
    }else{
      pCsr->atEOF = 1;
    }
  }else{
    /* Normal case - r-tree scan. Set up the RtreeCursor.aConstraint array 
    ** with the configured constraints. 
    */
    rc = nodeAcquire(pRtree, 1, 0, &pRoot);
    if( rc==SQLITE_OK && argc>0 ){
      pCsr->aConstraint = sqlite3_malloc(sizeof(RtreeConstraint)*argc);
      pCsr->nConstraint = argc;
      if( !pCsr->aConstraint ){
        rc = SQLITE_NOMEM;
      }else{
        memset(pCsr->aConstraint, 0, sizeof(RtreeConstraint)*argc);
        memset(pCsr->anQueue, 0, sizeof(u32)*(pRtree->iDepth + 1));
        assert( (idxStr==0 && argc==0)
                || (idxStr && (int)strlen(idxStr)==argc*2) );
        for(ii=0; ii<argc; ii++){
          RtreeConstraint *p = &pCsr->aConstraint[ii];
          p->op = idxStr[ii*2];
          p->iCoord = idxStr[ii*2+1]-'0';
          if( p->op>=RTREE_MATCH ){
            /* A MATCH operator. The right-hand-side must be a blob that
            ** can be cast into an RtreeMatchArg object. One created using
            ** an sqlite3_rtree_geometry_callback() SQL user function.
            */
            rc = deserializeGeometry(argv[ii], p);
            if( rc!=SQLITE_OK ){
              break;
            }
            p->pInfo->nCoord = pRtree->nDim*2;
            p->pInfo->anQueue = pCsr->anQueue;
            p->pInfo->mxLevel = pRtree->iDepth + 1;
          }else{
#ifdef SQLITE_RTREE_INT_ONLY
            p->u.rValue = sqlite3_value_int64(argv[ii]);
#else
            p->u.rValue = sqlite3_value_double(argv[ii]);
#endif
          }
        }
      }
    }
    if( rc==SQLITE_OK ){
      RtreeSearchPoint *pNew;
      pNew = rtreeSearchPointNew(pCsr, RTREE_ZERO, pRtree->iDepth+1);
      if( pNew==0 ) return SQLITE_NOMEM;
      pNew->id = 1;
      pNew->iCell = 0;
      pNew->eWithin = PARTLY_WITHIN;
      assert( pCsr->bPoint==1 );
      pCsr->aNode[0] = pRoot;
      pRoot = 0;
      RTREE_QUEUE_TRACE(pCsr, "PUSH-Fm:");
      rc = rtreeStepToLeaf(pCsr);
    }
  }

  nodeRelease(pRtree, pRoot);
  rtreeRelease(pRtree);
  return rc;
}

/*
** Set the pIdxInfo->estimatedRows variable to nRow. Unless this
** extension is currently being used by a version of SQLite too old to
** support estimatedRows. In that case this function is a no-op.
*/
static void setEstimatedRows(sqlite3_index_info *pIdxInfo, i64 nRow){
#if SQLITE_VERSION_NUMBER>=3008002
  if( sqlite3_libversion_number()>=3008002 ){
    pIdxInfo->estimatedRows = nRow;
  }
#endif
}

/*
** Rtree virtual table module xBestIndex method. There are three
** table scan strategies to choose from (in order from most to 
** least desirable):
**
**   idxNum     idxStr        Strategy
**   ------------------------------------------------
**     1        Unused        Direct lookup by rowid.
**     2        See below     R-tree query or full-table scan.
**   ------------------------------------------------
**
** If strategy 1 is used, then idxStr is not meaningful. If strategy
** 2 is used, idxStr is formatted to contain 2 bytes for each 
** constraint used. The first two bytes of idxStr correspond to 
** the constraint in sqlite3_index_info.aConstraintUsage[] with
** (argvIndex==1) etc.
**
** The first of each pair of bytes in idxStr identifies the constraint
** operator as follows:
**
**   Operator    Byte Value
**   ----------------------
**      =        0x41 ('A')
**     <=        0x42 ('B')
**      <        0x43 ('C')
**     >=        0x44 ('D')
**      >        0x45 ('E')
**   MATCH       0x46 ('F')
**   ----------------------
**
** The second of each pair of bytes identifies the coordinate column
** to which the constraint applies. The leftmost coordinate column
** is 'a', the second from the left 'b' etc.
*/
static int rtreeBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo){
  Rtree *pRtree = (Rtree*)tab;
  int rc = SQLITE_OK;
  int ii;
  i64 nRow;                       /* Estimated rows returned by this scan */

  int iIdx = 0;
  char zIdxStr[RTREE_MAX_DIMENSIONS*8+1];
  memset(zIdxStr, 0, sizeof(zIdxStr));

  assert( pIdxInfo->idxStr==0 );
  for(ii=0; ii<pIdxInfo->nConstraint && iIdx<(int)(sizeof(zIdxStr)-1); ii++){
    struct sqlite3_index_constraint *p = &pIdxInfo->aConstraint[ii];

    if( p->usable && p->iColumn==0 && p->op==SQLITE_INDEX_CONSTRAINT_EQ ){
      /* We have an equality constraint on the rowid. Use strategy 1. */
      int jj;
      for(jj=0; jj<ii; jj++){
        pIdxInfo->aConstraintUsage[jj].argvIndex = 0;
        pIdxInfo->aConstraintUsage[jj].omit = 0;
      }
      pIdxInfo->idxNum = 1;
      pIdxInfo->aConstraintUsage[ii].argvIndex = 1;
      pIdxInfo->aConstraintUsage[jj].omit = 1;

      /* This strategy involves a two rowid lookups on an B-Tree structures
      ** and then a linear search of an R-Tree node. This should be 
      ** considered almost as quick as a direct rowid lookup (for which 
      ** sqlite uses an internal cost of 0.0). It is expected to return
      ** a single row.
      */ 
      pIdxInfo->estimatedCost = 30.0;
      setEstimatedRows(pIdxInfo, 1);
      return SQLITE_OK;
    }

    if( p->usable && (p->iColumn>0 || p->op==SQLITE_INDEX_CONSTRAINT_MATCH) ){
      u8 op;
      switch( p->op ){
        case SQLITE_INDEX_CONSTRAINT_EQ: op = RTREE_EQ; break;
        case SQLITE_INDEX_CONSTRAINT_GT: op = RTREE_GT; break;
        case SQLITE_INDEX_CONSTRAINT_LE: op = RTREE_LE; break;
        case SQLITE_INDEX_CONSTRAINT_LT: op = RTREE_LT; break;
        case SQLITE_INDEX_CONSTRAINT_GE: op = RTREE_GE; break;
        default:
          assert( p->op==SQLITE_INDEX_CONSTRAINT_MATCH );
          op = RTREE_MATCH; 
          break;
      }
      zIdxStr[iIdx++] = op;
      zIdxStr[iIdx++] = p->iColumn - 1 + '0';
      pIdxInfo->aConstraintUsage[ii].argvIndex = (iIdx/2);
      pIdxInfo->aConstraintUsage[ii].omit = 1;
    }
  }

  pIdxInfo->idxNum = 2;
  pIdxInfo->needToFreeIdxStr = 1;
  if( iIdx>0 && 0==(pIdxInfo->idxStr = sqlite3_mprintf("%s", zIdxStr)) ){
    return SQLITE_NOMEM;
  }

  nRow = pRtree->nRowEst / (iIdx + 1);
  pIdxInfo->estimatedCost = (double)6.0 * (double)nRow;
  setEstimatedRows(pIdxInfo, nRow);

  return rc;
}

/*
** Return the N-dimensional volumn of the cell stored in *p.
*/
static RtreeDValue cellArea(Rtree *pRtree, RtreeCell *p){
  RtreeDValue area = (RtreeDValue)1;
  int ii;
  for(ii=0; ii<(pRtree->nDim*2); ii+=2){
    area = (area * (DCOORD(p->aCoord[ii+1]) - DCOORD(p->aCoord[ii])));
  }
  return area;
}

/*
** Return the margin length of cell p. The margin length is the sum
** of the objects size in each dimension.
*/
static RtreeDValue cellMargin(Rtree *pRtree, RtreeCell *p){
  RtreeDValue margin = (RtreeDValue)0;
  int ii;
  for(ii=0; ii<(pRtree->nDim*2); ii+=2){
    margin += (DCOORD(p->aCoord[ii+1]) - DCOORD(p->aCoord[ii]));
  }
  return margin;
}

/*
** Store the union of cells p1 and p2 in p1.
*/
static void cellUnion(Rtree *pRtree, RtreeCell *p1, RtreeCell *p2){
  int ii;
  if( pRtree->eCoordType==RTREE_COORD_REAL32 ){
    for(ii=0; ii<(pRtree->nDim*2); ii+=2){
      p1->aCoord[ii].f = MIN(p1->aCoord[ii].f, p2->aCoord[ii].f);
      p1->aCoord[ii+1].f = MAX(p1->aCoord[ii+1].f, p2->aCoord[ii+1].f);
    }
  }else{
    for(ii=0; ii<(pRtree->nDim*2); ii+=2){
      p1->aCoord[ii].i = MIN(p1->aCoord[ii].i, p2->aCoord[ii].i);
      p1->aCoord[ii+1].i = MAX(p1->aCoord[ii+1].i, p2->aCoord[ii+1].i);
    }
  }
}

/*
** Return true if the area covered by p2 is a subset of the area covered
** by p1. False otherwise.
*/
static int cellContains(Rtree *pRtree, RtreeCell *p1, RtreeCell *p2){
  int ii;
  int isInt = (pRtree->eCoordType==RTREE_COORD_INT32);
  for(ii=0; ii<(pRtree->nDim*2); ii+=2){
    RtreeCoord *a1 = &p1->aCoord[ii];
    RtreeCoord *a2 = &p2->aCoord[ii];
    if( (!isInt && (a2[0].f<a1[0].f || a2[1].f>a1[1].f)) 
     || ( isInt && (a2[0].i<a1[0].i || a2[1].i>a1[1].i)) 
    ){
      return 0;
    }
  }
  return 1;
}

/*
** Return the amount cell p would grow by if it were unioned with pCell.
*/
static RtreeDValue cellGrowth(Rtree *pRtree, RtreeCell *p, RtreeCell *pCell){
  RtreeDValue area;
  RtreeCell cell;
  memcpy(&cell, p, sizeof(RtreeCell));
  area = cellArea(pRtree, &cell);
  cellUnion(pRtree, &cell, pCell);
  return (cellArea(pRtree, &cell)-area);
}

static RtreeDValue cellOverlap(
  Rtree *pRtree, 
  RtreeCell *p, 
  RtreeCell *aCell, 
  int nCell
){
  int ii;
  RtreeDValue overlap = RTREE_ZERO;
  for(ii=0; ii<nCell; ii++){
    int jj;
    RtreeDValue o = (RtreeDValue)1;
    for(jj=0; jj<(pRtree->nDim*2); jj+=2){
      RtreeDValue x1, x2;
      x1 = MAX(DCOORD(p->aCoord[jj]), DCOORD(aCell[ii].aCoord[jj]));
      x2 = MIN(DCOORD(p->aCoord[jj+1]), DCOORD(aCell[ii].aCoord[jj+1]));
      if( x2<x1 ){
        o = (RtreeDValue)0;
        break;
      }else{
        o = o * (x2-x1);
      }
    }
    overlap += o;
  }
  return overlap;
}


/*
** This function implements the ChooseLeaf algorithm from Gutman[84].
** ChooseSubTree in r*tree terminology.
*/
static int ChooseLeaf(
  Rtree *pRtree,               /* Rtree table */
  RtreeCell *pCell,            /* Cell to insert into rtree */
  int iHeight,                 /* Height of sub-tree rooted at pCell */
  RtreeNode **ppLeaf           /* OUT: Selected leaf page */
){
  int rc;
  int ii;
  RtreeNode *pNode;
  rc = nodeAcquire(pRtree, 1, 0, &pNode);

  for(ii=0; rc==SQLITE_OK && ii<(pRtree->iDepth-iHeight); ii++){
    int iCell;
    sqlite3_int64 iBest = 0;

    RtreeDValue fMinGrowth = RTREE_ZERO;
    RtreeDValue fMinArea = RTREE_ZERO;

    int nCell = NCELL(pNode);
    RtreeCell cell;
    RtreeNode *pChild;

    RtreeCell *aCell = 0;

    /* Select the child node which will be enlarged the least if pCell
    ** is inserted into it. Resolve ties by choosing the entry with
    ** the smallest area.
    */
    for(iCell=0; iCell<nCell; iCell++){
      int bBest = 0;
      RtreeDValue growth;
      RtreeDValue area;
      nodeGetCell(pRtree, pNode, iCell, &cell);
      growth = cellGrowth(pRtree, &cell, pCell);
      area = cellArea(pRtree, &cell);
      if( iCell==0||growth<fMinGrowth||(growth==fMinGrowth && area<fMinArea) ){
        bBest = 1;
      }
      if( bBest ){
        fMinGrowth = growth;
        fMinArea = area;
        iBest = cell.iRowid;
      }
    }

    sqlite3_free(aCell);
    rc = nodeAcquire(pRtree, iBest, pNode, &pChild);
    nodeRelease(pRtree, pNode);
    pNode = pChild;
  }

  *ppLeaf = pNode;
  return rc;
}

/*
** A cell with the same content as pCell has just been inserted into
** the node pNode. This function updates the bounding box cells in
** all ancestor elements.
*/
static int AdjustTree(
  Rtree *pRtree,                    /* Rtree table */
  RtreeNode *pNode,                 /* Adjust ancestry of this node. */
  RtreeCell *pCell                  /* This cell was just inserted */
){
  RtreeNode *p = pNode;
  while( p->pParent ){
    RtreeNode *pParent = p->pParent;
    RtreeCell cell;
    int iCell;

    if( nodeParentIndex(pRtree, p, &iCell) ){
      return SQLITE_CORRUPT_VTAB;
    }

    nodeGetCell(pRtree, pParent, iCell, &cell);
    if( !cellContains(pRtree, &cell, pCell) ){
      cellUnion(pRtree, &cell, pCell);
      nodeOverwriteCell(pRtree, pParent, &cell, iCell);
    }
 
    p = pParent;
  }
  return SQLITE_OK;
}

/*
** Write mapping (iRowid->iNode) to the <rtree>_rowid table.
*/
static int rowidWrite(Rtree *pRtree, sqlite3_int64 iRowid, sqlite3_int64 iNode){
  sqlite3_bind_int64(pRtree->pWriteRowid, 1, iRowid);
  sqlite3_bind_int64(pRtree->pWriteRowid, 2, iNode);
  sqlite3_step(pRtree->pWriteRowid);
  return sqlite3_reset(pRtree->pWriteRowid);
}

/*
** Write mapping (iNode->iPar) to the <rtree>_parent table.
*/
static int parentWrite(Rtree *pRtree, sqlite3_int64 iNode, sqlite3_int64 iPar){
  sqlite3_bind_int64(pRtree->pWriteParent, 1, iNode);
  sqlite3_bind_int64(pRtree->pWriteParent, 2, iPar);
  sqlite3_step(pRtree->pWriteParent);
  return sqlite3_reset(pRtree->pWriteParent);
}

static int rtreeInsertCell(Rtree *, RtreeNode *, RtreeCell *, int);


/*
** Arguments aIdx, aDistance and aSpare all point to arrays of size
** nIdx. The aIdx array contains the set of integers from 0 to 
** (nIdx-1) in no particular order. This function sorts the values
** in aIdx according to the indexed values in aDistance. For
** example, assuming the inputs:
**
**   aIdx      = { 0,   1,   2,   3 }
**   aDistance = { 5.0, 2.0, 7.0, 6.0 }
**
** this function sets the aIdx array to contain:
**
**   aIdx      = { 0,   1,   2,   3 }
**
** The aSpare array is used as temporary working space by the
** sorting algorithm.
*/
static void SortByDistance(
  int *aIdx, 
  int nIdx, 
  RtreeDValue *aDistance, 
  int *aSpare
){
  if( nIdx>1 ){
    int iLeft = 0;
    int iRight = 0;

    int nLeft = nIdx/2;
    int nRight = nIdx-nLeft;
    int *aLeft = aIdx;
    int *aRight = &aIdx[nLeft];

    SortByDistance(aLeft, nLeft, aDistance, aSpare);
    SortByDistance(aRight, nRight, aDistance, aSpare);

    memcpy(aSpare, aLeft, sizeof(int)*nLeft);
    aLeft = aSpare;

    while( iLeft<nLeft || iRight<nRight ){
      if( iLeft==nLeft ){
        aIdx[iLeft+iRight] = aRight[iRight];
        iRight++;
      }else if( iRight==nRight ){
        aIdx[iLeft+iRight] = aLeft[iLeft];
        iLeft++;
      }else{
        RtreeDValue fLeft = aDistance[aLeft[iLeft]];
        RtreeDValue fRight = aDistance[aRight[iRight]];
        if( fLeft<fRight ){
          aIdx[iLeft+iRight] = aLeft[iLeft];
          iLeft++;
        }else{
          aIdx[iLeft+iRight] = aRight[iRight];
          iRight++;
        }
      }
    }

#if 0
    /* Check that the sort worked */
    {
      int jj;
      for(jj=1; jj<nIdx; jj++){
        RtreeDValue left = aDistance[aIdx[jj-1]];
        RtreeDValue right = aDistance[aIdx[jj]];
        assert( left<=right );
      }
    }
#endif
  }
}

/*
** Arguments aIdx, aCell and aSpare all point to arrays of size
** nIdx. The aIdx array contains the set of integers from 0 to 
** (nIdx-1) in no particular order. This function sorts the values
** in aIdx according to dimension iDim of the cells in aCell. The
** minimum value of dimension iDim is considered first, the
** maximum used to break ties.
**
** The aSpare array is used as temporary working space by the
** sorting algorithm.
*/
static void SortByDimension(
  Rtree *pRtree,
  int *aIdx, 
  int nIdx, 
  int iDim, 
  RtreeCell *aCell, 
  int *aSpare
){
  if( nIdx>1 ){

    int iLeft = 0;
    int iRight = 0;

    int nLeft = nIdx/2;
    int nRight = nIdx-nLeft;
    int *aLeft = aIdx;
    int *aRight = &aIdx[nLeft];

    SortByDimension(pRtree, aLeft, nLeft, iDim, aCell, aSpare);
    SortByDimension(pRtree, aRight, nRight, iDim, aCell, aSpare);

    memcpy(aSpare, aLeft, sizeof(int)*nLeft);
    aLeft = aSpare;
    while( iLeft<nLeft || iRight<nRight ){
      RtreeDValue xleft1 = DCOORD(aCell[aLeft[iLeft]].aCoord[iDim*2]);
      RtreeDValue xleft2 = DCOORD(aCell[aLeft[iLeft]].aCoord[iDim*2+1]);
      RtreeDValue xright1 = DCOORD(aCell[aRight[iRight]].aCoord[iDim*2]);
      RtreeDValue xright2 = DCOORD(aCell[aRight[iRight]].aCoord[iDim*2+1]);
      if( (iLeft!=nLeft) && ((iRight==nRight)
       || (xleft1<xright1)
       || (xleft1==xright1 && xleft2<xright2)
      )){
        aIdx[iLeft+iRight] = aLeft[iLeft];
        iLeft++;
      }else{
        aIdx[iLeft+iRight] = aRight[iRight];
        iRight++;
      }
    }

#if 0
    /* Check that the sort worked */
    {
      int jj;
      for(jj=1; jj<nIdx; jj++){
        RtreeDValue xleft1 = aCell[aIdx[jj-1]].aCoord[iDim*2];
        RtreeDValue xleft2 = aCell[aIdx[jj-1]].aCoord[iDim*2+1];
        RtreeDValue xright1 = aCell[aIdx[jj]].aCoord[iDim*2];
        RtreeDValue xright2 = aCell[aIdx[jj]].aCoord[iDim*2+1];
        assert( xleft1<=xright1 && (xleft1<xright1 || xleft2<=xright2) );
      }
    }
#endif
  }
}

/*
** Implementation of the R*-tree variant of SplitNode from Beckman[1990].
*/
static int splitNodeStartree(
  Rtree *pRtree,
  RtreeCell *aCell,
  int nCell,
  RtreeNode *pLeft,
  RtreeNode *pRight,
  RtreeCell *pBboxLeft,
  RtreeCell *pBboxRight
){
  int **aaSorted;
  int *aSpare;
  int ii;

  int iBestDim = 0;
  int iBestSplit = 0;
  RtreeDValue fBestMargin = RTREE_ZERO;

  int nByte = (pRtree->nDim+1)*(sizeof(int*)+nCell*sizeof(int));

  aaSorted = (int **)sqlite3_malloc(nByte);
  if( !aaSorted ){
    return SQLITE_NOMEM;
  }

  aSpare = &((int *)&aaSorted[pRtree->nDim])[pRtree->nDim*nCell];
  memset(aaSorted, 0, nByte);
  for(ii=0; ii<pRtree->nDim; ii++){
    int jj;
    aaSorted[ii] = &((int *)&aaSorted[pRtree->nDim])[ii*nCell];
    for(jj=0; jj<nCell; jj++){
      aaSorted[ii][jj] = jj;
    }
    SortByDimension(pRtree, aaSorted[ii], nCell, ii, aCell, aSpare);
  }

  for(ii=0; ii<pRtree->nDim; ii++){
    RtreeDValue margin = RTREE_ZERO;
    RtreeDValue fBestOverlap = RTREE_ZERO;
    RtreeDValue fBestArea = RTREE_ZERO;
    int iBestLeft = 0;
    int nLeft;

    for(
      nLeft=RTREE_MINCELLS(pRtree); 
      nLeft<=(nCell-RTREE_MINCELLS(pRtree)); 
      nLeft++
    ){
      RtreeCell left;
      RtreeCell right;
      int kk;
      RtreeDValue overlap;
      RtreeDValue area;

      memcpy(&left, &aCell[aaSorted[ii][0]], sizeof(RtreeCell));
      memcpy(&right, &aCell[aaSorted[ii][nCell-1]], sizeof(RtreeCell));
      for(kk=1; kk<(nCell-1); kk++){
        if( kk<nLeft ){
          cellUnion(pRtree, &left, &aCell[aaSorted[ii][kk]]);
        }else{
          cellUnion(pRtree, &right, &aCell[aaSorted[ii][kk]]);
        }
      }
      margin += cellMargin(pRtree, &left);
      margin += cellMargin(pRtree, &right);
      overlap = cellOverlap(pRtree, &left, &right, 1);
      area = cellArea(pRtree, &left) + cellArea(pRtree, &right);
      if( (nLeft==RTREE_MINCELLS(pRtree))
       || (overlap<fBestOverlap)
       || (overlap==fBestOverlap && area<fBestArea)
      ){
        iBestLeft = nLeft;
        fBestOverlap = overlap;
        fBestArea = area;
      }
    }

    if( ii==0 || margin<fBestMargin ){
      iBestDim = ii;
      fBestMargin = margin;
      iBestSplit = iBestLeft;
    }
  }

  memcpy(pBboxLeft, &aCell[aaSorted[iBestDim][0]], sizeof(RtreeCell));
  memcpy(pBboxRight, &aCell[aaSorted[iBestDim][iBestSplit]], sizeof(RtreeCell));
  for(ii=0; ii<nCell; ii++){
    RtreeNode *pTarget = (ii<iBestSplit)?pLeft:pRight;
    RtreeCell *pBbox = (ii<iBestSplit)?pBboxLeft:pBboxRight;
    RtreeCell *pCell = &aCell[aaSorted[iBestDim][ii]];
    nodeInsertCell(pRtree, pTarget, pCell);
    cellUnion(pRtree, pBbox, pCell);
  }

  sqlite3_free(aaSorted);
  return SQLITE_OK;
}


static int updateMapping(
  Rtree *pRtree, 
  i64 iRowid, 
  RtreeNode *pNode, 
  int iHeight
){
  int (*xSetMapping)(Rtree *, sqlite3_int64, sqlite3_int64);
  xSetMapping = ((iHeight==0)?rowidWrite:parentWrite);
  if( iHeight>0 ){
    RtreeNode *pChild = nodeHashLookup(pRtree, iRowid);
    if( pChild ){
      nodeRelease(pRtree, pChild->pParent);
      nodeReference(pNode);
      pChild->pParent = pNode;
    }
  }
  return xSetMapping(pRtree, iRowid, pNode->iNode);
}

static int SplitNode(
  Rtree *pRtree,
  RtreeNode *pNode,
  RtreeCell *pCell,
  int iHeight
){
  int i;
  int newCellIsRight = 0;

  int rc = SQLITE_OK;
  int nCell = NCELL(pNode);
  RtreeCell *aCell;
  int *aiUsed;

  RtreeNode *pLeft = 0;
  RtreeNode *pRight = 0;

  RtreeCell leftbbox;
  RtreeCell rightbbox;

  /* Allocate an array and populate it with a copy of pCell and 
  ** all cells from node pLeft. Then zero the original node.
  */
  aCell = sqlite3_malloc((sizeof(RtreeCell)+sizeof(int))*(nCell+1));
  if( !aCell ){
    rc = SQLITE_NOMEM;
    goto splitnode_out;
  }
  aiUsed = (int *)&aCell[nCell+1];
  memset(aiUsed, 0, sizeof(int)*(nCell+1));
  for(i=0; i<nCell; i++){
    nodeGetCell(pRtree, pNode, i, &aCell[i]);
  }
  nodeZero(pRtree, pNode);
  memcpy(&aCell[nCell], pCell, sizeof(RtreeCell));
  nCell++;

  if( pNode->iNode==1 ){
    pRight = nodeNew(pRtree, pNode);
    pLeft = nodeNew(pRtree, pNode);
    pRtree->iDepth++;
    pNode->isDirty = 1;
    writeInt16(pNode->zData, pRtree->iDepth);
  }else{
    pLeft = pNode;
    pRight = nodeNew(pRtree, pLeft->pParent);
    nodeReference(pLeft);
  }

  if( !pLeft || !pRight ){
    rc = SQLITE_NOMEM;
    goto splitnode_out;
  }

  memset(pLeft->zData, 0, pRtree->iNodeSize);
  memset(pRight->zData, 0, pRtree->iNodeSize);

  rc = splitNodeStartree(pRtree, aCell, nCell, pLeft, pRight,
                         &leftbbox, &rightbbox);
  if( rc!=SQLITE_OK ){
    goto splitnode_out;
  }

  /* Ensure both child nodes have node numbers assigned to them by calling
  ** nodeWrite(). Node pRight always needs a node number, as it was created
  ** by nodeNew() above. But node pLeft sometimes already has a node number.
  ** In this case avoid the all to nodeWrite().
  */
  if( SQLITE_OK!=(rc = nodeWrite(pRtree, pRight))
   || (0==pLeft->iNode && SQLITE_OK!=(rc = nodeWrite(pRtree, pLeft)))
  ){
    goto splitnode_out;
  }

  rightbbox.iRowid = pRight->iNode;
  leftbbox.iRowid = pLeft->iNode;

  if( pNode->iNode==1 ){
    rc = rtreeInsertCell(pRtree, pLeft->pParent, &leftbbox, iHeight+1);
    if( rc!=SQLITE_OK ){
      goto splitnode_out;
    }
  }else{
    RtreeNode *pParent = pLeft->pParent;
    int iCell;
    rc = nodeParentIndex(pRtree, pLeft, &iCell);
    if( rc==SQLITE_OK ){
      nodeOverwriteCell(pRtree, pParent, &leftbbox, iCell);
      rc = AdjustTree(pRtree, pParent, &leftbbox);
    }
    if( rc!=SQLITE_OK ){
      goto splitnode_out;
    }
  }
  if( (rc = rtreeInsertCell(pRtree, pRight->pParent, &rightbbox, iHeight+1)) ){
    goto splitnode_out;
  }

  for(i=0; i<NCELL(pRight); i++){
    i64 iRowid = nodeGetRowid(pRtree, pRight, i);
    rc = updateMapping(pRtree, iRowid, pRight, iHeight);
    if( iRowid==pCell->iRowid ){
      newCellIsRight = 1;
    }
    if( rc!=SQLITE_OK ){
      goto splitnode_out;
    }
  }
  if( pNode->iNode==1 ){
    for(i=0; i<NCELL(pLeft); i++){
      i64 iRowid = nodeGetRowid(pRtree, pLeft, i);
      rc = updateMapping(pRtree, iRowid, pLeft, iHeight);
      if( rc!=SQLITE_OK ){
        goto splitnode_out;
      }
    }
  }else if( newCellIsRight==0 ){
    rc = updateMapping(pRtree, pCell->iRowid, pLeft, iHeight);
  }

  if( rc==SQLITE_OK ){
    rc = nodeRelease(pRtree, pRight);
    pRight = 0;
  }
  if( rc==SQLITE_OK ){
    rc = nodeRelease(pRtree, pLeft);
    pLeft = 0;
  }

splitnode_out:
  nodeRelease(pRtree, pRight);
  nodeRelease(pRtree, pLeft);
  sqlite3_free(aCell);
  return rc;
}

/*
** If node pLeaf is not the root of the r-tree and its pParent pointer is 
** still NULL, load all ancestor nodes of pLeaf into memory and populate
** the pLeaf->pParent chain all the way up to the root node.
**
** This operation is required when a row is deleted (or updated - an update
** is implemented as a delete followed by an insert). SQLite provides the
** rowid of the row to delete, which can be used to find the leaf on which
** the entry resides (argument pLeaf). Once the leaf is located, this 
** function is called to determine its ancestry.
*/
static int fixLeafParent(Rtree *pRtree, RtreeNode *pLeaf){
  int rc = SQLITE_OK;
  RtreeNode *pChild = pLeaf;
  while( rc==SQLITE_OK && pChild->iNode!=1 && pChild->pParent==0 ){
    int rc2 = SQLITE_OK;          /* sqlite3_reset() return code */
    sqlite3_bind_int64(pRtree->pReadParent, 1, pChild->iNode);
    rc = sqlite3_step(pRtree->pReadParent);
    if( rc==SQLITE_ROW ){
      RtreeNode *pTest;           /* Used to test for reference loops */
      i64 iNode;                  /* Node number of parent node */

      /* Before setting pChild->pParent, test that we are not creating a
      ** loop of references (as we would if, say, pChild==pParent). We don't
      ** want to do this as it leads to a memory leak when trying to delete
      ** the referenced counted node structures.
      */
      iNode = sqlite3_column_int64(pRtree->pReadParent, 0);
      for(pTest=pLeaf; pTest && pTest->iNode!=iNode; pTest=pTest->pParent);
      if( !pTest ){
        rc2 = nodeAcquire(pRtree, iNode, 0, &pChild->pParent);
      }
    }
    rc = sqlite3_reset(pRtree->pReadParent);
    if( rc==SQLITE_OK ) rc = rc2;
    if( rc==SQLITE_OK && !pChild->pParent ) rc = SQLITE_CORRUPT_VTAB;
    pChild = pChild->pParent;
  }
  return rc;
}

static int deleteCell(Rtree *, RtreeNode *, int, int);

static int removeNode(Rtree *pRtree, RtreeNode *pNode, int iHeight){
  int rc;
  int rc2;
  RtreeNode *pParent = 0;
  int iCell;

  assert( pNode->nRef==1 );

  /* Remove the entry in the parent cell. */
  rc = nodeParentIndex(pRtree, pNode, &iCell);
  if( rc==SQLITE_OK ){
    pParent = pNode->pParent;
    pNode->pParent = 0;
    rc = deleteCell(pRtree, pParent, iCell, iHeight+1);
  }
  rc2 = nodeRelease(pRtree, pParent);
  if( rc==SQLITE_OK ){
    rc = rc2;
  }
  if( rc!=SQLITE_OK ){
    return rc;
  }

  /* Remove the xxx_node entry. */
  sqlite3_bind_int64(pRtree->pDeleteNode, 1, pNode->iNode);
  sqlite3_step(pRtree->pDeleteNode);
  if( SQLITE_OK!=(rc = sqlite3_reset(pRtree->pDeleteNode)) ){
    return rc;
  }

  /* Remove the xxx_parent entry. */
  sqlite3_bind_int64(pRtree->pDeleteParent, 1, pNode->iNode);
  sqlite3_step(pRtree->pDeleteParent);
  if( SQLITE_OK!=(rc = sqlite3_reset(pRtree->pDeleteParent)) ){
    return rc;
  }
  
  /* Remove the node from the in-memory hash table and link it into
  ** the Rtree.pDeleted list. Its contents will be re-inserted later on.
  */
  nodeHashDelete(pRtree, pNode);
  pNode->iNode = iHeight;
  pNode->pNext = pRtree->pDeleted;
  pNode->nRef++;
  pRtree->pDeleted = pNode;

  return SQLITE_OK;
}

static int fixBoundingBox(Rtree *pRtree, RtreeNode *pNode){
  RtreeNode *pParent = pNode->pParent;
  int rc = SQLITE_OK; 
  if( pParent ){
    int ii; 
    int nCell = NCELL(pNode);
    RtreeCell box;                            /* Bounding box for pNode */
    nodeGetCell(pRtree, pNode, 0, &box);
    for(ii=1; ii<nCell; ii++){
      RtreeCell cell;
      nodeGetCell(pRtree, pNode, ii, &cell);
      cellUnion(pRtree, &box, &cell);
    }
    box.iRowid = pNode->iNode;
    rc = nodeParentIndex(pRtree, pNode, &ii);
    if( rc==SQLITE_OK ){
      nodeOverwriteCell(pRtree, pParent, &box, ii);
      rc = fixBoundingBox(pRtree, pParent);
    }
  }
  return rc;
}

/*
** Delete the cell at index iCell of node pNode. After removing the
** cell, adjust the r-tree data structure if required.
*/
static int deleteCell(Rtree *pRtree, RtreeNode *pNode, int iCell, int iHeight){
  RtreeNode *pParent;
  int rc;

  if( SQLITE_OK!=(rc = fixLeafParent(pRtree, pNode)) ){
    return rc;
  }

  /* Remove the cell from the node. This call just moves bytes around
  ** the in-memory node image, so it cannot fail.
  */
  nodeDeleteCell(pRtree, pNode, iCell);

  /* If the node is not the tree root and now has less than the minimum
  ** number of cells, remove it from the tree. Otherwise, update the
  ** cell in the parent node so that it tightly contains the updated
  ** node.
  */
  pParent = pNode->pParent;
  assert( pParent || pNode->iNode==1 );
  if( pParent ){
    if( NCELL(pNode)<RTREE_MINCELLS(pRtree) ){
      rc = removeNode(pRtree, pNode, iHeight);
    }else{
      rc = fixBoundingBox(pRtree, pNode);
    }
  }

  return rc;
}

static int Reinsert(
  Rtree *pRtree, 
  RtreeNode *pNode, 
  RtreeCell *pCell, 
  int iHeight
){
  int *aOrder;
  int *aSpare;
  RtreeCell *aCell;
  RtreeDValue *aDistance;
  int nCell;
  RtreeDValue aCenterCoord[RTREE_MAX_DIMENSIONS];
  int iDim;
  int ii;
  int rc = SQLITE_OK;
  int n;

  memset(aCenterCoord, 0, sizeof(RtreeDValue)*RTREE_MAX_DIMENSIONS);

  nCell = NCELL(pNode)+1;
  n = (nCell+1)&(~1);

  /* Allocate the buffers used by this operation. The allocation is
  ** relinquished before this function returns.
  */
  aCell = (RtreeCell *)sqlite3_malloc(n * (
    sizeof(RtreeCell)     +         /* aCell array */
    sizeof(int)           +         /* aOrder array */
    sizeof(int)           +         /* aSpare array */
    sizeof(RtreeDValue)             /* aDistance array */
  ));
  if( !aCell ){
    return SQLITE_NOMEM;
  }
  aOrder    = (int *)&aCell[n];
  aSpare    = (int *)&aOrder[n];
  aDistance = (RtreeDValue *)&aSpare[n];

  for(ii=0; ii<nCell; ii++){
    if( ii==(nCell-1) ){
      memcpy(&aCell[ii], pCell, sizeof(RtreeCell));
    }else{
      nodeGetCell(pRtree, pNode, ii, &aCell[ii]);
    }
    aOrder[ii] = ii;
    for(iDim=0; iDim<pRtree->nDim; iDim++){
      aCenterCoord[iDim] += DCOORD(aCell[ii].aCoord[iDim*2]);
      aCenterCoord[iDim] += DCOORD(aCell[ii].aCoord[iDim*2+1]);
    }
  }
  for(iDim=0; iDim<pRtree->nDim; iDim++){
    aCenterCoord[iDim] = (aCenterCoord[iDim]/(nCell*(RtreeDValue)2));
  }

  for(ii=0; ii<nCell; ii++){
    aDistance[ii] = RTREE_ZERO;
    for(iDim=0; iDim<pRtree->nDim; iDim++){
      RtreeDValue coord = (DCOORD(aCell[ii].aCoord[iDim*2+1]) - 
                               DCOORD(aCell[ii].aCoord[iDim*2]));
      aDistance[ii] += (coord-aCenterCoord[iDim])*(coord-aCenterCoord[iDim]);
    }
  }

  SortByDistance(aOrder, nCell, aDistance, aSpare);
  nodeZero(pRtree, pNode);

  for(ii=0; rc==SQLITE_OK && ii<(nCell-(RTREE_MINCELLS(pRtree)+1)); ii++){
    RtreeCell *p = &aCell[aOrder[ii]];
    nodeInsertCell(pRtree, pNode, p);
    if( p->iRowid==pCell->iRowid ){
      if( iHeight==0 ){
        rc = rowidWrite(pRtree, p->iRowid, pNode->iNode);
      }else{
        rc = parentWrite(pRtree, p->iRowid, pNode->iNode);
      }
    }
  }
  if( rc==SQLITE_OK ){
    rc = fixBoundingBox(pRtree, pNode);
  }
  for(; rc==SQLITE_OK && ii<nCell; ii++){
    /* Find a node to store this cell in. pNode->iNode currently contains
    ** the height of the sub-tree headed by the cell.
    */
    RtreeNode *pInsert;
    RtreeCell *p = &aCell[aOrder[ii]];
    rc = ChooseLeaf(pRtree, p, iHeight, &pInsert);
    if( rc==SQLITE_OK ){
      int rc2;
      rc = rtreeInsertCell(pRtree, pInsert, p, iHeight);
      rc2 = nodeRelease(pRtree, pInsert);
      if( rc==SQLITE_OK ){
        rc = rc2;
      }
    }
  }

  sqlite3_free(aCell);
  return rc;
}

/*
** Insert cell pCell into node pNode. Node pNode is the head of a 
** subtree iHeight high (leaf nodes have iHeight==0).
*/
static int rtreeInsertCell(
  Rtree *pRtree,
  RtreeNode *pNode,
  RtreeCell *pCell,
  int iHeight
){
  int rc = SQLITE_OK;
  if( iHeight>0 ){
    RtreeNode *pChild = nodeHashLookup(pRtree, pCell->iRowid);
    if( pChild ){
      nodeRelease(pRtree, pChild->pParent);
      nodeReference(pNode);
      pChild->pParent = pNode;
    }
  }
  if( nodeInsertCell(pRtree, pNode, pCell) ){
    if( iHeight<=pRtree->iReinsertHeight || pNode->iNode==1){
      rc = SplitNode(pRtree, pNode, pCell, iHeight);
    }else{
      pRtree->iReinsertHeight = iHeight;
      rc = Reinsert(pRtree, pNode, pCell, iHeight);
    }
  }else{
    rc = AdjustTree(pRtree, pNode, pCell);
    if( rc==SQLITE_OK ){
      if( iHeight==0 ){
        rc = rowidWrite(pRtree, pCell->iRowid, pNode->iNode);
      }else{
        rc = parentWrite(pRtree, pCell->iRowid, pNode->iNode);
      }
    }
  }
  return rc;
}

static int reinsertNodeContent(Rtree *pRtree, RtreeNode *pNode){
  int ii;
  int rc = SQLITE_OK;
  int nCell = NCELL(pNode);

  for(ii=0; rc==SQLITE_OK && ii<nCell; ii++){
    RtreeNode *pInsert;
    RtreeCell cell;
    nodeGetCell(pRtree, pNode, ii, &cell);

    /* Find a node to store this cell in. pNode->iNode currently contains
    ** the height of the sub-tree headed by the cell.
    */
    rc = ChooseLeaf(pRtree, &cell, (int)pNode->iNode, &pInsert);
    if( rc==SQLITE_OK ){
      int rc2;
      rc = rtreeInsertCell(pRtree, pInsert, &cell, (int)pNode->iNode);
      rc2 = nodeRelease(pRtree, pInsert);
      if( rc==SQLITE_OK ){
        rc = rc2;
      }
    }
  }
  return rc;
}

/*
** Select a currently unused rowid for a new r-tree record.
*/
static int newRowid(Rtree *pRtree, i64 *piRowid){
  int rc;
  sqlite3_bind_null(pRtree->pWriteRowid, 1);
  sqlite3_bind_null(pRtree->pWriteRowid, 2);
  sqlite3_step(pRtree->pWriteRowid);
  rc = sqlite3_reset(pRtree->pWriteRowid);
  *piRowid = sqlite3_last_insert_rowid(pRtree->db);
  return rc;
}

/*
** Remove the entry with rowid=iDelete from the r-tree structure.
*/
static int rtreeDeleteRowid(Rtree *pRtree, sqlite3_int64 iDelete){
  int rc;                         /* Return code */
  RtreeNode *pLeaf = 0;           /* Leaf node containing record iDelete */
  int iCell;                      /* Index of iDelete cell in pLeaf */
  RtreeNode *pRoot;               /* Root node of rtree structure */


  /* Obtain a reference to the root node to initialize Rtree.iDepth */
  rc = nodeAcquire(pRtree, 1, 0, &pRoot);

  /* Obtain a reference to the leaf node that contains the entry 
  ** about to be deleted. 
  */
  if( rc==SQLITE_OK ){
    rc = findLeafNode(pRtree, iDelete, &pLeaf, 0);
  }

  /* Delete the cell in question from the leaf node. */
  if( rc==SQLITE_OK ){
    int rc2;
    rc = nodeRowidIndex(pRtree, pLeaf, iDelete, &iCell);
    if( rc==SQLITE_OK ){
      rc = deleteCell(pRtree, pLeaf, iCell, 0);
    }
    rc2 = nodeRelease(pRtree, pLeaf);
    if( rc==SQLITE_OK ){
      rc = rc2;
    }
  }

  /* Delete the corresponding entry in the <rtree>_rowid table. */
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pRtree->pDeleteRowid, 1, iDelete);
    sqlite3_step(pRtree->pDeleteRowid);
    rc = sqlite3_reset(pRtree->pDeleteRowid);
  }

  /* Check if the root node now has exactly one child. If so, remove
  ** it, schedule the contents of the child for reinsertion and 
  ** reduce the tree height by one.
  **
  ** This is equivalent to copying the contents of the child into
  ** the root node (the operation that Gutman's paper says to perform 
  ** in this scenario).
  */
  if( rc==SQLITE_OK && pRtree->iDepth>0 && NCELL(pRoot)==1 ){
    int rc2;
    RtreeNode *pChild;
    i64 iChild = nodeGetRowid(pRtree, pRoot, 0);
    rc = nodeAcquire(pRtree, iChild, pRoot, &pChild);
    if( rc==SQLITE_OK ){
      rc = removeNode(pRtree, pChild, pRtree->iDepth-1);
    }
    rc2 = nodeRelease(pRtree, pChild);
    if( rc==SQLITE_OK ) rc = rc2;
    if( rc==SQLITE_OK ){
      pRtree->iDepth--;
      writeInt16(pRoot->zData, pRtree->iDepth);
      pRoot->isDirty = 1;
    }
  }

  /* Re-insert the contents of any underfull nodes removed from the tree. */
  for(pLeaf=pRtree->pDeleted; pLeaf; pLeaf=pRtree->pDeleted){
    if( rc==SQLITE_OK ){
      rc = reinsertNodeContent(pRtree, pLeaf);
    }
    pRtree->pDeleted = pLeaf->pNext;
    sqlite3_free(pLeaf);
  }

  /* Release the reference to the root node. */
  if( rc==SQLITE_OK ){
    rc = nodeRelease(pRtree, pRoot);
  }else{
    nodeRelease(pRtree, pRoot);
  }

  return rc;
}

/*
** Rounding constants for float->double conversion.
*/
#define RNDTOWARDS  (1.0 - 1.0/8388608.0)  /* Round towards zero */
#define RNDAWAY     (1.0 + 1.0/8388608.0)  /* Round away from zero */

#if !defined(SQLITE_RTREE_INT_ONLY)
/*
** Convert an sqlite3_value into an RtreeValue (presumably a float)
** while taking care to round toward negative or positive, respectively.
*/
static RtreeValue rtreeValueDown(sqlite3_value *v){
  double d = sqlite3_value_double(v);
  float f = (float)d;
  if( f>d ){
    f = (float)(d*(d<0 ? RNDAWAY : RNDTOWARDS));
  }
  return f;
}
static RtreeValue rtreeValueUp(sqlite3_value *v){
  double d = sqlite3_value_double(v);
  float f = (float)d;
  if( f<d ){
    f = (float)(d*(d<0 ? RNDTOWARDS : RNDAWAY));
  }
  return f;
}
#endif /* !defined(SQLITE_RTREE_INT_ONLY) */


/*
** The xUpdate method for rtree module virtual tables.
*/
static int rtreeUpdate(
  sqlite3_vtab *pVtab, 
  int nData, 
  sqlite3_value **azData, 
  sqlite_int64 *pRowid
){
  Rtree *pRtree = (Rtree *)pVtab;
  int rc = SQLITE_OK;
  RtreeCell cell;                 /* New cell to insert if nData>1 */
  int bHaveRowid = 0;             /* Set to 1 after new rowid is determined */

  rtreeReference(pRtree);
  assert(nData>=1);

  /* Constraint handling. A write operation on an r-tree table may return
  ** SQLITE_CONSTRAINT for two reasons:
  **
  **   1. A duplicate rowid value, or
  **   2. The supplied data violates the "x2>=x1" constraint.
  **
  ** In the first case, if the conflict-handling mode is REPLACE, then
  ** the conflicting row can be removed before proceeding. In the second
  ** case, SQLITE_CONSTRAINT must be returned regardless of the
  ** conflict-handling mode specified by the user.
  */
  if( nData>1 ){
    int ii;

    /* Populate the cell.aCoord[] array. The first coordinate is azData[3]. */
    assert( nData==(pRtree->nDim*2 + 3) );
#ifndef SQLITE_RTREE_INT_ONLY
    if( pRtree->eCoordType==RTREE_COORD_REAL32 ){
      for(ii=0; ii<(pRtree->nDim*2); ii+=2){
        cell.aCoord[ii].f = rtreeValueDown(azData[ii+3]);
        cell.aCoord[ii+1].f = rtreeValueUp(azData[ii+4]);
        if( cell.aCoord[ii].f>cell.aCoord[ii+1].f ){
          rc = SQLITE_CONSTRAINT;
          goto constraint;
        }
      }
    }else
#endif
    {
      for(ii=0; ii<(pRtree->nDim*2); ii+=2){
        cell.aCoord[ii].i = sqlite3_value_int(azData[ii+3]);
        cell.aCoord[ii+1].i = sqlite3_value_int(azData[ii+4]);
        if( cell.aCoord[ii].i>cell.aCoord[ii+1].i ){
          rc = SQLITE_CONSTRAINT;
          goto constraint;
        }
      }
    }

    /* If a rowid value was supplied, check if it is already present in 
    ** the table. If so, the constraint has failed. */
    if( sqlite3_value_type(azData[2])!=SQLITE_NULL ){
      cell.iRowid = sqlite3_value_int64(azData[2]);
      if( sqlite3_value_type(azData[0])==SQLITE_NULL
       || sqlite3_value_int64(azData[0])!=cell.iRowid
      ){
        int steprc;
        sqlite3_bind_int64(pRtree->pReadRowid, 1, cell.iRowid);
        steprc = sqlite3_step(pRtree->pReadRowid);
        rc = sqlite3_reset(pRtree->pReadRowid);
        if( SQLITE_ROW==steprc ){
          if( sqlite3_vtab_on_conflict(pRtree->db)==SQLITE_REPLACE ){
            rc = rtreeDeleteRowid(pRtree, cell.iRowid);
          }else{
            rc = SQLITE_CONSTRAINT;
            goto constraint;
          }
        }
      }
      bHaveRowid = 1;
    }
  }

  /* If azData[0] is not an SQL NULL value, it is the rowid of a
  ** record to delete from the r-tree table. The following block does
  ** just that.
  */
  if( sqlite3_value_type(azData[0])!=SQLITE_NULL ){
    rc = rtreeDeleteRowid(pRtree, sqlite3_value_int64(azData[0]));
  }

  /* If the azData[] array contains more than one element, elements
  ** (azData[2]..azData[argc-1]) contain a new record to insert into
  ** the r-tree structure.
  */
  if( rc==SQLITE_OK && nData>1 ){
    /* Insert the new record into the r-tree */
    RtreeNode *pLeaf = 0;

    /* Figure out the rowid of the new row. */
    if( bHaveRowid==0 ){
      rc = newRowid(pRtree, &cell.iRowid);
    }
    *pRowid = cell.iRowid;

    if( rc==SQLITE_OK ){
      rc = ChooseLeaf(pRtree, &cell, 0, &pLeaf);
    }
    if( rc==SQLITE_OK ){
      int rc2;
      pRtree->iReinsertHeight = -1;
      rc = rtreeInsertCell(pRtree, pLeaf, &cell, 0);
      rc2 = nodeRelease(pRtree, pLeaf);
      if( rc==SQLITE_OK ){
        rc = rc2;
      }
    }
  }

constraint:
  rtreeRelease(pRtree);
  return rc;
}

/*
** The xRename method for rtree module virtual tables.
*/
static int rtreeRename(sqlite3_vtab *pVtab, const char *zNewName){
  Rtree *pRtree = (Rtree *)pVtab;
  int rc = SQLITE_NOMEM;
  char *zSql = sqlite3_mprintf(
    "ALTER TABLE %Q.'%q_node'   RENAME TO \"%w_node\";"
    "ALTER TABLE %Q.'%q_parent' RENAME TO \"%w_parent\";"
    "ALTER TABLE %Q.'%q_rowid'  RENAME TO \"%w_rowid\";"
    , pRtree->zDb, pRtree->zName, zNewName 
    , pRtree->zDb, pRtree->zName, zNewName 
    , pRtree->zDb, pRtree->zName, zNewName
  );
  if( zSql ){
    rc = sqlite3_exec(pRtree->db, zSql, 0, 0, 0);
    sqlite3_free(zSql);
  }
  return rc;
}

/*
** This function populates the pRtree->nRowEst variable with an estimate
** of the number of rows in the virtual table. If possible, this is based
** on sqlite_stat1 data. Otherwise, use RTREE_DEFAULT_ROWEST.
*/
static int rtreeQueryStat1(sqlite3 *db, Rtree *pRtree){
  const char *zFmt = "SELECT stat FROM %Q.sqlite_stat1 WHERE tbl = '%q_rowid'";
  char *zSql;
  sqlite3_stmt *p;
  int rc;
  i64 nRow = 0;

  zSql = sqlite3_mprintf(zFmt, pRtree->zDb, pRtree->zName);
  if( zSql==0 ){
    rc = SQLITE_NOMEM;
  }else{
    rc = sqlite3_prepare_v2(db, zSql, -1, &p, 0);
    if( rc==SQLITE_OK ){
      if( sqlite3_step(p)==SQLITE_ROW ) nRow = sqlite3_column_int64(p, 0);
      rc = sqlite3_finalize(p);
    }else if( rc!=SQLITE_NOMEM ){
      rc = SQLITE_OK;
    }

    if( rc==SQLITE_OK ){
      if( nRow==0 ){
        pRtree->nRowEst = RTREE_DEFAULT_ROWEST;
      }else{
        pRtree->nRowEst = MAX(nRow, RTREE_MIN_ROWEST);
      }
    }
    sqlite3_free(zSql);
  }

  return rc;
}

static sqlite3_module rtreeModule = {
  0,                          /* iVersion */
  rtreeCreate,                /* xCreate - create a table */
  rtreeConnect,               /* xConnect - connect to an existing table */
  rtreeBestIndex,             /* xBestIndex - Determine search strategy */
  rtreeDisconnect,            /* xDisconnect - Disconnect from a table */
  rtreeDestroy,               /* xDestroy - Drop a table */
  rtreeOpen,                  /* xOpen - open a cursor */
  rtreeClose,                 /* xClose - close a cursor */
  rtreeFilter,                /* xFilter - configure scan constraints */
  rtreeNext,                  /* xNext - advance a cursor */
  rtreeEof,                   /* xEof */
  rtreeColumn,                /* xColumn - read data */
  rtreeRowid,                 /* xRowid - read data */
  rtreeUpdate,                /* xUpdate - write data */
  0,                          /* xBegin - begin transaction */
  0,                          /* xSync - sync transaction */
  0,                          /* xCommit - commit transaction */
  0,                          /* xRollback - rollback transaction */
  0,                          /* xFindFunction - function overloading */
  rtreeRename,                /* xRename - rename the table */
  0,                          /* xSavepoint */
  0,                          /* xRelease */
  0                           /* xRollbackTo */
};

static int rtreeSqlInit(
  Rtree *pRtree, 
  sqlite3 *db, 
  const char *zDb, 
  const char *zPrefix, 
  int isCreate
){
  int rc = SQLITE_OK;

  #define N_STATEMENT 9
  static const char *azSql[N_STATEMENT] = {
    /* Read and write the xxx_node table */
    "SELECT data FROM '%q'.'%q_node' WHERE nodeno = :1",
    "INSERT OR REPLACE INTO '%q'.'%q_node' VALUES(:1, :2)",
    "DELETE FROM '%q'.'%q_node' WHERE nodeno = :1",

    /* Read and write the xxx_rowid table */
    "SELECT nodeno FROM '%q'.'%q_rowid' WHERE rowid = :1",
    "INSERT OR REPLACE INTO '%q'.'%q_rowid' VALUES(:1, :2)",
    "DELETE FROM '%q'.'%q_rowid' WHERE rowid = :1",

    /* Read and write the xxx_parent table */
    "SELECT parentnode FROM '%q'.'%q_parent' WHERE nodeno = :1",
    "INSERT OR REPLACE INTO '%q'.'%q_parent' VALUES(:1, :2)",
    "DELETE FROM '%q'.'%q_parent' WHERE nodeno = :1"
  };
  sqlite3_stmt **appStmt[N_STATEMENT];
  int i;

  pRtree->db = db;

  if( isCreate ){
    char *zCreate = sqlite3_mprintf(
"CREATE TABLE \"%w\".\"%w_node\"(nodeno INTEGER PRIMARY KEY, data BLOB);"
"CREATE TABLE \"%w\".\"%w_rowid\"(rowid INTEGER PRIMARY KEY, nodeno INTEGER);"
"CREATE TABLE \"%w\".\"%w_parent\"(nodeno INTEGER PRIMARY KEY,"
                                  " parentnode INTEGER);"
"INSERT INTO '%q'.'%q_node' VALUES(1, zeroblob(%d))",
      zDb, zPrefix, zDb, zPrefix, zDb, zPrefix, zDb, zPrefix, pRtree->iNodeSize
    );
    if( !zCreate ){
      return SQLITE_NOMEM;
    }
    rc = sqlite3_exec(db, zCreate, 0, 0, 0);
    sqlite3_free(zCreate);
    if( rc!=SQLITE_OK ){
      return rc;
    }
  }

  appStmt[0] = &pRtree->pReadNode;
  appStmt[1] = &pRtree->pWriteNode;
  appStmt[2] = &pRtree->pDeleteNode;
  appStmt[3] = &pRtree->pReadRowid;
  appStmt[4] = &pRtree->pWriteRowid;
  appStmt[5] = &pRtree->pDeleteRowid;
  appStmt[6] = &pRtree->pReadParent;
  appStmt[7] = &pRtree->pWriteParent;
  appStmt[8] = &pRtree->pDeleteParent;

  rc = rtreeQueryStat1(db, pRtree);
  for(i=0; i<N_STATEMENT && rc==SQLITE_OK; i++){
    char *zSql = sqlite3_mprintf(azSql[i], zDb, zPrefix);
    if( zSql ){
      rc = sqlite3_prepare_v2(db, zSql, -1, appStmt[i], 0); 
    }else{
      rc = SQLITE_NOMEM;
    }
    sqlite3_free(zSql);
  }

  return rc;
}

/*
** The second argument to this function contains the text of an SQL statement
** that returns a single integer value. The statement is compiled and executed
** using database connection db. If successful, the integer value returned
** is written to *piVal and SQLITE_OK returned. Otherwise, an SQLite error
** code is returned and the value of *piVal after returning is not defined.
*/
static int getIntFromStmt(sqlite3 *db, const char *zSql, int *piVal){
  int rc = SQLITE_NOMEM;
  if( zSql ){
    sqlite3_stmt *pStmt = 0;
    rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
    if( rc==SQLITE_OK ){
      if( SQLITE_ROW==sqlite3_step(pStmt) ){
        *piVal = sqlite3_column_int(pStmt, 0);
      }
      rc = sqlite3_finalize(pStmt);
    }
  }
  return rc;
}

/*
** This function is called from within the xConnect() or xCreate() method to
** determine the node-size used by the rtree table being created or connected
** to. If successful, pRtree->iNodeSize is populated and SQLITE_OK returned.
** Otherwise, an SQLite error code is returned.
**
** If this function is being called as part of an xConnect(), then the rtree
** table already exists. In this case the node-size is determined by inspecting
** the root node of the tree.
**
** Otherwise, for an xCreate(), use 64 bytes less than the database page-size. 
** This ensures that each node is stored on a single database page. If the 
** database page-size is so large that more than RTREE_MAXCELLS entries 
** would fit in a single node, use a smaller node-size.
*/
static int getNodeSize(
  sqlite3 *db,                    /* Database handle */
  Rtree *pRtree,                  /* Rtree handle */
  int isCreate,                   /* True for xCreate, false for xConnect */
  char **pzErr                    /* OUT: Error message, if any */
){
  int rc;
  char *zSql;
  if( isCreate ){
    int iPageSize = 0;
    zSql = sqlite3_mprintf("PRAGMA %Q.page_size", pRtree->zDb);
    rc = getIntFromStmt(db, zSql, &iPageSize);
    if( rc==SQLITE_OK ){
      pRtree->iNodeSize = iPageSize-64;
      if( (4+pRtree->nBytesPerCell*RTREE_MAXCELLS)<pRtree->iNodeSize ){
        pRtree->iNodeSize = 4+pRtree->nBytesPerCell*RTREE_MAXCELLS;
      }
    }else{
      *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    }
  }else{
    zSql = sqlite3_mprintf(
        "SELECT length(data) FROM '%q'.'%q_node' WHERE nodeno = 1",
        pRtree->zDb, pRtree->zName
    );
    rc = getIntFromStmt(db, zSql, &pRtree->iNodeSize);
    if( rc!=SQLITE_OK ){
      *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    }
  }

  sqlite3_free(zSql);
  return rc;
}

/* 
** This function is the implementation of both the xConnect and xCreate
** methods of the r-tree virtual table.
**
**   argv[0]   -> module name
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> column names...
*/
static int rtreeInit(
  sqlite3 *db,                        /* Database connection */
  void *pAux,                         /* One of the RTREE_COORD_* constants */
  int argc, const char *const*argv,   /* Parameters to CREATE TABLE statement */
  sqlite3_vtab **ppVtab,              /* OUT: New virtual table */
  char **pzErr,                       /* OUT: Error message, if any */
  int isCreate                        /* True for xCreate, false for xConnect */
){
  int rc = SQLITE_OK;
  Rtree *pRtree;
  int nDb;              /* Length of string argv[1] */
  int nName;            /* Length of string argv[2] */
  int eCoordType = (pAux ? RTREE_COORD_INT32 : RTREE_COORD_REAL32);

  const char *aErrMsg[] = {
    0,                                                    /* 0 */
    "Wrong number of columns for an rtree table",         /* 1 */
    "Too few columns for an rtree table",                 /* 2 */
    "Too many columns for an rtree table"                 /* 3 */
  };

  int iErr = (argc<6) ? 2 : argc>(RTREE_MAX_DIMENSIONS*2+4) ? 3 : argc%2;
  if( aErrMsg[iErr] ){
    *pzErr = sqlite3_mprintf("%s", aErrMsg[iErr]);
    return SQLITE_ERROR;
  }

  sqlite3_vtab_config(db, SQLITE_VTAB_CONSTRAINT_SUPPORT, 1);

  /* Allocate the sqlite3_vtab structure */
  nDb = (int)strlen(argv[1]);
  nName = (int)strlen(argv[2]);
  pRtree = (Rtree *)sqlite3_malloc(sizeof(Rtree)+nDb+nName+2);
  if( !pRtree ){
    return SQLITE_NOMEM;
  }
  memset(pRtree, 0, sizeof(Rtree)+nDb+nName+2);
  pRtree->nBusy = 1;
  pRtree->base.pModule = &rtreeModule;
  pRtree->zDb = (char *)&pRtree[1];
  pRtree->zName = &pRtree->zDb[nDb+1];
  pRtree->nDim = (argc-4)/2;
  pRtree->nBytesPerCell = 8 + pRtree->nDim*4*2;
  pRtree->eCoordType = eCoordType;
  memcpy(pRtree->zDb, argv[1], nDb);
  memcpy(pRtree->zName, argv[2], nName);

  /* Figure out the node size to use. */
  rc = getNodeSize(db, pRtree, isCreate, pzErr);

  /* Create/Connect to the underlying relational database schema. If
  ** that is successful, call sqlite3_declare_vtab() to configure
  ** the r-tree table schema.
  */
  if( rc==SQLITE_OK ){
    if( (rc = rtreeSqlInit(pRtree, db, argv[1], argv[2], isCreate)) ){
      *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    }else{
      char *zSql = sqlite3_mprintf("CREATE TABLE x(%s", argv[3]);
      char *zTmp;
      int ii;
      for(ii=4; zSql && ii<argc; ii++){
        zTmp = zSql;
        zSql = sqlite3_mprintf("%s, %s", zTmp, argv[ii]);
        sqlite3_free(zTmp);
      }
      if( zSql ){
        zTmp = zSql;
        zSql = sqlite3_mprintf("%s);", zTmp);
        sqlite3_free(zTmp);
      }
      if( !zSql ){
        rc = SQLITE_NOMEM;
      }else if( SQLITE_OK!=(rc = sqlite3_declare_vtab(db, zSql)) ){
        *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
      }
      sqlite3_free(zSql);
    }
  }

  if( rc==SQLITE_OK ){
    *ppVtab = (sqlite3_vtab *)pRtree;
  }else{
    assert( *ppVtab==0 );
    assert( pRtree->nBusy==1 );
    rtreeRelease(pRtree);
  }
  return rc;
}


/*
** Implementation of a scalar function that decodes r-tree nodes to
** human readable strings. This can be used for debugging and analysis.
**
** The scalar function takes two arguments: (1) the number of dimensions
** to the rtree (between 1 and 5, inclusive) and (2) a blob of data containing
** an r-tree node.  For a two-dimensional r-tree structure called "rt", to
** deserialize all nodes, a statement like:
**
**   SELECT rtreenode(2, data) FROM rt_node;
**
** The human readable string takes the form of a Tcl list with one
** entry for each cell in the r-tree node. Each entry is itself a
** list, containing the 8-byte rowid/pageno followed by the 
** <num-dimension>*2 coordinates.
*/
static void rtreenode(sqlite3_context *ctx, int nArg, sqlite3_value **apArg){
  char *zText = 0;
  RtreeNode node;
  Rtree tree;
  int ii;

  UNUSED_PARAMETER(nArg);
  memset(&node, 0, sizeof(RtreeNode));
  memset(&tree, 0, sizeof(Rtree));
  tree.nDim = sqlite3_value_int(apArg[0]);
  tree.nBytesPerCell = 8 + 8 * tree.nDim;
  node.zData = (u8 *)sqlite3_value_blob(apArg[1]);

  for(ii=0; ii<NCELL(&node); ii++){
    char zCell[512];
    int nCell = 0;
    RtreeCell cell;
    int jj;

    nodeGetCell(&tree, &node, ii, &cell);
    sqlite3_snprintf(512-nCell,&zCell[nCell],"%lld", cell.iRowid);
    nCell = (int)strlen(zCell);
    for(jj=0; jj<tree.nDim*2; jj++){
#ifndef SQLITE_RTREE_INT_ONLY
      sqlite3_snprintf(512-nCell,&zCell[nCell], " %g",
                       (double)cell.aCoord[jj].f);
#else
      sqlite3_snprintf(512-nCell,&zCell[nCell], " %d",
                       cell.aCoord[jj].i);
#endif
      nCell = (int)strlen(zCell);
    }

    if( zText ){
      char *zTextNew = sqlite3_mprintf("%s {%s}", zText, zCell);
      sqlite3_free(zText);
      zText = zTextNew;
    }else{
      zText = sqlite3_mprintf("{%s}", zCell);
    }
  }
  
  sqlite3_result_text(ctx, zText, -1, sqlite3_free);
}

/* This routine implements an SQL function that returns the "depth" parameter
** from the front of a blob that is an r-tree node.  For example:
**
**     SELECT rtreedepth(data) FROM rt_node WHERE nodeno=1;
**
** The depth value is 0 for all nodes other than the root node, and the root
** node always has nodeno=1, so the example above is the primary use for this
** routine.  This routine is intended for testing and analysis only.
*/
static void rtreedepth(sqlite3_context *ctx, int nArg, sqlite3_value **apArg){
  UNUSED_PARAMETER(nArg);
  if( sqlite3_value_type(apArg[0])!=SQLITE_BLOB 
   || sqlite3_value_bytes(apArg[0])<2
  ){
    sqlite3_result_error(ctx, "Invalid argument to rtreedepth()", -1); 
  }else{
    u8 *zBlob = (u8 *)sqlite3_value_blob(apArg[0]);
    sqlite3_result_int(ctx, readInt16(zBlob));
  }
}

/*
** Register the r-tree module with database handle db. This creates the
** virtual table module "rtree" and the debugging/analysis scalar 
** function "rtreenode".
*/
SQLITE_PRIVATE int sqlite3RtreeInit(sqlite3 *db){
  const int utf8 = SQLITE_UTF8;
  int rc;

  rc = sqlite3_create_function(db, "rtreenode", 2, utf8, 0, rtreenode, 0, 0);
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "rtreedepth", 1, utf8, 0,rtreedepth, 0, 0);
  }
  if( rc==SQLITE_OK ){
#ifdef SQLITE_RTREE_INT_ONLY
    void *c = (void *)RTREE_COORD_INT32;
#else
    void *c = (void *)RTREE_COORD_REAL32;
#endif
    rc = sqlite3_create_module_v2(db, "rtree", &rtreeModule, c, 0);
  }
  if( rc==SQLITE_OK ){
    void *c = (void *)RTREE_COORD_INT32;
    rc = sqlite3_create_module_v2(db, "rtree_i32", &rtreeModule, c, 0);
  }

  return rc;
}

/*
** This routine deletes the RtreeGeomCallback object that was attached
** one of the SQL functions create by sqlite3_rtree_geometry_callback()
** or sqlite3_rtree_query_callback().  In other words, this routine is the
** destructor for an RtreeGeomCallback objecct.  This routine is called when
** the corresponding SQL function is deleted.
*/
static void rtreeFreeCallback(void *p){
  RtreeGeomCallback *pInfo = (RtreeGeomCallback*)p;
  if( pInfo->xDestructor ) pInfo->xDestructor(pInfo->pContext);
  sqlite3_free(p);
}

/*
** Each call to sqlite3_rtree_geometry_callback() or
** sqlite3_rtree_query_callback() creates an ordinary SQLite
** scalar function that is implemented by this routine.
**
** All this function does is construct an RtreeMatchArg object that
** contains the geometry-checking callback routines and a list of
** parameters to this function, then return that RtreeMatchArg object
** as a BLOB.
**
** The R-Tree MATCH operator will read the returned BLOB, deserialize
** the RtreeMatchArg object, and use the RtreeMatchArg object to figure
** out which elements of the R-Tree should be returned by the query.
*/
static void geomCallback(sqlite3_context *ctx, int nArg, sqlite3_value **aArg){
  RtreeGeomCallback *pGeomCtx = (RtreeGeomCallback *)sqlite3_user_data(ctx);
  RtreeMatchArg *pBlob;
  int nBlob;

  nBlob = sizeof(RtreeMatchArg) + (nArg-1)*sizeof(RtreeDValue);
  pBlob = (RtreeMatchArg *)sqlite3_malloc(nBlob);
  if( !pBlob ){
    sqlite3_result_error_nomem(ctx);
  }else{
    int i;
    pBlob->magic = RTREE_GEOMETRY_MAGIC;
    pBlob->cb = pGeomCtx[0];
    pBlob->nParam = nArg;
    for(i=0; i<nArg; i++){
#ifdef SQLITE_RTREE_INT_ONLY
      pBlob->aParam[i] = sqlite3_value_int64(aArg[i]);
#else
      pBlob->aParam[i] = sqlite3_value_double(aArg[i]);
#endif
    }
    sqlite3_result_blob(ctx, pBlob, nBlob, sqlite3_free);
  }
}

/*
** Register a new geometry function for use with the r-tree MATCH operator.
*/
SQLITE_API int sqlite3_rtree_geometry_callback(
  sqlite3 *db,                  /* Register SQL function on this connection */
  const char *zGeom,            /* Name of the new SQL function */
  int (*xGeom)(sqlite3_rtree_geometry*,int,RtreeDValue*,int*), /* Callback */
  void *pContext                /* Extra data associated with the callback */
){
  RtreeGeomCallback *pGeomCtx;      /* Context object for new user-function */

  /* Allocate and populate the context object. */
  pGeomCtx = (RtreeGeomCallback *)sqlite3_malloc(sizeof(RtreeGeomCallback));
  if( !pGeomCtx ) return SQLITE_NOMEM;
  pGeomCtx->xGeom = xGeom;
  pGeomCtx->xQueryFunc = 0;
  pGeomCtx->xDestructor = 0;
  pGeomCtx->pContext = pContext;
  return sqlite3_create_function_v2(db, zGeom, -1, SQLITE_ANY, 
      (void *)pGeomCtx, geomCallback, 0, 0, rtreeFreeCallback
  );
}

/*
** Register a new 2nd-generation geometry function for use with the
** r-tree MATCH operator.
*/
SQLITE_API int sqlite3_rtree_query_callback(
  sqlite3 *db,                 /* Register SQL function on this connection */
  const char *zQueryFunc,      /* Name of new SQL function */
  int (*xQueryFunc)(sqlite3_rtree_query_info*), /* Callback */
  void *pContext,              /* Extra data passed into the callback */
  void (*xDestructor)(void*)   /* Destructor for the extra data */
){
  RtreeGeomCallback *pGeomCtx;      /* Context object for new user-function */

  /* Allocate and populate the context object. */
  pGeomCtx = (RtreeGeomCallback *)sqlite3_malloc(sizeof(RtreeGeomCallback));
  if( !pGeomCtx ) return SQLITE_NOMEM;
  pGeomCtx->xGeom = 0;
  pGeomCtx->xQueryFunc = xQueryFunc;
  pGeomCtx->xDestructor = xDestructor;
  pGeomCtx->pContext = pContext;
  return sqlite3_create_function_v2(db, zQueryFunc, -1, SQLITE_ANY, 
      (void *)pGeomCtx, geomCallback, 0, 0, rtreeFreeCallback
  );
}

#if !SQLITE_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
SQLITE_API int sqlite3_rtree_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  SQLITE_EXTENSION_INIT2(pApi)
  return sqlite3RtreeInit(db);
}
#endif

#endif

/************** End of rtree.c ***********************************************/
/************** Begin file icu.c *********************************************/
/*
** 2007 May 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** $Id: icu.c,v 1.7 2007/12/13 21:54:11 drh Exp $
**
** This file implements an integration between the ICU library 
** ("International Components for Unicode", an open-source library 
** for handling unicode data) and SQLite. The integration uses 
** ICU to provide the following to SQLite:
**
**   * An implementation of the SQL regexp() function (and hence REGEXP
**     operator) using the ICU uregex_XX() APIs.
**
**   * Implementations of the SQL scalar upper() and lower() functions
**     for case mapping.
**
**   * Integration of ICU and SQLite collation sequences.
**
**   * An implementation of the LIKE operator that uses ICU to 
**     provide case-independent matching.
*/

#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_ICU)

/* Include ICU headers */
#include <unicode/utypes.h>
#include <unicode/uregex.h>
#include <unicode/ustring.h>
#include <unicode/ucol.h>

/* #include <assert.h> */

#ifndef SQLITE_CORE
  SQLITE_EXTENSION_INIT1
#else
#endif

/*
** Maximum length (in bytes) of the pattern in a LIKE or GLOB
** operator.
*/
#ifndef SQLITE_MAX_LIKE_PATTERN_LENGTH
# define SQLITE_MAX_LIKE_PATTERN_LENGTH 50000
#endif

/*
** Version of sqlite3_free() that is always a function, never a macro.
*/
static void xFree(void *p){
  sqlite3_free(p);
}

/*
** Compare two UTF-8 strings for equality where the first string is
** a "LIKE" expression. Return true (1) if they are the same and 
** false (0) if they are different.
*/
static int icuLikeCompare(
  const uint8_t *zPattern,   /* LIKE pattern */
  const uint8_t *zString,    /* The UTF-8 string to compare against */
  const UChar32 uEsc         /* The escape character */
){
  static const int MATCH_ONE = (UChar32)'_';
  static const int MATCH_ALL = (UChar32)'%';

  int iPattern = 0;       /* Current byte index in zPattern */
  int iString = 0;        /* Current byte index in zString */

  int prevEscape = 0;     /* True if the previous character was uEsc */

  while( zPattern[iPattern]!=0 ){

    /* Read (and consume) the next character from the input pattern. */
    UChar32 uPattern;
    U8_NEXT_UNSAFE(zPattern, iPattern, uPattern);
    assert(uPattern!=0);

    /* There are now 4 possibilities:
    **
    **     1. uPattern is an unescaped match-all character "%",
    **     2. uPattern is an unescaped match-one character "_",
    **     3. uPattern is an unescaped escape character, or
    **     4. uPattern is to be handled as an ordinary character
    */
    if( !prevEscape && uPattern==MATCH_ALL ){
      /* Case 1. */
      uint8_t c;

      /* Skip any MATCH_ALL or MATCH_ONE characters that follow a
      ** MATCH_ALL. For each MATCH_ONE, skip one character in the 
      ** test string.
      */
      while( (c=zPattern[iPattern]) == MATCH_ALL || c == MATCH_ONE ){
        if( c==MATCH_ONE ){
          if( zString[iString]==0 ) return 0;
          U8_FWD_1_UNSAFE(zString, iString);
        }
        iPattern++;
      }

      if( zPattern[iPattern]==0 ) return 1;

      while( zString[iString] ){
        if( icuLikeCompare(&zPattern[iPattern], &zString[iString], uEsc) ){
          return 1;
        }
        U8_FWD_1_UNSAFE(zString, iString);
      }
      return 0;

    }else if( !prevEscape && uPattern==MATCH_ONE ){
      /* Case 2. */
      if( zString[iString]==0 ) return 0;
      U8_FWD_1_UNSAFE(zString, iString);

    }else if( !prevEscape && uPattern==uEsc){
      /* Case 3. */
      prevEscape = 1;

    }else{
      /* Case 4. */
      UChar32 uString;
      U8_NEXT_UNSAFE(zString, iString, uString);
      uString = u_foldCase(uString, U_FOLD_CASE_DEFAULT);
      uPattern = u_foldCase(uPattern, U_FOLD_CASE_DEFAULT);
      if( uString!=uPattern ){
        return 0;
      }
      prevEscape = 0;
    }
  }

  return zString[iString]==0;
}

/*
** Implementation of the like() SQL function.  This function implements
** the build-in LIKE operator.  The first argument to the function is the
** pattern and the second argument is the string.  So, the SQL statements:
**
**       A LIKE B
**
** is implemented as like(B, A). If there is an escape character E, 
**
**       A LIKE B ESCAPE E
**
** is mapped to like(B, A, E).
*/
static void icuLikeFunc(
  sqlite3_context *context, 
  int argc, 
  sqlite3_value **argv
){
  const unsigned char *zA = sqlite3_value_text(argv[0]);
  const unsigned char *zB = sqlite3_value_text(argv[1]);
  UChar32 uEsc = 0;

  /* Limit the length of the LIKE or GLOB pattern to avoid problems
  ** of deep recursion and N*N behavior in patternCompare().
  */
  if( sqlite3_value_bytes(argv[0])>SQLITE_MAX_LIKE_PATTERN_LENGTH ){
    sqlite3_result_error(context, "LIKE or GLOB pattern too complex", -1);
    return;
  }


  if( argc==3 ){
    /* The escape character string must consist of a single UTF-8 character.
    ** Otherwise, return an error.
    */
    int nE= sqlite3_value_bytes(argv[2]);
    const unsigned char *zE = sqlite3_value_text(argv[2]);
    int i = 0;
    if( zE==0 ) return;
    U8_NEXT(zE, i, nE, uEsc);
    if( i!=nE){
      sqlite3_result_error(context, 
          "ESCAPE expression must be a single character", -1);
      return;
    }
  }

  if( zA && zB ){
    sqlite3_result_int(context, icuLikeCompare(zA, zB, uEsc));
  }
}

/*
** This function is called when an ICU function called from within
** the implementation of an SQL scalar function returns an error.
**
** The scalar function context passed as the first argument is 
** loaded with an error message based on the following two args.
*/
static void icuFunctionError(
  sqlite3_context *pCtx,       /* SQLite scalar function context */
  const char *zName,           /* Name of ICU function that failed */
  UErrorCode e                 /* Error code returned by ICU function */
){
  char zBuf[128];
  sqlite3_snprintf(128, zBuf, "ICU error: %s(): %s", zName, u_errorName(e));
  zBuf[127] = '\0';
  sqlite3_result_error(pCtx, zBuf, -1);
}

/*
** Function to delete compiled regexp objects. Registered as
** a destructor function with sqlite3_set_auxdata().
*/
static void icuRegexpDelete(void *p){
  URegularExpression *pExpr = (URegularExpression *)p;
  uregex_close(pExpr);
}

/*
** Implementation of SQLite REGEXP operator. This scalar function takes
** two arguments. The first is a regular expression pattern to compile
** the second is a string to match against that pattern. If either 
** argument is an SQL NULL, then NULL Is returned. Otherwise, the result
** is 1 if the string matches the pattern, or 0 otherwise.
**
** SQLite maps the regexp() function to the regexp() operator such
** that the following two are equivalent:
**
**     zString REGEXP zPattern
**     regexp(zPattern, zString)
**
** Uses the following ICU regexp APIs:
**
**     uregex_open()
**     uregex_matches()
**     uregex_close()
*/
static void icuRegexpFunc(sqlite3_context *p, int nArg, sqlite3_value **apArg){
  UErrorCode status = U_ZERO_ERROR;
  URegularExpression *pExpr;
  UBool res;
  const UChar *zString = sqlite3_value_text16(apArg[1]);

  (void)nArg;  /* Unused parameter */

  /* If the left hand side of the regexp operator is NULL, 
  ** then the result is also NULL. 
  */
  if( !zString ){
    return;
  }

  pExpr = sqlite3_get_auxdata(p, 0);
  if( !pExpr ){
    const UChar *zPattern = sqlite3_value_text16(apArg[0]);
    if( !zPattern ){
      return;
    }
    pExpr = uregex_open(zPattern, -1, 0, 0, &status);

    if( U_SUCCESS(status) ){
      sqlite3_set_auxdata(p, 0, pExpr, icuRegexpDelete);
    }else{
      assert(!pExpr);
      icuFunctionError(p, "uregex_open", status);
      return;
    }
  }

  /* Configure the text that the regular expression operates on. */
  uregex_setText(pExpr, zString, -1, &status);
  if( !U_SUCCESS(status) ){
    icuFunctionError(p, "uregex_setText", status);
    return;
  }

  /* Attempt the match */
  res = uregex_matches(pExpr, 0, &status);
  if( !U_SUCCESS(status) ){
    icuFunctionError(p, "uregex_matches", status);
    return;
  }

  /* Set the text that the regular expression operates on to a NULL
  ** pointer. This is not really necessary, but it is tidier than 
  ** leaving the regular expression object configured with an invalid
  ** pointer after this function returns.
  */
  uregex_setText(pExpr, 0, 0, &status);

  /* Return 1 or 0. */
  sqlite3_result_int(p, res ? 1 : 0);
}

/*
** Implementations of scalar functions for case mapping - upper() and 
** lower(). Function upper() converts its input to upper-case (ABC).
** Function lower() converts to lower-case (abc).
**
** ICU provides two types of case mapping, "general" case mapping and
** "language specific". Refer to ICU documentation for the differences
** between the two.
**
** To utilise "general" case mapping, the upper() or lower() scalar 
** functions are invoked with one argument:
**
**     upper('ABC') -> 'abc'
**     lower('abc') -> 'ABC'
**
** To access ICU "language specific" case mapping, upper() or lower()
** should be invoked with two arguments. The second argument is the name
** of the locale to use. Passing an empty string ("") or SQL NULL value
** as the second argument is the same as invoking the 1 argument version
** of upper() or lower().
**
**     lower('I', 'en_us') -> 'i'
**     lower('I', 'tr_tr') -> 'ı' (small dotless i)
**
** http://www.icu-project.org/userguide/posix.html#case_mappings
*/
static void icuCaseFunc16(sqlite3_context *p, int nArg, sqlite3_value **apArg){
  const UChar *zInput;
  UChar *zOutput;
  int nInput;
  int nOutput;

  UErrorCode status = U_ZERO_ERROR;
  const char *zLocale = 0;

  assert(nArg==1 || nArg==2);
  if( nArg==2 ){
    zLocale = (const char *)sqlite3_value_text(apArg[1]);
  }

  zInput = sqlite3_value_text16(apArg[0]);
  if( !zInput ){
    return;
  }
  nInput = sqlite3_value_bytes16(apArg[0]);

  nOutput = nInput * 2 + 2;
  zOutput = sqlite3_malloc(nOutput);
  if( !zOutput ){
    return;
  }

  if( sqlite3_user_data(p) ){
    u_strToUpper(zOutput, nOutput/2, zInput, nInput/2, zLocale, &status);
  }else{
    u_strToLower(zOutput, nOutput/2, zInput, nInput/2, zLocale, &status);
  }

  if( !U_SUCCESS(status) ){
    icuFunctionError(p, "u_strToLower()/u_strToUpper", status);
    return;
  }

  sqlite3_result_text16(p, zOutput, -1, xFree);
}

/*
** Collation sequence destructor function. The pCtx argument points to
** a UCollator structure previously allocated using ucol_open().
*/
static void icuCollationDel(void *pCtx){
  UCollator *p = (UCollator *)pCtx;
  ucol_close(p);
}

/*
** Collation sequence comparison function. The pCtx argument points to
** a UCollator structure previously allocated using ucol_open().
*/
static int icuCollationColl(
  void *pCtx,
  int nLeft,
  const void *zLeft,
  int nRight,
  const void *zRight
){
  UCollationResult res;
  UCollator *p = (UCollator *)pCtx;
  res = ucol_strcoll(p, (UChar *)zLeft, nLeft/2, (UChar *)zRight, nRight/2);
  switch( res ){
    case UCOL_LESS:    return -1;
    case UCOL_GREATER: return +1;
    case UCOL_EQUAL:   return 0;
  }
  assert(!"Unexpected return value from ucol_strcoll()");
  return 0;
}

/*
** Implementation of the scalar function icu_load_collation().
**
** This scalar function is used to add ICU collation based collation 
** types to an SQLite database connection. It is intended to be called
** as follows:
**
**     SELECT icu_load_collation(<locale>, <collation-name>);
**
** Where <locale> is a string containing an ICU locale identifier (i.e.
** "en_AU", "tr_TR" etc.) and <collation-name> is the name of the
** collation sequence to create.
*/
static void icuLoadCollation(
  sqlite3_context *p, 
  int nArg, 
  sqlite3_value **apArg
){
  sqlite3 *db = (sqlite3 *)sqlite3_user_data(p);
  UErrorCode status = U_ZERO_ERROR;
  const char *zLocale;      /* Locale identifier - (eg. "jp_JP") */
  const char *zName;        /* SQL Collation sequence name (eg. "japanese") */
  UCollator *pUCollator;    /* ICU library collation object */
  int rc;                   /* Return code from sqlite3_create_collation_x() */

  assert(nArg==2);
  zLocale = (const char *)sqlite3_value_text(apArg[0]);
  zName = (const char *)sqlite3_value_text(apArg[1]);

  if( !zLocale || !zName ){
    return;
  }

  pUCollator = ucol_open(zLocale, &status);
  if( !U_SUCCESS(status) ){
    icuFunctionError(p, "ucol_open", status);
    return;
  }
  assert(p);

  rc = sqlite3_create_collation_v2(db, zName, SQLITE_UTF16, (void *)pUCollator, 
      icuCollationColl, icuCollationDel
  );
  if( rc!=SQLITE_OK ){
    ucol_close(pUCollator);
    sqlite3_result_error(p, "Error registering collation function", -1);
  }
}

/*
** Register the ICU extension functions with database db.
*/
SQLITE_PRIVATE int sqlite3IcuInit(sqlite3 *db){
  struct IcuScalar {
    const char *zName;                        /* Function name */
    int nArg;                                 /* Number of arguments */
    int enc;                                  /* Optimal text encoding */
    void *pContext;                           /* sqlite3_user_data() context */
    void (*xFunc)(sqlite3_context*,int,sqlite3_value**);
  } scalars[] = {
    {"regexp", 2, SQLITE_ANY,          0, icuRegexpFunc},

    {"lower",  1, SQLITE_UTF16,        0, icuCaseFunc16},
    {"lower",  2, SQLITE_UTF16,        0, icuCaseFunc16},
    {"upper",  1, SQLITE_UTF16, (void*)1, icuCaseFunc16},
    {"upper",  2, SQLITE_UTF16, (void*)1, icuCaseFunc16},

    {"lower",  1, SQLITE_UTF8,         0, icuCaseFunc16},
    {"lower",  2, SQLITE_UTF8,         0, icuCaseFunc16},
    {"upper",  1, SQLITE_UTF8,  (void*)1, icuCaseFunc16},
    {"upper",  2, SQLITE_UTF8,  (void*)1, icuCaseFunc16},

    {"like",   2, SQLITE_UTF8,         0, icuLikeFunc},
    {"like",   3, SQLITE_UTF8,         0, icuLikeFunc},

    {"icu_load_collation",  2, SQLITE_UTF8, (void*)db, icuLoadCollation},
  };

  int rc = SQLITE_OK;
  int i;

  for(i=0; rc==SQLITE_OK && i<(int)(sizeof(scalars)/sizeof(scalars[0])); i++){
    struct IcuScalar *p = &scalars[i];
    rc = sqlite3_create_function(
        db, p->zName, p->nArg, p->enc, p->pContext, p->xFunc, 0, 0
    );
  }

  return rc;
}

#if !SQLITE_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
SQLITE_API int sqlite3_icu_init(
  sqlite3 *db, 
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  SQLITE_EXTENSION_INIT2(pApi)
  return sqlite3IcuInit(db);
}
#endif

#endif

/************** End of icu.c *************************************************/
/************** Begin file fts3_icu.c ****************************************/
/*
** 2007 June 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file implements a tokenizer for fts3 based on the ICU library.
*/
#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3)
#ifdef SQLITE_ENABLE_ICU

/* #include <assert.h> */
/* #include <string.h> */

#include <unicode/ubrk.h>
/* #include <unicode/ucol.h> */
/* #include <unicode/ustring.h> */
#include <unicode/utf16.h>

typedef struct IcuTokenizer IcuTokenizer;
typedef struct IcuCursor IcuCursor;

struct IcuTokenizer {
  sqlite3_tokenizer base;
  char *zLocale;
};

struct IcuCursor {
  sqlite3_tokenizer_cursor base;

  UBreakIterator *pIter;      /* ICU break-iterator object */
  int nChar;                  /* Number of UChar elements in pInput */
  UChar *aChar;               /* Copy of input using utf-16 encoding */
  int *aOffset;               /* Offsets of each character in utf-8 input */

  int nBuffer;
  char *zBuffer;

  int iToken;
};

/*
** Create a new tokenizer instance.
*/
static int icuCreate(
  int argc,                            /* Number of entries in argv[] */
  const char * const *argv,            /* Tokenizer creation arguments */
  sqlite3_tokenizer **ppTokenizer      /* OUT: Created tokenizer */
){
  IcuTokenizer *p;
  int n = 0;

  if( argc>0 ){
    n = strlen(argv[0])+1;
  }
  p = (IcuTokenizer *)sqlite3_malloc(sizeof(IcuTokenizer)+n);
  if( !p ){
    return SQLITE_NOMEM;
  }
  memset(p, 0, sizeof(IcuTokenizer));

  if( n ){
    p->zLocale = (char *)&p[1];
    memcpy(p->zLocale, argv[0], n);
  }

  *ppTokenizer = (sqlite3_tokenizer *)p;

  return SQLITE_OK;
}

/*
** Destroy a tokenizer
*/
static int icuDestroy(sqlite3_tokenizer *pTokenizer){
  IcuTokenizer *p = (IcuTokenizer *)pTokenizer;
  sqlite3_free(p);
  return SQLITE_OK;
}

/*
** Prepare to begin tokenizing a particular string.  The input
** string to be tokenized is pInput[0..nBytes-1].  A cursor
** used to incrementally tokenize this string is returned in 
** *ppCursor.
*/
static int icuOpen(
  sqlite3_tokenizer *pTokenizer,         /* The tokenizer */
  const char *zInput,                    /* Input string */
  int nInput,                            /* Length of zInput in bytes */
  sqlite3_tokenizer_cursor **ppCursor    /* OUT: Tokenization cursor */
){
  IcuTokenizer *p = (IcuTokenizer *)pTokenizer;
  IcuCursor *pCsr;

  const int32_t opt = U_FOLD_CASE_DEFAULT;
  UErrorCode status = U_ZERO_ERROR;
  int nChar;

  UChar32 c;
  int iInput = 0;
  int iOut = 0;

  *ppCursor = 0;

  if( zInput==0 ){
    nInput = 0;
    zInput = "";
  }else if( nInput<0 ){
    nInput = strlen(zInput);
  }
  nChar = nInput+1;
  pCsr = (IcuCursor *)sqlite3_malloc(
      sizeof(IcuCursor) +                /* IcuCursor */
      ((nChar+3)&~3) * sizeof(UChar) +   /* IcuCursor.aChar[] */
      (nChar+1) * sizeof(int)            /* IcuCursor.aOffset[] */
  );
  if( !pCsr ){
    return SQLITE_NOMEM;
  }
  memset(pCsr, 0, sizeof(IcuCursor));
  pCsr->aChar = (UChar *)&pCsr[1];
  pCsr->aOffset = (int *)&pCsr->aChar[(nChar+3)&~3];

  pCsr->aOffset[iOut] = iInput;
  U8_NEXT(zInput, iInput, nInput, c); 
  while( c>0 ){
    int isError = 0;
    c = u_foldCase(c, opt);
    U16_APPEND(pCsr->aChar, iOut, nChar, c, isError);
    if( isError ){
      sqlite3_free(pCsr);
      return SQLITE_ERROR;
    }
    pCsr->aOffset[iOut] = iInput;

    if( iInput<nInput ){
      U8_NEXT(zInput, iInput, nInput, c);
    }else{
      c = 0;
    }
  }

  pCsr->pIter = ubrk_open(UBRK_WORD, p->zLocale, pCsr->aChar, iOut, &status);
  if( !U_SUCCESS(status) ){
    sqlite3_free(pCsr);
    return SQLITE_ERROR;
  }
  pCsr->nChar = iOut;

  ubrk_first(pCsr->pIter);
  *ppCursor = (sqlite3_tokenizer_cursor *)pCsr;
  return SQLITE_OK;
}

/*
** Close a tokenization cursor previously opened by a call to icuOpen().
*/
static int icuClose(sqlite3_tokenizer_cursor *pCursor){
  IcuCursor *pCsr = (IcuCursor *)pCursor;
  ubrk_close(pCsr->pIter);
  sqlite3_free(pCsr->zBuffer);
  sqlite3_free(pCsr);
  return SQLITE_OK;
}

/*
** Extract the next token from a tokenization cursor.
*/
static int icuNext(
  sqlite3_tokenizer_cursor *pCursor,  /* Cursor returned by simpleOpen */
  const char **ppToken,               /* OUT: *ppToken is the token text */
  int *pnBytes,                       /* OUT: Number of bytes in token */
  int *piStartOffset,                 /* OUT: Starting offset of token */
  int *piEndOffset,                   /* OUT: Ending offset of token */
  int *piPosition                     /* OUT: Position integer of token */
){
  IcuCursor *pCsr = (IcuCursor *)pCursor;

  int iStart = 0;
  int iEnd = 0;
  int nByte = 0;

  while( iStart==iEnd ){
    UChar32 c;

    iStart = ubrk_current(pCsr->pIter);
    iEnd = ubrk_next(pCsr->pIter);
    if( iEnd==UBRK_DONE ){
      return SQLITE_DONE;
    }

    while( iStart<iEnd ){
      int iWhite = iStart;
      U16_NEXT(pCsr->aChar, iWhite, pCsr->nChar, c);
      if( u_isspace(c) ){
        iStart = iWhite;
      }else{
        break;
      }
    }
    assert(iStart<=iEnd);
  }

  do {
    UErrorCode status = U_ZERO_ERROR;
    if( nByte ){
      char *zNew = sqlite3_realloc(pCsr->zBuffer, nByte);
      if( !zNew ){
        return SQLITE_NOMEM;
      }
      pCsr->zBuffer = zNew;
      pCsr->nBuffer = nByte;
    }

    u_strToUTF8(
        pCsr->zBuffer, pCsr->nBuffer, &nByte,    /* Output vars */
        &pCsr->aChar[iStart], iEnd-iStart,       /* Input vars */
        &status                                  /* Output success/failure */
    );
  } while( nByte>pCsr->nBuffer );

  *ppToken = pCsr->zBuffer;
  *pnBytes = nByte;
  *piStartOffset = pCsr->aOffset[iStart];
  *piEndOffset = pCsr->aOffset[iEnd];
  *piPosition = pCsr->iToken++;

  return SQLITE_OK;
}

/*
** The set of routines that implement the simple tokenizer
*/
static const sqlite3_tokenizer_module icuTokenizerModule = {
  0,                           /* iVersion */
  icuCreate,                   /* xCreate  */
  icuDestroy,                  /* xCreate  */
  icuOpen,                     /* xOpen    */
  icuClose,                    /* xClose   */
  icuNext,                     /* xNext    */
};

/*
** Set *ppModule to point at the implementation of the ICU tokenizer.
*/
SQLITE_PRIVATE void sqlite3Fts3IcuTokenizerModule(
  sqlite3_tokenizer_module const**ppModule
){
  *ppModule = &icuTokenizerModule;
}

#endif /* defined(SQLITE_ENABLE_ICU) */
#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_FTS3) */

/************** End of fts3_icu.c ********************************************/

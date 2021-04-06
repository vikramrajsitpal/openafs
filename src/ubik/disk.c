/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>

#include <afs/afsutil.h>

#include "ubik_internal.h"

#define	PHSIZE	128
static struct buffer {
    struct ubik_dbase *dbase;	/*!< dbase within which the buffer resides */
    afs_int32 file;		/*!< Unique cache key */
    afs_int32 page;		/*!< page number */
    struct buffer *lru_next;
    struct buffer *lru_prev;
    struct buffer *hashNext;	/*!< next dude in hash table */
    char *data;			/*!< ptr to the data */
    char lockers;		/*!< usage ref count */
    char dirty;			/*!< is buffer modified */
    char hashIndex;		/*!< back ptr to hash table */
} *Buffers;

#define pHash(page) ((page) & (PHSIZE-1))

afs_int32 ubik_nBuffers = NBUFFERS;
static struct buffer *phTable[PHSIZE];	/*!< page hash table */
static struct buffer *LruBuffer;
static int nbuffers;
static int calls = 0, ios = 0, lastb = 0;
static char *BufferData;
static struct buffer *newslot(struct ubik_dbase *adbase, afs_int32 afid,
			      afs_int32 apage);
#define	BADFID	    0xffffffff

/*!
 * \brief Remove a transaction from the database's active transaction list.  Don't free it.
 */
static int
unthread(struct ubik_trans *atrans)
{
    struct ubik_trans **lt, *tt;
    lt = &atrans->dbase->activeTrans;
    for (tt = *lt; tt; lt = &tt->next, tt = *lt) {
	if (tt == atrans) {
	    /* found it */
	    *lt = tt->next;
	    return 0;
	}
    }
    return 2;			/* no entry */
}

/*!
 * \brief some debugging assistance
 */
void
udisk_Debug(struct ubik_debug *aparm)
{
    struct buffer *tb;
    int i;

    memcpy(&aparm->localVersion, &ubik_dbase->version,
	   sizeof(struct ubik_version));
    aparm->lockedPages = 0;
    aparm->writeLockedPages = 0;
    tb = Buffers;
    for (i = 0; i < nbuffers; i++, tb++) {
	if (tb->lockers) {
	    aparm->lockedPages++;
	    if (tb->dirty)
		aparm->writeLockedPages++;
	}
    }
}

/*!
 * \brief Write an opcode to the log.
 *
 * log format is defined here, and implicitly in recovery.c
 *
 * 4 byte opcode, followed by parameters, each 4 bytes long.  All integers
 * are in logged in network standard byte order, in case we want to move logs
 * from machine-to-machine someday.
 *
 * Begin transaction: opcode \n
 * Commit transaction: opcode, version (8 bytes) \n
 * Abort transaction: opcode \n
 * Write data: opcode, file, position, length, <length> data bytes \n
 */
static int
udisk_LogOpcode(struct ubik_dbase *adbase, afs_int32 aopcode, int async)
{
    afs_int32 code;

    /* setup data and do write */
    aopcode = htonl(aopcode);
    code = uphys_buf_append(adbase, LOGFILE, &aopcode, sizeof(afs_int32));
    if (code != sizeof(afs_int32))
	return UIOERROR;

    /* optionally sync data */
    if (async)
	code = uphys_sync(adbase, LOGFILE);
    else
	code = 0;
    return code;
}

/*!
 * \brief Log a commit, never syncing.
 */
static int
udisk_LogEnd(struct ubik_dbase *adbase, struct ubik_version *aversion)
{
    afs_int32 code;
    afs_int32 data[3];

    /* setup data */
    data[0] = htonl(LOGEND);
    data[1] = htonl(aversion->epoch);
    data[2] = htonl(aversion->counter);

    /* do write */
    code = uphys_buf_append(adbase, LOGFILE, data, 3 * sizeof(afs_int32));
    if (code != 3 * sizeof(afs_int32))
	return UIOERROR;

    /* finally sync the log */
    code = uphys_sync(adbase, LOGFILE);
    return code;
}

/*!
 * \brief Write some data to the log, never syncing.
 */
static int
udisk_LogWriteData(struct ubik_dbase *adbase, afs_int32 afile, void *abuffer,
		   afs_int32 apos, afs_int32 alen)
{
    afs_int32 code;
    afs_int32 data[4];

    /* setup header */
    data[0] = htonl(LOGDATA);
    data[1] = htonl(afile);
    data[2] = htonl(apos);
    data[3] = htonl(alen);

    /* write header */
    code = uphys_buf_append(adbase, LOGFILE, data, 4 * sizeof(afs_int32));
    if (code != 4 * sizeof(afs_int32))
	return UIOERROR;

    /* write data */
    code = uphys_buf_append(adbase, LOGFILE, abuffer, alen);
    if (code != alen)
	return UIOERROR;
    return 0;
}

int
udisk_Init(int abuffers)
{
    /* Initialize the venus buffer system. */
    int i;
    struct buffer *tb;
    Buffers = calloc(abuffers, sizeof(struct buffer));
    BufferData = malloc(abuffers * UBIK_PAGESIZE);
    nbuffers = abuffers;
    for (i = 0; i < PHSIZE; i++)
	phTable[i] = 0;
    for (i = 0; i < abuffers; i++) {
	/* Fill in each buffer with an empty indication. */
	tb = &Buffers[i];
	tb->lru_next = &(Buffers[i + 1]);
	tb->lru_prev = &(Buffers[i - 1]);
	tb->data = &BufferData[UBIK_PAGESIZE * i];
	tb->file = BADFID;
    }
    Buffers[0].lru_prev = &(Buffers[abuffers - 1]);
    Buffers[abuffers - 1].lru_next = &(Buffers[0]);
    LruBuffer = &(Buffers[0]);
    return 0;
}

/*!
 * \brief Take a buffer and mark it as the least recently used buffer.
 */
static void
Dlru(struct buffer *abuf)
{
    if (LruBuffer == abuf)
	return;

    /* Unthread from where it is in the list */
    abuf->lru_next->lru_prev = abuf->lru_prev;
    abuf->lru_prev->lru_next = abuf->lru_next;

    /* Thread onto beginning of LRU list */
    abuf->lru_next = LruBuffer;
    abuf->lru_prev = LruBuffer->lru_prev;

    LruBuffer->lru_prev->lru_next = abuf;
    LruBuffer->lru_prev = abuf;
    LruBuffer = abuf;
}

/*!
 * \brief Take a buffer and mark it as the most recently used buffer.
 */
static void
Dmru(struct buffer *abuf)
{
    if (LruBuffer == abuf) {
	LruBuffer = LruBuffer->lru_next;
	return;
    }

    /* Unthread from where it is in the list */
    abuf->lru_next->lru_prev = abuf->lru_prev;
    abuf->lru_prev->lru_next = abuf->lru_next;

    /* Thread onto end of LRU list - making it the MRU buffer */
    abuf->lru_next = LruBuffer;
    abuf->lru_prev = LruBuffer->lru_prev;
    LruBuffer->lru_prev->lru_next = abuf;
    LruBuffer->lru_prev = abuf;
}

static_inline int
MatchBuffer(struct buffer *buf, int page, afs_int32 fid,
            struct ubik_trans *atrans)
{
    if (buf->page != page) {
	return 0;
    }
    if (buf->file != fid) {
	return 0;
    }
    if (atrans->type == UBIK_READTRANS && buf->dirty) {
	/* if 'buf' is dirty, it has uncommitted changes; we do not want to
	 * see uncommitted changes if we are a read transaction, so skip over
	 * it. */
	return 0;
    }
    if (buf->dbase != atrans->dbase) {
	return 0;
    }
    return 1;
}

/*!
 * \brief Get a pointer to a particular buffer.
 */
static char *
DRead(struct ubik_trans *atrans, afs_int32 fid, int page)
{
    /* Read a page from the disk. */
    struct buffer *tb, *lastbuffer, *found_tb = NULL;
    afs_int32 code;
    struct ubik_dbase *dbase = atrans->dbase;

    calls++;
    lastbuffer = LruBuffer->lru_prev;

    /* Skip for write transactions for a clean page - this may not be the right page to use */
    if (MatchBuffer(lastbuffer, page, fid, atrans)
		&& (atrans->type == UBIK_READTRANS || lastbuffer->dirty)) {
	tb = lastbuffer;
	tb->lockers++;
	lastb++;
	return tb->data;
    }
    for (tb = phTable[pHash(page)]; tb; tb = tb->hashNext) {
	if (MatchBuffer(tb, page, fid, atrans)) {
	    if (tb->dirty || atrans->type == UBIK_READTRANS) {
		found_tb = tb;
		break;
	    }
	    /* Remember this clean page - we might use it */
	    found_tb = tb;
	}
    }
    /* For a write transaction, use a matching clean page if no dirty one was found */
    if (found_tb) {
	Dmru(found_tb);
	found_tb->lockers++;
	return found_tb->data;
    }

    /* can't find it */
    tb = newslot(dbase, fid, page);
    if (!tb)
	return 0;
    memset(tb->data, 0, UBIK_PAGESIZE);

    tb->lockers++;
    code = uphys_read(dbase, fid, tb->data, page * UBIK_PAGESIZE,
		      UBIK_PAGESIZE);
    if (code < 0) {
	tb->file = BADFID;
	Dlru(tb);
	tb->lockers--;
	ViceLog(0, ("Ubik: Error reading database file: errno=%d\n", errno));
	return 0;
    }
    ios++;

    /* Note that findslot sets the page field in the buffer equal to
     * what it is searching for.
     */
    return tb->data;
}

/*!
 * \brief Mark an \p fid as invalid.
 */
int
udisk_Invalidate(struct ubik_dbase *adbase, afs_int32 afid)
{
    struct buffer *tb;
    int i;

    for (i = 0, tb = Buffers; i < nbuffers; i++, tb++) {
	if (tb->file == afid) {
	    tb->file = BADFID;
	    Dlru(tb);
	}
    }
    return 0;
}

/*!
 * \brief Move this page into the correct hash bucket.
 */
static int
FixupBucket(struct buffer *ap)
{
    struct buffer **lp, *tp;
    int i;
    /* first try to get it out of its current hash bucket, in which it might not be */
    i = ap->hashIndex;
    lp = &phTable[i];
    for (tp = *lp; tp; tp = tp->hashNext) {
	if (tp == ap) {
	    *lp = tp->hashNext;
	    break;
	}
	lp = &tp->hashNext;
    }
    /* now figure the new hash bucket */
    i = pHash(ap->page);
    ap->hashIndex = i;		/* remember where we are for deletion */
    ap->hashNext = phTable[i];	/* add us to the list */
    phTable[i] = ap;
    return 0;
}

/*!
 * \brief Create a new slot for a particular dbase page.
 */
static struct buffer *
newslot(struct ubik_dbase *adbase, afs_int32 afid, afs_int32 apage)
{
    /* Find a usable buffer slot */
    afs_int32 i;
    struct buffer *pp, *tp;

    pp = 0;			/* last pure */
    for (i = 0, tp = LruBuffer; i < nbuffers; i++, tp = tp->lru_next) {
	if (!tp->lockers && !tp->dirty) {
	    pp = tp;
	    break;
	}
    }

    if (pp == 0) {
	/* There are no unlocked buffers that don't need to be written to the disk. */
	ViceLog(0, ("Ubik: Internal Error: Unable to find free buffer in ubik cache\n"));
	return NULL;
    }

    /* Now fill in the header. */
    pp->dbase = adbase;
    pp->file = afid;
    pp->page = apage;

    FixupBucket(pp);		/* move to the right hash bucket */
    Dmru(pp);
    return pp;
}

/*!
 * \brief Release a buffer, specifying whether or not the buffer has been modified by the locker.
 */
static void
DRelease(char *ap, int flag)
{
    int index;
    struct buffer *bp;

    if (!ap)
	return;
    index = (int)(ap - (char *)BufferData) >> UBIK_LOGPAGESIZE;
    bp = &(Buffers[index]);
    bp->lockers--;
    if (flag)
	bp->dirty = 1;
    return;
}

/*!
 * \brief Flush all modified buffers, leaves dirty bits set (they're cleared
 * by DSync()).
 *
 * \note Note interaction with DSync(): you call this thing first,
 * writing the buffers to the disk.  Then you call DSync() to sync all the
 * files that were written, and to clear the dirty bits.  You should
 * always call DFlush/DSync as a pair.
 */
static int
DFlush(struct ubik_trans *atrans)
{
    int i;
    afs_int32 code;
    struct buffer *tb;
    struct ubik_dbase *adbase = atrans->dbase;

    tb = Buffers;
    for (i = 0; i < nbuffers; i++, tb++) {
	if (tb->dirty) {
	    code = tb->page * UBIK_PAGESIZE;	/* offset within file */
	    code = uphys_write(adbase, tb->file, tb->data, code,
			       UBIK_PAGESIZE);
	    if (code != UBIK_PAGESIZE)
		return UIOERROR;
	}
    }
    return 0;
}

/*!
 * \brief Flush all modified buffers.
 */
static int
DAbort(struct ubik_trans *atrans)
{
    int i;
    struct buffer *tb;

    tb = Buffers;
    for (i = 0; i < nbuffers; i++, tb++) {
	if (tb->dirty) {
	    tb->dirty = 0;
	    tb->file = BADFID;
	    Dlru(tb);
	}
    }
    return 0;
}

/**
 * Invalidate any buffers that are duplicates of abuf. Duplicate buffers
 * can appear if a read transaction reads a page that is dirty, then that
 * dirty page is synced. The read transaction will skip over the dirty page,
 * and create a new buffer, and when the dirty page is synced, it will be
 * identical (except for contents) to the read-transaction buffer.
 */
static void
DedupBuffer(struct buffer *abuf)
{
    struct buffer *tb;
    for (tb = phTable[pHash(abuf->page)]; tb; tb = tb->hashNext) {
	if (tb->page == abuf->page && tb != abuf && tb->file == abuf->file
	    && tb->dbase == abuf->dbase) {

	    tb->file = BADFID;
	    Dlru(tb);
	}
    }
}

/*!
 * \attention DSync() must only be called after DFlush(), due to its interpretation of dirty flag.
 */
static int
DSync(struct ubik_trans *atrans)
{
    int i;
    afs_int32 code;
    struct buffer *tb;
    afs_int32 file;
    afs_int32 rCode;
    struct ubik_dbase *adbase = atrans->dbase;

    rCode = 0;
    while (1) {
	file = BADFID;
	for (i = 0, tb = Buffers; i < nbuffers; i++, tb++) {
	    if (tb->dirty == 1) {
		if (file == BADFID)
		    file = tb->file;
		if (file != BADFID && tb->file == file) {
		    tb->dirty = 0;
		    DedupBuffer(tb);
		}
	    }
	}
	if (file == BADFID)
	    break;
	/* otherwise we have a file to sync */
	code = uphys_sync(adbase, file);
	if (code)
	    rCode = code;
    }
    return rCode;
}

/*!
 * \brief Same as DRead(), only do not even try to read the page.
 */
static char *
DNew(struct ubik_trans *atrans, afs_int32 fid, int page)
{
    struct buffer *tb;
    struct ubik_dbase *dbase = atrans->dbase;

    if ((tb = newslot(dbase, fid, page)) == 0)
	return NULL;
    tb->lockers++;
    memset(tb->data, 0, UBIK_PAGESIZE);
    return tb->data;
}

/*!
 * \brief Read data from database.
 */
int
udisk_read(struct ubik_trans *atrans, afs_int32 afile, void *abuffer,
	   afs_int32 apos, afs_int32 alen)
{
    char *bp;
    afs_int32 offset, len;

    if (atrans->flags & TRDONE)
	return UDONE;
    while (alen > 0) {
	bp = DRead(atrans, afile, apos >> UBIK_LOGPAGESIZE);
	if (!bp)
	    return UEOF;
	/* otherwise, min of remaining bytes and end of buffer to user mode */
	offset = apos & (UBIK_PAGESIZE - 1);
	len = UBIK_PAGESIZE - offset;
	if (len > alen)
	    len = alen;
	memcpy(abuffer, bp + offset, len);
	abuffer = (char *)abuffer + len;
	apos += len;
	alen -= len;
	DRelease(bp, 0);
    }
    return 0;
}

/*!
 * \brief Write data to database, using logs.
 */
int
udisk_write(struct ubik_trans *atrans, afs_int32 afile, void *abuffer,
	    afs_int32 apos, afs_int32 alen)
{
    char *bp;
    afs_int32 offset, len;
    afs_int32 code;

    if (atrans->flags & TRDONE)
	return UDONE;
    if (atrans->type != UBIK_WRITETRANS)
	return UBADTYPE;

    /* first write the data to the log */
    code = udisk_LogWriteData(atrans->dbase, afile, abuffer, apos, alen);
    if (code)
	return code;

    /* now update vm */
    while (alen > 0) {
	bp = DRead(atrans, afile, apos >> UBIK_LOGPAGESIZE);
	if (!bp) {
	    bp = DNew(atrans, afile, apos >> UBIK_LOGPAGESIZE);
	    if (!bp)
		return UIOERROR;
	}
	/* otherwise, min of remaining bytes and end of buffer to user mode */
	offset = apos & (UBIK_PAGESIZE - 1);
	len = UBIK_PAGESIZE - offset;
	if (len > alen)
	    len = alen;
	memcpy(bp + offset, abuffer, len);
	abuffer = (char *)abuffer + len;
	apos += len;
	alen -= len;
	DRelease(bp, 1);	/* buffer modified */
    }
    return 0;
}

/*!
 * \brief Begin a new local transaction.
 */
int
udisk_begin(struct ubik_dbase *adbase, int atype, struct ubik_trans **atrans)
{
    afs_int32 code;
    struct ubik_trans *tt;

    *atrans = NULL;
    if (atype == UBIK_WRITETRANS) {
	if (adbase->dbFlags & (DBWRITING | DBRECEIVING | DBSENDING))
	    return USYNC;
	code = udisk_LogOpcode(adbase, LOGNEW, 0);
	if (code)
	    return code;
    }
    tt = calloc(1, sizeof(struct ubik_trans));
    tt->dbase = adbase;
    tt->next = adbase->activeTrans;
    adbase->activeTrans = tt;
    tt->type = atype;
    if (atype == UBIK_READTRANS)
	adbase->readers++;
    else if (atype == UBIK_WRITETRANS) {
	UBIK_VERSION_LOCK;
	version_globals.db_writing = 1;
	UBIK_VERSION_UNLOCK;
	ubik_set_db_flags(adbase, DBWRITING);
    }
    *atrans = tt;
    return 0;
}

/*!
 * \brief Commit transaction.
 */
int
udisk_commit(struct ubik_trans *atrans)
{
    struct ubik_dbase *dbase;
    afs_int32 code = 0;
    struct ubik_version oldversion, newversion;
    afs_int32 now = FT_ApproxTime();

    if (atrans->flags & TRDONE)
	return (UTWOENDS);

    if (atrans->type == UBIK_WRITETRANS) {
	dbase = atrans->dbase;

	/* On the first write to the database. We update the versions */
	if (ubeacon_AmSyncSite() && !(urecovery_state & UBIK_RECLABELDB)) {
	    UBIK_VERSION_LOCK;
	    if (version_globals.ubik_epochTime < UBIK_MILESTONE
		|| version_globals.ubik_epochTime > now) {
		ViceLog(0,
		    ("Ubik: New database label %d is out of the valid range (%d - %d)\n",
		     version_globals.ubik_epochTime, UBIK_MILESTONE, now));
		panic("Writing Ubik DB label\n");
	    }
	    oldversion = dbase->version;
	    newversion.epoch = version_globals.ubik_epochTime;
	    newversion.counter = 1;

	    code = uphys_setlabel(dbase, 0, &newversion);
	    if (code) {
		UBIK_VERSION_UNLOCK;
		return code;
	    }

	    dbase->version = newversion;
	    UBIK_VERSION_UNLOCK;

	    urecovery_state |= UBIK_RECLABELDB;

	    /* Ignore the error here. If the call fails, the site is
	     * marked down and when we detect it is up again, we will
	     * send the entire database to it.
	     */
	    ContactQuorum_DISK_SetVersion( atrans, 1 /*CStampVersion */ ,
					   &oldversion, &newversion);
	}

	UBIK_VERSION_LOCK;
	dbase->version.counter++;	/* bump commit count */
	code = udisk_LogEnd(dbase, &dbase->version);
	if (code) {
	    dbase->version.counter--;
	    UBIK_VERSION_UNLOCK;
	    return code;
	}
	UBIK_VERSION_UNLOCK;

	/* If we fail anytime after this, then panic and let the
	 * recovery replay the log.
	 */
	code = DFlush(atrans);	/* write dirty pages to respective files */
	if (code)
	    panic("Writing Ubik DB modifications\n");
	code = DSync(atrans);	/* sync the files and mark pages not dirty */
	if (code)
	    panic("Synchronizing Ubik DB modifications\n");

	/* label the committed dbase */
	code = uphys_setlabel(dbase, 0, &dbase->version);
	if (code)
	    panic("Truncating Ubik DB\n");

	code = uphys_truncate(dbase, LOGFILE, 0);	/* discard log (optional) */
	if (code)
	    panic("Truncating Ubik logfile\n");

    }

    /* When the transaction is marked done, it also means the logfile
     * has been truncated.
     */
    atrans->flags |= TRDONE;
    return code;
}

/*!
 * \brief Abort transaction.
 */
int
udisk_abort(struct ubik_trans *atrans)
{
    struct ubik_dbase *dbase;
    afs_int32 code;

    if (atrans->flags & TRDONE)
	return UTWOENDS;

    /* Check if we are the write trans before logging abort, lest we
     * abort a good write trans in progress.
     * We don't really care if the LOGABORT gets to the log because we
     * truncate the log next. If the truncate fails, we panic; for
     * otherwise, the log entries remain. On restart, replay of the log
     * will do nothing because the abort is there or no LogEnd opcode.
     */
    dbase = atrans->dbase;
    if (atrans->type == UBIK_WRITETRANS && dbase->dbFlags & DBWRITING) {
	udisk_LogOpcode(dbase, LOGABORT, 1);
	code = uphys_truncate(dbase, LOGFILE, 0);
	if (code)
	    panic("Truncating Ubik logfile during an abort\n");
	DAbort(atrans);		/* remove all dirty pages */
    }

    /* When the transaction is marked done, it also means the logfile
     * has been truncated.
     */
    atrans->flags |= (TRABORT | TRDONE);
    return 0;
}

/*!
 * \brief Destroy a transaction after it has been committed or aborted.
 *
 * If it hasn't committed before you call this routine, we'll abort the
 * transaction for you.
 */
void
udisk_end(struct ubik_trans *atrans)
{
    struct ubik_dbase *dbase;

    if (!(atrans->flags & TRDONE))
	udisk_abort(atrans);
    dbase = atrans->dbase;

    ulock_relLock(atrans);
    unthread(atrans);

    /* check if we are the write trans before unsetting the DBWRITING bit, else
     * we could be unsetting someone else's bit.
     */
    if (atrans->type == UBIK_WRITETRANS && dbase->dbFlags & DBWRITING) {
	UBIK_VERSION_LOCK;
	version_globals.db_writing = 0;
	UBIK_VERSION_UNLOCK;
	ubik_clear_db_flags(dbase, DBWRITING);
    } else {
	dbase->readers--;
    }
    if (atrans->iovec_info.val)
	free(atrans->iovec_info.val);
    if (atrans->iovec_data.val)
	free(atrans->iovec_data.val);
    free(atrans);

    /* Wakeup any writers waiting in BeginTrans() */
#ifdef AFS_PTHREAD_ENV
    opr_cv_broadcast(&dbase->flags_cond);
#else
    LWP_NoYieldSignal(&dbase->dbFlags);
#endif
}

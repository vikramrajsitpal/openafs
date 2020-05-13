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

#include <lwp.h>

#include <lock.h>
#include <afs/opr.h>
#include <afs/afsutil.h>

#include "ubik_internal.h"

/*! \file
 * These routines are called via the proc ptr in the ubik_dbase structure.  They provide access to
 * the physical disk, by converting the file numbers being processed ( >= 0 for user data space, < 0
 * for ubik system files, such as the log) to actual pathnames to open, read, write, truncate, sync,
 * etc.
 */

#define	MAXFDCACHE  4
static struct fdcache {
    int fd;
    int fileID;
    int refCount;
} fdcache[MAXFDCACHE];

/* Cache a stdio handle for a given database file, for uphys_buf_append
 * operations. We only use buf_append for one file at a time, so only try to
 * cache a single file handle, since that's all we should need. */
static struct buf_fdcache {
    int fileID;
    FILE *stream;
} buf_fdcache;

static char pbuffer[1024];

static int uphys_buf_flush(struct ubik_dbase *adbase, afs_int32 afid);

#ifdef HAVE_PIO
# define uphys_pread  pread
# define uphys_pwrite pwrite
#else /* HAVE_PIO */
static_inline ssize_t
uphys_pread(int fd, void *buf, size_t nbyte, off_t offset)
{
    if (lseek(fd, offset, 0) < 0) {
	return -1;
    }
    return read(fd, buf, nbyte);
}

static_inline ssize_t
uphys_pwrite(int fd, void *buf, size_t nbyte, off_t offset)
{
    if (lseek(fd, offset, 0) < 0) {
	return -1;
    }
    return write(fd, buf, nbyte);
}
#endif /* !HAVE_PIO */

/*
 * Make sure our globals are initialized (currently just 'fdcache'). All
 * callers already hold DBHOLD, so we don't need any locking or pthread_once
 * behavior here.
 */
static void
uphys_init(void)
{
    static int initd;

    struct fdcache *tfd;
    int i;

    if (!initd) {
	initd = 1;
	tfd = fdcache;
	for (i = 0; i < MAXFDCACHE; tfd++, i++) {
	    tfd->fd = -1;	/* invalid value */
	    tfd->fileID = -10000;	/* invalid value */
	    tfd->refCount = 0;
	}
    }
}

/*!
 * \warning Beware, when using this function, of the header in front of most files.
 */
static int
uphys_open(struct ubik_dbase *adbase, afs_int32 afid)
{
    int fd;
    int i;
    struct fdcache *tfd;
    struct fdcache *bestfd;

    opr_Assert(!ubik_RawDbase(adbase));

    uphys_init();

    /* scan file descr cache */
    for (tfd = fdcache, i = 0; i < MAXFDCACHE; i++, tfd++) {
	if (afid == tfd->fileID && tfd->refCount == 0) {	/* don't use open fd */
	    tfd->refCount++;
	    return tfd->fd;
	}
    }

    /* not found, open it and try to enter in cache */
    snprintf(pbuffer, sizeof(pbuffer), "%s.DB%s%d", adbase->pathName,
	     (afid<0)?"SYS":"", (afid<0)?-afid:afid);
    fd = open(pbuffer, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
	/* try opening read-only */
	fd = open(pbuffer, O_RDONLY, 0);
	if (fd < 0)
	    return fd;
    }

    /* enter it in the cache */
    tfd = fdcache;
    bestfd = NULL;
    for (i = 0; i < MAXFDCACHE; i++, tfd++) {	/* look for empty slot */
	if (tfd->fd == -1) {
	    bestfd = tfd;
	    break;
	}
    }
    if (!bestfd) {		/* look for reclaimable slot */
	tfd = fdcache;
	for (i = 0; i < MAXFDCACHE; i++, tfd++) {
	    if (tfd->refCount == 0) {
		bestfd = tfd;
		break;
	    }
	}
    }
    if (bestfd) {		/* found a usable slot */
	tfd = bestfd;
	if (tfd->fd >= 0)
	    close(tfd->fd);
	tfd->fd = fd;
	tfd->refCount = 1;	/* us */
	tfd->fileID = afid;
    }

    /* finally, we're done */
    return fd;
}

/*!
 * \brief Close the file, maintaining ref count in cache structure.
 */
static int
uphys_close(int afd)
{
    int i;
    struct fdcache *tfd;

    uphys_init();

    if (afd < 0)
	return EBADF;
    tfd = fdcache;
    for (i = 0; i < MAXFDCACHE; i++, tfd++) {
	if (tfd->fd == afd) {
	    if (tfd->fileID != -10000) {
		tfd->refCount--;
		return 0;
	    } else {
		if (tfd->refCount > 0) {
		    tfd->refCount--;
		    if (tfd->refCount == 0) {
			close(tfd->fd);
			tfd->fd = -1;
		    }
		    return 0;
		}
		tfd->fd = -1;
		break;
	    }
	}
    }
    return close(afd);
}

int
uphys_stat_path(char *path, struct ubik_stat *astat)
{
    int code;
    struct stat st;

    memset(&st, 0, sizeof(st));

    code = stat(path, &st);
    if (code != 0) {
	ViceLog(0, ("ubik: Cannot stat %s, errno=%d\n", path, errno));
	return UIOERROR;
    }

    if (!S_ISREG(st.st_mode)) {
	ViceLog(0, ("ubik: Cannot stat non-file %s (mode 0x%x)\n", path,
		(unsigned)st.st_mode));
	return UIOERROR;
    }

    if (st.st_size >= HDRSIZE) {
	astat->size = st.st_size - HDRSIZE;
    }
    return 0;
}

int
uphys_read(struct ubik_dbase *adbase, afs_int32 afile,
	   void *abuffer, afs_int32 apos, afs_int32 alength)
{
    int fd;
    afs_int32 code;

    fd = uphys_open(adbase, afile);
    if (fd < 0)
	return -1;
    code = uphys_pread(fd, abuffer, alength, apos + HDRSIZE);
    uphys_close(fd);
    return code;
}

int
uphys_write(struct ubik_dbase *adbase, afs_int32 afile,
	    void *abuffer, afs_int32 apos, afs_int32 alength)
{
    int fd;
    afs_int32 code;
    afs_int32 length;

    if (adbase->write_hook != NULL) {
	(*adbase->write_hook)(adbase, afile, abuffer, apos, alength);
    }

    fd = uphys_open(adbase, afile);
    if (fd < 0)
	return -1;
    length = uphys_pwrite(fd, abuffer, alength, apos + HDRSIZE);
    code = uphys_close(fd);
    if (code)
	return -1;
    else
	return length;
}

int
uphys_truncate(struct ubik_dbase *adbase, afs_int32 afile,
	       afs_int32 asize)
{
    afs_int32 code, fd;

    /* Just in case there's memory-buffered data for this file, flush it before
     * truncating. */
    if (uphys_buf_flush(adbase, afile) < 0) {
        return UIOERROR;
    }

    fd = uphys_open(adbase, afile);
    if (fd < 0)
	return UNOENT;
    code = ftruncate(fd, asize + HDRSIZE);
    uphys_close(fd);
    return code;
}

static int
uphys_getlabel_fd(int fd, struct ubik_version *aversion)
{
    int code;
    struct ubik_hdr thdr;

    memset(&thdr, 0, sizeof(thdr));

    code = uphys_pread(fd, &thdr, sizeof(thdr), 0);
    if (code != sizeof(thdr)) {
	return UIOERROR;
    }

    aversion->epoch = ntohl(thdr.version.epoch);
    aversion->counter = ntohl(thdr.version.counter);
    return 0;
}

/*!
 * \brief Get database label, with \p aversion in host order.
 */
int
uphys_getlabel(struct ubik_dbase *adbase, afs_int32 afile,
	       struct ubik_version *aversion)
{
    afs_int32 code, fd;

    fd = uphys_open(adbase, afile);
    if (fd < 0)
	return UNOENT;
    code = uphys_getlabel_fd(fd, aversion);
    uphys_close(fd);
    return code;
}

int
uphys_getlabel_path(char *path, struct ubik_version *version)
{
    int code;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
	return UIOERROR;
    }
    code = uphys_getlabel_fd(fd, version);
    close(fd);
    return code;
}

static int
uphys_setlabel_fd(int fd, struct ubik_version *aversion)
{
    ssize_t nbytes;
    struct ubik_hdr thdr;

    memset(&thdr, 0, sizeof(thdr));

    thdr.version.epoch = htonl(aversion->epoch);
    thdr.version.counter = htonl(aversion->counter);
    thdr.magic = htonl(UBIK_MAGIC);
    thdr.size = htons(HDRSIZE);

    nbytes = uphys_pwrite(fd, &thdr, sizeof(thdr), 0);
    fsync(fd);			/* preserve over crash */
    if (nbytes != sizeof(thdr)) {
	return UIOERROR;
    }
    return 0;
}

/*!
 * \brief Label database, with \p aversion in host order.
 */
int
uphys_setlabel(struct ubik_dbase *adbase, afs_int32 afile,
	       struct ubik_version *aversion)
{
    afs_int32 code, fd;

    fd = uphys_open(adbase, afile);
    if (fd < 0)
	return UNOENT;

    code = uphys_setlabel_fd(fd, aversion);
    uphys_close(fd);
    return code;
}

int
uphys_setlabel_path(char *path, struct ubik_version *version)
{
    int code;
    int fd = open(path, O_RDWR);
    if (fd < 0) {
	return UIOERROR;
    }
    code = uphys_setlabel_fd(fd, version);
    close(fd);
    return code;
}

int
uphys_sync(struct ubik_dbase *adbase, afs_int32 afile)
{
    afs_int32 code, fd;

    /* Flush any in-memory data, so we can sync it. */
    if (uphys_buf_flush(adbase, afile) < 0) {
        return -1;
    }

    fd = uphys_open(adbase, afile);
    code = fsync(fd);
    uphys_close(fd);
    return code;
}

void
uphys_invalidate(struct ubik_dbase *adbase, afs_int32 afid)
{
    int i;
    struct fdcache *tfd;

    uphys_init();

    /* scan file descr cache */
    for (tfd = fdcache, i = 0; i < MAXFDCACHE; i++, tfd++) {
	if (afid == tfd->fileID) {
	    tfd->fileID = -10000;
	    if (tfd->fd >= 0 && tfd->refCount == 0) {
		close(tfd->fd);
		tfd->fd = -1;
	    }
	    return;
	}
    }
}

static FILE *
uphys_buf_append_open(struct ubik_dbase *adbase, afs_int32 afid)
{
    opr_Assert(!ubik_RawDbase(adbase));

    /* If we have a cached handle open for this file, just return it. */
    if (buf_fdcache.stream && buf_fdcache.fileID == afid) {
        return buf_fdcache.stream;
    }

    /* Otherwise, close the existing handle, and open a new handle for the
     * given file. */

    if (buf_fdcache.stream) {
        fclose(buf_fdcache.stream);
    }

    snprintf(pbuffer, sizeof(pbuffer), "%s.DB%s%d", adbase->pathName,
	     (afid<0)?"SYS":"", (afid<0)?-afid:afid);
    buf_fdcache.stream = fopen(pbuffer, "a");
    buf_fdcache.fileID = afid;
    return buf_fdcache.stream;
}

static int
uphys_buf_flush(struct ubik_dbase *adbase, afs_int32 afid)
{
    if (buf_fdcache.stream && buf_fdcache.fileID == afid) {
        int code = fflush(buf_fdcache.stream);
        if (code) {
            return -1;
        }
    }
    return 0;
}

/* Append the given data to the given database file, allowing the data to be
 * buffered in memory. */
int
uphys_buf_append(struct ubik_dbase *adbase, afs_int32 afid, void *adata,
                 afs_int32 alength)
{
    FILE *stream;
    size_t code;

    stream = uphys_buf_append_open(adbase, afid);
    if (!stream) {
        return -1;
    }

    code = fwrite(adata, alength, 1, stream);
    if (code != 1) {
        return -1;
    }
    return alength;
}

/**
 * Receive a ubik database from an Rx call.
 *
 * @param[in] rxcall	The rx call to receive from.
 * @param[in] path	The path to store the database into.
 * @param[in] version	The version of the database. If NULL, we will not set a
 *			label on the dbase; the caller must do so afterwards.
 * @param[in] length	The logical size of the database.
 * @return ubik error codes
 */
int
uphys_recvdb(struct rx_call *rxcall, char *path,
	     struct ubik_version *version, afs_int64 length)
{
    int code;
    afs_int32 pass;
    afs_int32 offset;
    int fd = -1;
    int tlen;
    int nbytes;
    char tbuffer[1024];

    memset(tbuffer, 0, sizeof(tbuffer));

    if (length > MAX_AFS_INT32) {
	ViceLog(0, ("ubik: Error, database too big to receive, length=%lld.\n",
		    length));
	code = UIOERROR;
	goto done;
    }

    fd = open(path, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
	ViceLog(0, ("ubik: Cannot open %s, errno=%d\n", path, errno));
	code = UIOERROR;
	goto done;
    }

    nbytes = lseek(fd, HDRSIZE, 0);
    if (nbytes != HDRSIZE) {
	ViceLog(0, ("ubik: lseek error, nbytes=%d, errno=%d\n", nbytes, errno));
	code = UIOERROR;
	goto done;
    }

    pass = 0;
    offset = 0;
    while (length > 0) {
	tlen = (length > sizeof(tbuffer) ? sizeof(tbuffer) : length);
#ifndef AFS_PTHREAD_ENV
	if (pass % 4 == 0)
	    IOMGR_Poll();
#endif
	nbytes = rx_Read(rxcall, tbuffer, tlen);
	if (nbytes != tlen) {
	    ViceLog(0, ("ubik: Rx-read bulk error, nbytes=%d/%d, call error=%d\n",
			nbytes, tlen, rx_Error(rxcall)));
	    code = UIOERROR;
	    goto done;
	}
	nbytes = write(fd, tbuffer, tlen);
	pass++;
	if (nbytes != tlen) {
	    ViceLog(0, ("ubik: local write failed, nbytes=%d/%d, errno=%d\n",
			nbytes, tlen, errno));
	    code = UIOERROR;
	    goto done;
	}
	offset += tlen;
	length -= tlen;
    }

    if (version != NULL) {
	code = uphys_setlabel_fd(fd, version);
	if (code != 0) {
	    ViceLog(0, ("ubik: setlabel failed, code=%d\n", code));
	    goto done;
	}
    }

    code = close(fd);
    fd = -1;
    if (code != 0) {
	ViceLog(0, ("ubik: close failed, errno=%d\n", errno));
	code = UIOERROR;
	goto done;
    }

 done:
    if (fd >= 0) {
	close(fd);
    }
    return code;
}

/**
 * Send a ubik database to an Rx call.
 *
 * @param[in] path	The path containing the dbase to send.
 * @param[in] rxcall	The rx call to send the dbase to.
 * @param[in] version	The version of the database. If this doesn't match
 *			what's inside 'path', UINTERNAL will be returned.
 * @param[in] length	The logical size of the database.
 * @return ubik error codes
 */
int
uphys_senddb(char *path, struct rx_call *rxcall, struct ubik_version *version,
	     afs_int64 length)
{
    int code;
    int fd = -1;
    char tbuffer[256];
    struct ubik_version disk_vers;
    int nbytes;

    memset(tbuffer, 0, sizeof(tbuffer));
    memset(&disk_vers, 0, sizeof(disk_vers));

    fd = open(path, O_RDONLY);
    if (fd < 0) {
	ViceLog(0, ("ubik: Cannot open %s, errno=%d\n", path, errno));
	code = UIOERROR;
	goto done;
    }

    code = uphys_getlabel_fd(fd, &disk_vers);
    if (code != 0) {
	ViceLog(0, ("ubik: Cannot read header from %s, code %d\n",
		path, code));
	goto done;
    }

    if (vcmp(disk_vers, *version) != 0) {
	ViceLog(0, ("ubik: Local db version mismatch: %d.%d != %d.%d\n",
		disk_vers.epoch, disk_vers.counter,
		version->epoch, version->counter));
	code = UINTERNAL;
	goto done;
    }

    nbytes = lseek(fd, HDRSIZE, 0);
    if (nbytes != HDRSIZE) {
	ViceLog(0, ("ubik: lseek failed, errno=%d\n", errno));
	code = UIOERROR;
	goto done;
    }

    while (length > 0) {
	int tlen = (length > sizeof(tbuffer) ? sizeof(tbuffer) : length);

	nbytes = read(fd, tbuffer, tlen);
	if (nbytes != tlen) {
	    ViceLog(0, ("ubik: Local disk read failed, nbytes=%d/%d\n",
		    nbytes, tlen));
	    code = UIOERROR;
	    goto done;
	}

	nbytes = rx_Write(rxcall, tbuffer, tlen);
	if (nbytes != tlen) {
	    ViceLog(0, ("ubik: Rx-write bulk error, nbytes=%d/%d, "
			"call error=%d\n", nbytes, tlen,
			rx_Error(rxcall)));
	    code = UIOERROR;
	    goto done;
	}
	length -= tlen;
    }

    code = 0;

 done:
    if (fd >= 0) {
	close(fd);
    }
    return code;
}

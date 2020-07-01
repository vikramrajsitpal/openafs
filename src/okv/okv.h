/*
 * Copyright (c) 2020 Sine Nomine Associates
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef OPENAFS_OKV_H
#define OPENAFS_OKV_H

#include <errno.h>

#include <afs/opr.h>
#include <rx/rx_opaque.h>
#include <afs/cmd.h>

struct okv_dbhandle;
struct okv_trans;

/* Flags for okv_dbhandle_setflags() */
#define OKV_DBH_NOSYNC	    0x1 /**< don't sync writes, less consistency */
#define OKV_DBH_FLAGMASK    0x1

/* Flags for okv_begin() */
#define OKV_BEGIN_RO	    0x1	/**< tx is readonly */
#define OKV_BEGIN_RW	    0x2	/**< tx is readwrite */
#define OKV_BEGIN_XTHREAD   0x4	/**< tx may be used in different threads */
#define OKV_BEGIN_FLAGMASK  0x7

/* Flags for okv_put() */
#define OKV_PUT_REPLACE	    0x1	/**< Replace key if it already exists */
#define OKV_PUT_BULKSORT    0x2	/**< Used for bulk-loading data; given key must
				 *   sort "after" previous key */
#define OKV_PUT_FLAGMASK    0x3

struct okv_statinfo {
    /*
     * For each stat, if the underlying database does not support reporting
     * that stat, the value is set to NULL. Otherwise, the value points to
     * where the actual stat is (usually somewhere in os_s).
     */
    afs_uint64 *os_entries;

    struct {
	afs_uint64 os_entries_s;
    } os_s;
};

/* Options for okv_create() */
struct okv_create_opts {
    char *engine;   /**< Storage engine to use. If NULL, a default will be
		     *   chosen. */
};

#ifdef AFS_PTHREAD_ENV

int okv_create(char *dir_path, struct okv_create_opts *c_opts,
	       struct okv_dbhandle **a_kvdb);
int okv_open(char *dir_path, struct okv_dbhandle **a_kvdb);
void okv_close(struct okv_dbhandle **a_kvdb);
int okv_unlink(char *dir_path);
int okv_copyall(struct okv_dbhandle *src, struct okv_dbhandle *dest);

int okv_dbhandle_setflags(struct okv_dbhandle *kvdb, int flags, int onoff);
struct okv_dbhandle * okv_dbhandle_ref(struct okv_dbhandle *kvdb);
void okv_dbhandle_rele(struct okv_dbhandle **a_kvdb);
char *okv_dbhandle_descr(struct okv_dbhandle *kvdb);
char *okv_dbhandle_engine(struct okv_dbhandle *kvdb);
int okv_stat(struct okv_trans *tx, struct okv_statinfo *stat);

int okv_begin(struct okv_dbhandle *kvdb, int flags, struct okv_trans **a_tx);
int okv_commit(struct okv_trans **a_tx);
void okv_abort(struct okv_trans **a_tx);

int okv_get(struct okv_trans *tx, struct rx_opaque *key,
	    struct rx_opaque *val, int *a_noent);
int okv_get_copy(struct okv_trans *tx, struct rx_opaque *key,
		 void *dest, size_t len, int *a_noent);
int okv_next(struct okv_trans *tx, struct rx_opaque *key,
	     struct rx_opaque *val, int *a_eof);

int okv_put(struct okv_trans *tx, struct rx_opaque *key,
	    struct rx_opaque *value, int flags);
int okv_del(struct okv_trans *tx, struct rx_opaque *key, int *a_noent);

int okv_rename(const char *oldpath, const char *newpath);

#else /* AFS_PTHREAD_ENV */

/*
 * We don't support okv in LWP. So for LWP, just stub out all of our public
 * calls to return errors or be noops, to avoid the need for #ifdefs in our
 * callers. When no LWP code references okv, this can be removed.
 */
static_inline int
okv_create(char *dir_path, struct okv_create_opts *c_opts,
	   struct okv_dbhandle **a_kvdb)
{
    return ENOTSUP;
}
static_inline int
okv_open(char *dir_path, struct okv_dbhandle **a_kvdb)
{
    return ENOTSUP;
}
static_inline void
okv_close(struct okv_dbhandle **a_kvdb)
{
    opr_Assert(*a_kvdb == NULL);
}
static_inline int
okv_unlink(char *dir_path)
{
    return ENOTSUP;
}
static_inline int
okv_copyall(struct okv_dbhandle *src, struct okv_dbhandle *dest)
{
    return ENOTSUP;
}
static_inline int
okv_dbhandle_setflags(struct okv_dbhandle *kvdb, int flags, int onoff)
{
    return ENOTSUP;
}
static_inline struct okv_dbhandle *
okv_dbhandle_ref(struct okv_dbhandle *kvdb)
{
    opr_Assert(kvdb == NULL);
    return NULL;
}
static_inline void
okv_dbhandle_rele(struct okv_dbhandle **a_kvdb)
{
    opr_Assert(*a_kvdb == NULL);
}
static_inline char *
okv_dbhandle_descr(struct okv_dbhandle *kvdb)
{
    return "Invalid lwp okv handle";
}
static_inline char *
okv_dbhandle_engine(struct okv_dbhandle *kvdb)
{
    return "[lwp]";
}

static_inline int
okv_stat(struct okv_trans *tx, struct okv_statinfo *stat)
{
    return ENOTSUP;
}

static_inline int
okv_begin(struct okv_dbhandle *kvdb, int flags, struct okv_trans **a_tx)
{
    return ENOTSUP;
}
static_inline int
okv_commit(struct okv_trans **a_tx)
{
    opr_Assert(*a_tx == NULL);
    return EBADF;
}
static_inline void
okv_abort(struct okv_trans **a_tx)
{
    opr_Assert(*a_tx == NULL);
}

static_inline int
okv_get(struct okv_trans *tx, struct rx_opaque *key,
	struct rx_opaque *val, int *a_noent)
{
    opr_Assert(tx == NULL);
    return EBADF;
}
static_inline int
okv_get_copy(struct okv_trans *tx, struct rx_opaque *key, void *dest,
	     size_t len, int *a_noent)
{
    opr_Assert(tx == NULL);
    return EBADF;
}
static_inline int
okv_next(struct okv_trans *tx, struct rx_opaque *key,
	 struct rx_opaque *val, int *a_eof)
{
    opr_Assert(tx == NULL);
    return EBADF;
}

static_inline int
okv_put(struct okv_trans *tx, struct rx_opaque *key,
	    struct rx_opaque *value, int flags)
{
    opr_Assert(tx == NULL);
    return EBADF;
}
static_inline int
okv_del(struct okv_trans *tx, struct rx_opaque *key, int *a_noent)
{
    opr_Assert(tx == NULL);
    return EBADF;
}
static_inline int
okv_rename(const char *oldpath, const char *newpath)
{
    if (rename(oldpath, newpath) != 0) {
	return errno;
    }
    return 0;
}

#endif /* AFS_PTHREAD_ENV */

#endif /* OPENAFS_OKV_H */

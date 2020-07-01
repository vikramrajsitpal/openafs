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

#ifndef OPENAFS_OKV_PRIVATE_H
#define OPENAFS_OKV_PRIVATE_H

#include <roken.h>
#include <errno.h>

#include "okv.h"

#include <afs/opr.h>
#include <opr/lock.h>
#include <opr/queue.h>
#include <rx/rx_atomic.h>

#include <afs/afsutil.h>

struct okv_dbhandle;
struct okv_trans;
struct okv_ops;
struct okv_txthread_callinfo;

struct okv_txthread_data {
    /* Protected by xt_lock. */
    struct okv_txthread_callinfo *xt_callinfo;

    pthread_t xt_tid;
    opr_mutex_t xt_lock;
    opr_cv_t xt_cv;
};

struct okv_devino {
    dev_t dev;
    ino_t ino;
};

/*
 * An okv_disk represents an open handle for a KV dbase on disk. Only one of
 * these can exist for a given KV dbase dir, but many 'struct okv_dbhandle's
 * can exist referencing the same struct okv_disk.
 */
struct okv_disk {
    /* Link to kvdlist_head (protected by kvdlist_lock). */
    struct opr_queue kvd_link;

    /* devino for the container dir on disk (protected by kvdlist_lock). */
    struct okv_devino kvd_devino;

    struct okv_ops *kvd_ops;
    void *kvd_rock;

    rx_atomic_t kvd_refcnt;

    struct okv_txthread_data *kvd_txthread;
    struct okv_txthread_data kvd_txthread_s;

    /* Items below here are protected by kvd_lock. */

    /* If this is set, the okv_disk is closing. The struct will be freed as
     * soon as we can remove it from the global kvdlist. */
    int kvd_closing;

    /* The active write tx for this dbase. We can only one one active write tx
     * at a time; subsequent writes must wait for the active tx to finish. */
    struct okv_trans *kvd_write_tx;

    opr_mutex_t kvd_lock;
    opr_cv_t kvd_cv;
};

/*
 * An okv_dbhandle represents a caller's handle for a KV dbase. Multiple
 * okv_dbhandles can exist for a given KV dbase dir, but they'll all share the
 * same okv_disk.
 */
struct okv_dbhandle {
    struct okv_disk *dbh_disk;

    rx_atomic_t dbh_refcnt;

    /* If this is set, someone has tried to close the dbase, and we're waiting
     * for all refs to go away. (Protected by dbh_lock.) */
    int dbh_closewait;

    opr_mutex_t dbh_lock;
    opr_cv_t dbh_cv;
};

/* A KV transaction. */
struct okv_trans {
    struct okv_dbhandle *kvt_dbh;
    void *kvt_rock;	/**< rock private to the storage engine */

    int kvt_ro;		/**< Is the rx readonly? */
    int kvt_txthread;	/**< Can the tx be used by different threads? */
};

/* ops implemented by the storage engine */
struct okv_ops {
    char *kvo_name;	/**< The name of the engine, as returned by
			 *   okv_dbhandle_engine and mentioned in struct okv_create_opts. */
    char *kvo_descr;	/***< A human-readable description of the storage engine
			 *   (possibly mentioning the version or variant, etc) */

    int kvo_txthread_rw;    /**< Does this storage engine need cross-thread
			     *   calls for a tx redirected into a dedicated
			     *   thread, for RW transactions? (e.g. in lmdb,
			     *   you cannot use a write tx across multiple
			     *   threads) */

    int (*kvo_create)(struct okv_disk *kvd, char *dir_path, FILE *conf_fh);
    int (*kvo_open)(struct okv_disk *kvd, char *dir_path,
		    cmd_config_section *conf);
    void (*kvo_close)(struct okv_disk *kvd);
    int (*kvo_setflags)(struct okv_disk *kvd, int flags, int onoff);

    int (*kvo_begin)(struct okv_trans *tx);
    int (*kvo_commit)(struct okv_trans *tx);
    void (*kvo_abort)(struct okv_trans *tx);

    int (*kvo_get)(struct okv_trans *tx, struct rx_opaque *key,
		   struct rx_opaque *value);
    int (*kvo_next)(struct okv_trans *tx, struct rx_opaque *key,
		    struct rx_opaque *value);
    int (*kvo_stat)(struct okv_trans *tx, struct okv_statinfo *stat);

    int (*kvo_put)(struct okv_trans *tx, struct rx_opaque *key,
		   struct rx_opaque *value, int flags);
    int (*kvo_del)(struct okv_trans *tx, struct rx_opaque *key, int *a_noent);
};

/*** okv_lmdb.c ***/

extern struct okv_ops okv_lmdb_ops;

#endif /* OPENAFS_OKV_PRIVATE_H */

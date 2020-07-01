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

/*
 * oafs_okv - An abstraction layer for key-value (kv) storage on disk. The
 * actual implementation is delegated to a storage backend, which is a "real"
 * kv database like LMDB, BerkeleyDB, etc.
 *
 * Note that oafs_okv has a few dependencies, and so is not part of, say,
 * libopr. We require some things from Rx, and we need config file parsing
 * (specifically, the ini-style config file format used by libcmd) in order to
 * detect the storage backend in use and any other needed metadata. We also may
 * depend on system shared libraries or other things for implementing the
 * various oafs_okv storage backends.
 *
 * An oafs_okv database consists of a mini-file format. A database located at
 * the path /tmp/data.db consists of:
 *
 * - A directory, /tmp/data.db
 * - A config file, /tmp/data.db/oafs-storage.conf
 * - Backend-specific data inside /tmp/data.db/
 *
 * The config file contains a section that looks like this (for example):
 *
 * [oafs_okv]
 * engine = lmdb
 *
 * Which just specifies that the backend storage engine for that database is
 * LMDB (specifically, referring to the oafs_okv module that uses LMDB). The
 * config file can contain other sections that contain backend-specific data
 * (e.g. connection information for non-local databases). The config file can
 * also contain other sections for use by the higher-level application; for
 * example, it may contain information to indicate that we're using oafs_okv
 * storage (as opposed to, for example, a ubik .DB0-style flat file).
 *
 * For example, a config file with other sections might look like this (for a
 * database used for ubik storage using a hypothetical MariaDB-backed
 * storage engine):
 *
 * [ubik_db]
 * format = oafs_okv
 *
 * [oafs_okv]
 * engine = mariadb
 *
 * [oafs_okv_mariadb]
 * host = db.example.com
 * user = ubik
 * password = hunter2
 */

#include <afsconfig.h>
#include <afs/param.h>

#include "okv_internal.h"

#define STORAGE_CONF_FILENAME "oafs-storage.conf"

static struct okv_ops *kvd_engines[] = {
    &okv_lmdb_ops,
    NULL
};
static struct okv_ops *default_ops = &okv_lmdb_ops;

enum txcall_op {
    OKV_SHUTDOWN = 1,
    OKV_BEGIN,
    OKV_ABORT,
    OKV_COMMIT,
    OKV_GET,
    OKV_NEXT,
    OKV_PUT,
    OKV_DEL,
    OKV_STAT,
};

/*
 * Arguments for all of our user-facing calls (get, put, del, etc.). We pass
 * our args around in a struct to make it easier to make cross-thread calls.
 * (See tx_call, and related functions.)
 */
struct txcall_args {
    struct rx_opaque *xa_key;
    struct rx_opaque *xa_value;
    int *xa_anoent;
    int *xa_aeof;
    int xa_flags;
    struct okv_statinfo *xa_stat;
};

/*
 * Info about a single cross-thread call. txthread_call() submits requests
 * using this struct, which are processed by txthread_loop().
 */
struct okv_txthread_callinfo {
    int ci_done;
    int ci_code;

    struct okv_trans **ci_atx;
    enum txcall_op ci_op;
    struct txcall_args *ci_args;
};

static void init_locks(void);
static_inline void
init_okv(void)
{
    static pthread_once_t okv_once = PTHREAD_ONCE_INIT;
    opr_Verify(pthread_once(&okv_once, init_locks) == 0);
}

/* Given a path to an okv db (e.g. /tmp/foo.db), calculates the path to the
 * metadata config file (e.g. /tmp/foo.db/oafs-storage.conf). */
static int
get_conf_path(char *dir_path, char **a_path)
{
    int code = 0;
    int nbytes;
    nbytes = asprintf(a_path, "%s/%s", dir_path, STORAGE_CONF_FILENAME);
    if (nbytes < 0) {
	*a_path = NULL;
	code = ENOMEM;
    }
    return code;
}

/* Is the given 'key' blob valid? */
static int
check_key(struct rx_opaque *key)
{
    if (key == NULL || key->val == NULL || key->len < 1) {
	return EINVAL;
    }
    return 0;
}

/* Is the given 'value' blob valid? */
static int
check_value(struct rx_opaque *value)
{
    if (value == NULL) {
	return EINVAL;
    }
    return 0;
}

/* Implementation for okv_begin(). */
static int
tx_begin(struct okv_trans *tx)
{
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
    opr_Assert(kvd->kvd_ops->kvo_begin != NULL);
    return (*kvd->kvd_ops->kvo_begin)(tx);
}

static void
tx_free(struct okv_trans **a_tx)
{
    struct okv_trans *tx = *a_tx;
    *a_tx = NULL;
    if (tx == NULL) {
	return;
    }

    if (!tx->kvt_ro) {
	/*
	 * We're ending a write tx, so clear kvd_write_tx. Only do this if
	 * we're marked as the active write tx; we might not be if we weren't
	 * fully initialized.
	 */
	struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
	opr_mutex_enter(&kvd->kvd_lock);
	if (kvd->kvd_write_tx == tx) {
	    kvd->kvd_write_tx = NULL;
	    opr_cv_broadcast(&kvd->kvd_cv);
	}
	opr_mutex_exit(&kvd->kvd_lock);
    }

    okv_dbhandle_rele(&tx->kvt_dbh);
    free(tx);
}

/* Implementation for okv_abort(). */
static void
tx_abort(struct okv_trans **a_tx)
{
    struct okv_trans *tx = *a_tx;
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;

    *a_tx = NULL;
    if (tx == NULL) {
	return;
    }

    opr_Assert(kvd->kvd_ops->kvo_abort != NULL);
    (*kvd->kvd_ops->kvo_abort)(tx);
    tx_free(&tx);
}

/* Implementation for okv_commit(). */
static int
tx_commit(struct okv_trans **a_tx)
{
    struct okv_trans *tx = *a_tx;
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
    int code;

    *a_tx = NULL;
    if (tx == NULL) {
	return EBADF;
    }

    opr_Assert(kvd->kvd_ops->kvo_commit != NULL);
    code = (*kvd->kvd_ops->kvo_commit)(tx);
    tx_free(&tx);

    return code;
}

/* Implementation for okv_get(). */
static int
tx_get(struct okv_trans *tx, struct rx_opaque *key,
       struct rx_opaque *value, int *a_noent)
{
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
    int code;

    code = check_key(key);
    if (code != 0) {
	return code;
    }

    opr_Assert(kvd->kvd_ops->kvo_get != NULL);
    code = (*kvd->kvd_ops->kvo_get)(tx, key, value);
    if (code != 0) {
	return code;
    }

    if (a_noent != NULL) {
	*a_noent = 0;
    }
    if (value->val == NULL) {
	if (a_noent == NULL) {
	    return ENOENT;
	}
	*a_noent = 1;
    }
    return 0;
}

/* Implementation for okv_next(). */
static int
tx_next(struct okv_trans *tx, struct rx_opaque *key,
	struct rx_opaque *value, int *a_eof)
{
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
    int code;

    opr_Assert(kvd->kvd_ops->kvo_next != NULL);
    code = (*kvd->kvd_ops->kvo_next)(tx, key, value);
    if (code != 0) {
	return code;
    }

    *a_eof = 0;
    if (value->val == NULL) {
	*a_eof = 1;
    }
    return 0;
}

/* Implementation for okv_stat(). */
static int
tx_stat(struct okv_trans *tx, struct okv_statinfo *stat)
{
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
    if (kvd->kvd_ops->kvo_stat == NULL) {
	return 0;
    }
    return (*kvd->kvd_ops->kvo_stat)(tx, stat);
}

/* Implementation for okv_put(). */
static int
tx_put(struct okv_trans *tx, struct rx_opaque *key,
       struct rx_opaque *value, int flags)
{
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
    int code;

    if (tx->kvt_ro) {
	return EACCES;
    }

    code = check_key(key);
    if (code != 0) {
	return code;
    }
    code = check_value(value);
    if (code != 0) {
	return code;
    }
    if ((flags & OKV_PUT_FLAGMASK) != flags) {
	return EINVAL;
    }

    opr_Assert(kvd->kvd_ops->kvo_put != NULL);
    return (*kvd->kvd_ops->kvo_put)(tx, key, value, flags);
}

/* Implementation for okv_del(). */
static int
tx_del(struct okv_trans *tx, struct rx_opaque *key, int *a_noent)
{
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
    int code;
    int noent = 0;

    if (tx->kvt_ro) {
	return EACCES;
    }

    code = check_key(key);
    if (code != 0) {
	return EINVAL;
    }

    opr_Assert(kvd->kvd_ops->kvo_del != NULL);
    code = (*kvd->kvd_ops->kvo_del)(tx, key, &noent);
    if (code != 0) {
	return code;
    }

    if (a_noent != NULL) {
	*a_noent = noent;
    } else if (noent) {
	code = ENOENT;
    }
    return code;
}

/* Directly run the actual transaction op (get, put, del, etc). */
static int
txcall_run(struct okv_trans **a_tx, enum txcall_op op,
	   struct txcall_args *args)
{
    switch (op) {
    case OKV_SHUTDOWN:
	return 0;

    case OKV_BEGIN:
	return tx_begin(*a_tx);

    case OKV_ABORT:
	tx_abort(a_tx);
	return 0;

    case OKV_COMMIT:
	return tx_commit(a_tx);

    case OKV_GET:
	opr_Assert(args != NULL);
	return tx_get(*a_tx, args->xa_key, args->xa_value, args->xa_anoent);

    case OKV_NEXT:
	opr_Assert(args != NULL);
	return tx_next(*a_tx, args->xa_key, args->xa_value, args->xa_aeof);

    case OKV_PUT:
	opr_Assert(args != NULL);
	return tx_put(*a_tx, args->xa_key, args->xa_value, args->xa_flags);

    case OKV_DEL:
	opr_Assert(args != NULL);
	return tx_del(*a_tx, args->xa_key, args->xa_anoent);

    case OKV_STAT:
	opr_Assert(args != NULL);
	return tx_stat(*a_tx, args->xa_stat);

    default:
	ViceLog(0, ("okv: Internal error: xthread op 0x%x\n", op));
	return EIO;
    }
}

/*
 * Run the relevant transaction op (get, put, del, etc) on the txthread. The
 * txthread actually runs the op, we just submit the request and wait for the
 * results here.
 */
static int
txthread_call(struct okv_txthread_data *xt, struct okv_trans **a_tx,
	      enum txcall_op op, struct txcall_args *args)
{
    struct okv_txthread_callinfo info;

    opr_Assert(xt != NULL);

    memset(&info, 0, sizeof(info));
    info.ci_atx = a_tx;
    info.ci_op = op;
    info.ci_args = args;

    opr_mutex_enter(&xt->xt_lock);

    /* Wait for any running xthread call to finish. */
    while (xt->xt_callinfo != NULL) {
	opr_cv_wait(&xt->xt_cv, &xt->xt_lock);
    }

    xt->xt_callinfo = &info;

    opr_cv_broadcast(&xt->xt_cv);

    /* Wait for our call to finish running. */
    while (!info.ci_done) {
	opr_cv_wait(&xt->xt_cv, &xt->xt_lock);
    }

    opr_mutex_exit(&xt->xt_lock);

    return info.ci_code;
}

/* Does this tx need its ops to be run in the txthread? (Instead of run
 * directly.) */
static_inline int
needs_xcall(struct okv_trans *tx)
{
    if (tx->kvt_txthread) {
	return 1;
    }
    return 0;
}

/*
 * Perform some operation on a transaction (get, put, del, etc). For
 * transactions with kvt_txthread set, we do not run the operation here, but
 * submit it it to the okv_disk's dedicated write-tx thread (txthread), which
 * runs it for us and gives us back the results.
 */
static int
tx_call(struct okv_trans **a_tx, enum txcall_op op,
	struct txcall_args *args)
{
    struct okv_txthread_data *xt;

    if (!needs_xcall(*a_tx)) {
	return txcall_run(a_tx, op, args);
    }

    xt = a_tx[0]->kvt_dbh->dbh_disk->kvd_txthread;
    return txthread_call(xt, a_tx, op, args);
}

/* Main loop for the txthread. */
static void *
txthread_loop(void *rock)
{
    struct okv_disk *kvd = rock;
    struct okv_txthread_data *xt = kvd->kvd_txthread;
    char *name;

    if (asprintf(&name, "[%s] okv txthread", kvd->kvd_ops->kvo_name) < 0) {
	name = NULL;
    } else {
	opr_threadname_set(name);
	free(name);
	name = NULL;
    }

    opr_mutex_enter(&xt->xt_lock);

    for (;;) {
	struct okv_txthread_callinfo *info;

	while (xt->xt_callinfo == NULL) {
	    opr_cv_wait(&xt->xt_cv, &xt->xt_lock);
	}

	info = xt->xt_callinfo;

	info->ci_code = txcall_run(info->ci_atx, info->ci_op, info->ci_args);
	info->ci_done = 1;

	xt->xt_callinfo = NULL;
	opr_cv_broadcast(&xt->xt_cv);

	if (info->ci_op == OKV_SHUTDOWN) {
	    /* Special case to shutdown the thread. */
	    break;
	}
    }

    opr_mutex_exit(&xt->xt_lock);

    return NULL;
}

static void
txthread_start(struct okv_disk *kvd)
{
    struct okv_txthread_data *xt = &kvd->kvd_txthread_s;

    memset(xt, 0, sizeof(*xt));
    opr_mutex_init(&xt->xt_lock);
    opr_cv_init(&xt->xt_cv);

    kvd->kvd_txthread = xt;

    opr_Verify(pthread_create(&xt->xt_tid, NULL, txthread_loop, kvd) == 0);
}

static void
txthread_stop(struct okv_disk *kvd)
{
    struct okv_txthread_data *xt = kvd->kvd_txthread;
    if (xt == NULL) {
	return;
    }

    /* Signal the txthread to shutdown. */
    opr_Verify(txthread_call(xt, NULL, OKV_SHUTDOWN, NULL) == 0);

    /* Make sure the thread actually stops. */
    opr_Verify(pthread_join(xt->xt_tid, NULL) == 0);

    opr_mutex_destroy(&xt->xt_lock);
    opr_cv_destroy(&xt->xt_cv);

    memset(xt, 0, sizeof(*xt));
    kvd->kvd_txthread = NULL;
}

/**
 * Read a key/value from the db.
 *
 * Note that the contents of 'value' are only guaranteed to be valid until the
 * next operation on this transaction. If you need to keep the data around for
 * longer, make a copy!
 *
 * @param[in] tx    Transaction
 * @param[in] key   The key to retrieve
 * @param[out] value	On success and if the key exists, set to the value
 *			associated with the given key
 * @param[out] a_noent	Optional. On success, set to 0 if the given key exists,
 *			or set to 1 if the key does not exist. If NULL and the
 *			given key does not exist, it is considered an error.
 * @returns errno error codes
 * @retval ENOENT   The given key does not exist, and a_noent is NULL.
 * @retval EINVAL   Invalid key given.
 */
int
okv_get(struct okv_trans *tx, struct rx_opaque *key,
	struct rx_opaque *value, int *a_noent)
{
    struct txcall_args args;
    memset(&args, 0, sizeof(args));
    args.xa_key = key;
    args.xa_value = value;
    args.xa_anoent = a_noent;
    return tx_call(&tx, OKV_GET, &args);
}

/**
 * A small wrapper around okv_get(). Call this if the size of the value in the
 * database is known and you want the value copied into a local variable. Using
 * this instead of okv_get() means the caller doesn't need to do the memcpy or
 * check the size of the value.
 *
 * If the size of the value in the database is wrong, this function will return
 * EIO and log an error.
 *
 * @param[in] tx    Transaction
 * @param[in] key   The key to retrieve
 * @param[out] dest On success and if the key exists, filled with the value
 *		    associated with the given key
 * @param[in] len   Size of 'dest'
 * @param[out] a_noent	Same as okv_get()
 *
 * @see okv_get
 *
 * @returns any error from okv_get(), and errno error codes
 * @retval EIO	    Value in the database exists, but has an incorrect size
 */
int
okv_get_copy(struct okv_trans *tx, struct rx_opaque *key, void *dest,
	     size_t len, int *a_noent)
{
    int code;
    struct rx_opaque value;
    memset(&value, 0, sizeof(value));

    code = okv_get(tx, key, &value, a_noent);
    if (code != 0) {
	return code;
    }
    if (a_noent != NULL && *a_noent) {
	return code;
    }
    if (value.len != len) {
	ViceLog(0, ("okv: Bad value size: %d != %d\n",
		(int)value.len, (int)len));
	return EIO;
    }

    memcpy(dest, value.val, len);
    return 0;
}

/**
 * Get the next key/value from the db.
 *
 * Note that the contents of 'key' and 'value' are only guaranteed to be valid
 * until the next operation on this transaction. If you need to keep the data
 * around for longer, make a copy!
 *
 * @param[in] tx    Transaction
 * @param[inout] key	The current key that we should start from. Set to
 *			0,NULL to get the first key in the database. On
 *			success, set to the next key if there is one (that is,
 *			the first key that is strictly greater than the given
 *			key).
 * @param[out] value	On success, set to the value for the key returned in
 *			'key', if there is one.
 * @param[out] a_eof	On success, set to 1 when there are no more keys.
 * @returns errno error codes
 */
int
okv_next(struct okv_trans *tx, struct rx_opaque *key,
	 struct rx_opaque *value, int *a_eof)
{
    struct txcall_args args;
    memset(&args, 0, sizeof(args));
    args.xa_key = key;
    args.xa_value = value;
    args.xa_aeof = a_eof;
    return tx_call(&tx, OKV_NEXT, &args);
}

/**
 * Get some stats about the db.
 *
 * @param[in] tx    Transaction
 * @param[out] stat Statistics about the underlying db
 * @returns errno error codes
 */
int
okv_stat(struct okv_trans *tx, struct okv_statinfo *stat)
{
    struct txcall_args args;
    int code;

    memset(stat, 0, sizeof(*stat));
    memset(&args, 0, sizeof(args));
    args.xa_stat = stat;

    code = tx_call(&tx, OKV_STAT, &args);
    if (code != 0) {
	return code;
    }

    return 0;
}

/**
 * Store a key/value to the db.
 *
 * @param[in] tx    Transaction
 * @param[in] key   The key to store
 * @param[in] value The value to store
 * @param[in] flags Bitmask of OKV_PUT_* flags
 *
 * @return errno error codes
 * @retval EINVAL   Invalid key, value, or flags given
 * @retval EACCES   Transaction is readonly
 * @retval EEXIST   Key already exists and OKV_PUT_REPLACE was not given
 */
int
okv_put(struct okv_trans *tx, struct rx_opaque *key,
	struct rx_opaque *value, int flags)
{
    struct txcall_args args;
    memset(&args, 0, sizeof(args));
    args.xa_key = key;
    args.xa_value = value;
    args.xa_flags = flags;
    return tx_call(&tx, OKV_PUT, &args);
}

/**
 * Delete a key/value from the db.
 *
 * @param[in] tx	Transaction
 * @param[in] key	The key to delete
 * @param[out] a_noent	Optional. On success, set to 1 if the given key did not
 *			exist, or set to 0 if the given key did exist. If NULL,
 *			it is considered an error for the key to not exist.
 *
 * @return errno error codes
 * @retval ENOENT   The given key did not exist, and a_noent is NULL.
 * @retval EACCES   Transaction is readonly
 * @retval EINVAL   Invalid key given
 */
int
okv_del(struct okv_trans *tx, struct rx_opaque *key, int *a_noent)
{
    struct txcall_args args;
    memset(&args, 0, sizeof(args));
    args.xa_key = key;
    args.xa_anoent = a_noent;
    return tx_call(&tx, OKV_DEL, &args);
}

/**
 * Commit a transaction.
 *
 * @param[inout] a_tx	Transaction to commit. Set to NULL on return.
 * @return errno error codes
 * @retval EBADF    transaction was already committed or aborted
 */
int
okv_commit(struct okv_trans **a_tx)
{
    if (*a_tx == NULL) {
	return EBADF;
    }
    return tx_call(a_tx, OKV_COMMIT, NULL);
}

/**
 * Abort a transaction.
 *
 * @param[inout] a_tx	Transaction to abort. If NULL, nothing is done. Set to
 *			NULL on return.
 */
void
okv_abort(struct okv_trans **a_tx)
{
    int code;
    if (*a_tx == NULL) {
	return;
    }
    code = tx_call(a_tx, OKV_ABORT, NULL);
    opr_Assert(code == 0);
}

/**
 * Copy the entire contents of one db to another.
 *
 * @param[in] src_dbh	The source db.
 * @param[in] dest_dbh	The destination db.
 * @return errno error codes
 */
int
okv_copyall(struct okv_dbhandle *src_dbh, struct okv_dbhandle *dest_dbh)
{
    int code;
    struct okv_trans *src_tx = NULL;
    struct okv_trans *dest_tx = NULL;
    struct rx_opaque key;
    struct rx_opaque value;

    memset(&key, 0, sizeof(key));
    memset(&value, 0, sizeof(value));

    code = okv_begin(src_dbh, OKV_BEGIN_RO, &src_tx);
    if (code != 0) {
	goto done;
    }

    code = okv_begin(dest_dbh, OKV_BEGIN_RW, &dest_tx);
    if (code != 0) {
	goto done;
    }

    for (;;) {
	int eof = 0;
	code = okv_next(src_tx, &key, &value, &eof);
	if (code != 0) {
	    goto done;
	}
	if (eof) {
	    break;
	}

	code = okv_put(dest_tx, &key, &value, OKV_PUT_BULKSORT);
	if (code != 0) {
	    goto done;
	}
    }

    code = okv_commit(&dest_tx);
    if (code != 0) {
	goto done;
    }

 done:
    okv_abort(&src_tx);
    okv_abort(&dest_tx);
    return code;
}

/**
 * Start a new transaction.
 *
 * @param[in] dbh   The okv dbase handle
 * @param[in] flags Bitmask of OKV_BEGIN_* flags. Either OKV_BEGIN_RO
 *		    or OKV_BEGIN_RW must be specified, but not both.
 * @param[out] a_tx On success, set to the new transaction
 *
 * @return errno error codes
 * @retval EINVAL   invalid flags given
 */
int
okv_begin(struct okv_dbhandle *dbh, int flags, struct okv_trans **a_tx)
{
    struct okv_trans *tx;
    int ro = 0;
    int rw = 0;
    int xthread = 0;
    int code;

    *a_tx = NULL;

    tx = calloc(1, sizeof(*tx));
    if (tx == NULL) {
	code = ENOMEM;
	goto done;
    }

    if ((flags & OKV_BEGIN_FLAGMASK) != flags) {
	code = EINVAL;
	goto done;
    }
    if ((flags & OKV_BEGIN_RO) != 0) {
	ro = 1;
    }
    if ((flags & OKV_BEGIN_RW) != 0) {
	rw = 1;
    }
    if ((flags & OKV_BEGIN_XTHREAD) != 0) {
	xthread = 1;
    }
    if (ro == rw) {
	code = EINVAL;
	goto done;
    }

    tx->kvt_dbh = okv_dbhandle_ref(dbh);

    /*
     * If we set kvt_txthread, all operations on this transaction will be run
     * from the same thread (the dedicated 'txthread' for the okv_disk).
     *
     * We set kvt_txthread if the caller told us that it may use this
     * transaction across threads (by giving _XTHREAD), and the storage engine
     * says it needs txthread functionality (kvo_txthread_*). kvo_txthread_rw
     * indicates it needs this behavior for RW transactions only; if an engine
     * needs this behavior for other transactions, we'll need to create a new
     * flag to indicate as such.
     */
    if (ro) {
	tx->kvt_ro = 1;

    } else {
	struct okv_disk *kvd = dbh->dbh_disk;
	opr_Assert(rw);
	if (xthread && kvd->kvd_ops->kvo_txthread_rw) {
	    tx->kvt_txthread = 1;
	}

	/*
	 * We only allow for a single write tx to run at a time (per database).
	 * We can run while other RO transactions are running, but not while
	 * another RW tx is running. So if another RW tx is running, wait for
	 * it to finish.
	 *
	 * We enforce this here, instead of letting the underlying storage code
	 * handle this, to avoid possible deadlocks in _XTHREAD behavior. If we
	 * introduce a storage engine that can handle multiple parallel RW
	 * transactions, then this could probably be skipped for them.
	 */
	opr_mutex_enter(&kvd->kvd_lock);
	while (kvd->kvd_write_tx != NULL) {
	    opr_cv_wait(&kvd->kvd_cv, &kvd->kvd_lock);
	}
	kvd->kvd_write_tx = tx;
	opr_mutex_exit(&kvd->kvd_lock);
    }

    code = tx_call(&tx, OKV_BEGIN, NULL);
    if (code != 0) {
	goto done;
    }

    *a_tx = tx;
    tx = NULL;

 done:
    okv_abort(&tx);
    return code;
}

static struct okv_disk *
kvd_ref(struct okv_disk *kvd)
{
    if (kvd != NULL) {
	int refcnt = rx_atomic_inc_and_read(&kvd->kvd_refcnt);
	opr_Assert(refcnt > 1);
    }
    return kvd;
}

static int
devino_init(char *path, struct okv_devino *devino)
{
    int code;
    struct stat st;

    memset(&st, 0, sizeof(st));

    code = lstat(path, &st);
    if (code != 0) {
	return errno;
    }

    if (!S_ISDIR(st.st_mode)) {
	ViceLog(0, ("okv: Cannot open non-dir %s (file mode %x)\n", path,
		(unsigned)st.st_mode));
	return ENOTDIR;
    }

    devino->dev = st.st_dev;
    devino->ino = st.st_ino;
    return 0;
}

static int
devino_eq(struct okv_devino *devino_a, struct okv_devino *devino_b)
{
    if (memcmp(devino_a, devino_b, sizeof(*devino_a)) == 0) {
	return 1;
    }
    return 0;
}

/*
 * 'kvdlist' is our global list of open okv_disk instances. Callers can look up
 * okv_disks by 'devino' (essentially the dev_t,ino_t pair of the okv database
 * directory).
 *
 * Why do we do this? For some okv backends (lmdb), the same database must not
 * be opened twice in the same process, beceause it uses posix advisory locking
 * as part of its locking model, and closing the relevant file effectively
 * releases all locks for that file for the process.
 *
 * To avoid the relevant consistency issues, then, when someone tries to open
 * an okv database that's already open in this process, we just give them a
 * handle to the existing okv_disk instead. To detect if that dbase is already
 * open, we look up the (dev_t,ino_t) pair of the dbase dir in this global
 * list.
 *
 * Note that we also hold our relevant lock (kvdlist_lock) across any renames
 * (via okv_rename), to avoid races between the stat()'ing the dir (to get the
 * dev_t,ino_t pair) and opening the actual database on disk. If we don't do
 * this, we could stat() the dir and do the kvdlist lookup, and then someone
 * rename()s the dir on disk, and then we open the dbase on disk; the dbase we
 * opened then doesn't match the dev_t,ino_t pair for the kvdlist.
 */

static struct opr_queue kvdlist_head = {&kvdlist_head, &kvdlist_head};
static opr_mutex_t kvdlist_lock;
static opr_cv_t kvdlist_cv;

static void
init_locks(void)
{
    opr_mutex_init(&kvdlist_lock);
    opr_cv_init(&kvdlist_cv);
}

/* @pre kvdlist_lock held */
static void
kvdlist_find(struct okv_devino *devino, struct okv_disk **a_kvd)
{
    struct opr_queue *cur;
    struct okv_disk *kvd = NULL;

    *a_kvd = NULL;

    for (opr_queue_Scan(&kvdlist_head, cur)) {
	kvd = opr_queue_Entry(cur, struct okv_disk, kvd_link);
	if (devino_eq(&kvd->kvd_devino, devino)) {
	    *a_kvd = kvd;
	    return;
	}
    }
}

/* @pre kvdlist_lock held */
static int
kvdlist_exists(struct okv_devino *devino)
{
    struct okv_disk *kvd = NULL;
    kvdlist_find(devino, &kvd);
    if (kvd != NULL) {
	return 1;
    }
    return 0;
}

/* @pre kvdlist_lock held */
static void
kvdlist_get(struct okv_devino *devino, struct okv_disk **a_kvd)
{
    int safety;
    struct okv_disk *kvd;

    *a_kvd = NULL;

    for (safety = 0; ; safety++) {
	opr_Assert(safety < 10000);

	kvd = NULL;
	kvdlist_find(devino, &kvd);
	if (kvd == NULL) {
	    return;
	}

	opr_mutex_enter(&kvd->kvd_lock);
	if (kvd->kvd_closing) {
	    /* kvd is about to be freed; wait for it to go away and retry. */
	    opr_mutex_exit(&kvd->kvd_lock);
	    opr_cv_wait(&kvdlist_cv, &kvdlist_lock);
	    continue;
	}
	*a_kvd = kvd_ref(kvd);
	opr_mutex_exit(&kvd->kvd_lock);

	return;
    }
}

/* @pre kvdlist_lock held */
static void
kvdlist_store(struct okv_devino *devino, struct okv_disk *kvd)
{
    opr_Assert(!kvdlist_exists(devino));
    kvd->kvd_devino = *devino;
    opr_queue_Prepend(&kvdlist_head, &kvd->kvd_link);
    opr_cv_broadcast(&kvdlist_cv);
}

/* @pre kvdlist_lock held */
static void
kvdlist_del(struct okv_disk *kvd)
{
    if (opr_queue_IsOnQueue(&kvd->kvd_link)) {
	opr_queue_Remove(&kvd->kvd_link);
	opr_cv_broadcast(&kvdlist_cv);
    }
}

static void
kvd_free(struct okv_disk **a_kvd)
{
    struct okv_disk *kvd = *a_kvd;
    *a_kvd = NULL;
    if (kvd == NULL) {
	return;
    }
    opr_cv_destroy(&kvd->kvd_cv);
    opr_mutex_destroy(&kvd->kvd_lock);

    free(kvd);
}

/* @pre if 'kvdlist_locked' is set, then kvdlist_lock is held */
static void
kvd_rele(struct okv_disk **a_kvd, int kvdlist_locked)
{
    int refcnt;
    struct okv_disk *kvd = *a_kvd;
    if (kvd == NULL) {
	return;
    }
    *a_kvd = NULL;

    opr_mutex_enter(&kvd->kvd_lock);
    refcnt = rx_atomic_dec_and_read(&kvd->kvd_refcnt);
    if (refcnt > 0) {
	opr_mutex_exit(&kvd->kvd_lock);
	return;
    }
    kvd->kvd_closing = 1;
    opr_mutex_exit(&kvd->kvd_lock);

    txthread_stop(kvd);

    opr_Assert(kvd->kvd_ops->kvo_close != NULL);
    (*kvd->kvd_ops->kvo_close)(kvd);

    if (!kvdlist_locked) {
	opr_mutex_enter(&kvdlist_lock);
    }
    kvdlist_del(kvd);
    if (!kvdlist_locked) {
	opr_mutex_exit(&kvdlist_lock);
    }

    kvd_free(&kvd);
}

static int
kvd_alloc(struct okv_ops *ops, struct okv_disk **a_kvd)
{
    struct okv_disk *kvd;

    kvd = calloc(1, sizeof(*kvd));
    if (kvd == NULL) {
	return ENOMEM;
    }

    kvd->kvd_ops = ops;

    opr_mutex_init(&kvd->kvd_lock);
    opr_cv_init(&kvd->kvd_cv);
    rx_atomic_set(&kvd->kvd_refcnt, 1);

    if (ops->kvo_txthread_rw) {
	/* Only startup the txthread if our engine indicates it actually needs
	 * _XTHREAD behavior. */
	txthread_start(kvd);
    }

    *a_kvd = kvd;

    return 0;
}

static struct okv_ops *
engine_lookup(const char *name)
{
    int eng_i;
    for (eng_i = 0; ; eng_i++) {
	struct okv_ops *ops = kvd_engines[eng_i];
	if (ops == NULL) {
	    break;
	}

	if (strcmp(name, ops->kvo_name) == 0) {
	    return ops;
	}
    }
    return NULL;
}

/**
 * Open an okv_disk for an okv database.
 *
 * @param[in] orig_path The path to the database
 * @param[out] a_kvd    An okv_disk for that database. If there is already an
 *                      okv_disk for the same db open in this process, we'll
 *                      return it instead of creating a new one.
 *
 * @return errno error codes
 */
static int
kvd_open(char *orig_path, struct okv_disk **a_kvd)
{
    const char *engine_name;
    char *conf_path = NULL;
    char *dir_path = NULL;
    struct okv_disk *kvd = NULL;
    struct okv_ops *ops = NULL;
    cmd_config_section *conf = NULL;
    struct okv_devino devino;
    struct okv_devino devino_postopen;
    int code;

    memset(&devino, 0, sizeof(devino));
    memset(&devino_postopen, 0, sizeof(devino_postopen));

    *a_kvd = NULL;

    init_okv();

    /*
     * Resolve any symlinks in our given path. This avoids possible races in
     * kvdlist_* processing where a given path can suddenly refer to a
     * different dbase because a symlink was changed. okv_rename avoids the
     * issue with moving databases around; here we avoid the issue with
     * symlinks by just resolving the symlinks before we open the dbase.
     */
    dir_path = realpath(orig_path, NULL);
    if (dir_path == NULL) {
	code = errno;
	ViceLog(0, ("okv: Cannot resolve path %s (errno %d)\n",
		orig_path, code));
	if (code != ENOMEM && code != ENOENT) {
	    code = EIO;
	}
	goto done_unlocked;
    }

    opr_mutex_enter(&kvdlist_lock);

    code = devino_init(dir_path, &devino);
    if (code != 0) {
	goto done;
    }

    kvdlist_get(&devino, &kvd);
    if (kvd != NULL) {
	/* Someone else already has this database open. Give our caller a
	 * reference to the same okv_disk, so we don't open the same db twice. */
	*a_kvd = kvd;
	kvd = NULL;
	goto done;
    }

    code = get_conf_path(dir_path, &conf_path);
    if (code != 0) {
	goto done;
    }

    code = cmd_RawConfigParseFile(conf_path, &conf);
    if (code != 0) {
	if (code != ENOENT) {
	    ViceLog(0, ("okv: Cannot parse %s, code=%d\n",
		    conf_path, code));
	}
	goto done;
    }

    engine_name = cmd_RawConfigGetString(conf, NULL, "oafs_okv", "engine",
					 (char*)NULL);
    if (engine_name == NULL) {
	ViceLog(0, ("okv: Cannot find 'engine' in %s\n", conf_path));
	code = ENOTBLK;
	goto done;
    }

    ops = engine_lookup(engine_name);
    if (ops == NULL) {
	ViceLog(0, ("okv: Cannot open kv dbase %s; no implementation found "
		    "for engine '%s'\n", dir_path, engine_name));
	code = ENOTBLK;
	goto done;
    }

    code = kvd_alloc(ops, &kvd);
    if (code != 0) {
	goto done;
    }

    opr_Assert(kvd->kvd_ops->kvo_open != NULL);
    code = (*kvd->kvd_ops->kvo_open)(kvd, dir_path, conf);
    if (code != 0) {
	goto done;
    }

    code = devino_init(dir_path, &devino_postopen);
    if (code != 0 || !devino_eq(&devino, &devino_postopen)) {
	/*
	 * After opening the dbase on disk, it looks like our devino changed
	 * (maybe someone is shuffling dbases around?). Which devino is
	 * correct: the first one or the second one? We have no way to know for
	 * sure, and without knowing the correct devino, it's not safe to open
	 * the db. So refuse to continue opening it.
	 */
	ViceLog(0, ("okv: Cannot open kv dbase %s; devino race/mismatch (code "
		"%d, %lu,%lu != %lu,%lu). Is something moving kv dbases "
		"around?\n",
		dir_path, code,
		(long unsigned)devino.dev,
		(long unsigned)devino.ino,
		(long unsigned)devino_postopen.dev,
		(long unsigned)devino_postopen.ino));
	code = EIO;
	goto done;
    }

    kvdlist_store(&devino, kvd);

    *a_kvd = kvd;
    kvd = NULL;

 done:
    /*
     * We might have a partially-opened kvd here. So we must hold kvdlist_lock
     * while we run kvd_rele(), to make sure that nobody else can try to open
     * the given database until we've closed ours.
     */
    kvd_rele(&kvd, 1);

    opr_mutex_exit(&kvdlist_lock);

 done_unlocked:
    if (conf != NULL) {
	cmd_RawConfigFileFree(conf);
    }
    free(conf_path);
    free(dir_path);

    return code;
}

/*
 * Actual implementation of okv_unlink().
 * @pre kvdlist_lock held
 */
static int
unlink_db(char *dir_path)
{
    int code;
    int nbytes;
    char *ent_path = NULL;
    DIR *dirp = NULL;

    /* Try to see if we can just do a plain rmdir, so removing empty
     * directories will still work and skip the oafs-storage.conf test below. */
    code = rmdir(dir_path);
    if (code == 0) {
	goto done;
    }

    dirp = opendir(dir_path);
    if (dirp == NULL) {
	if (errno == ENOENT) {
	    code = 0;
	    goto done;
	}
	ViceLog(0, ("okv: Cannot opendir %s, errno=%d\n", dir_path, errno));
	code = EIO;
	goto done;
    }

    code = get_conf_path(dir_path, &ent_path);
    if (code != 0) {
	goto done;
    }

    if (access(ent_path, F_OK) < 0) {
	/*
	 * The directory exists and is non-empty, but oafs-storage.conf doesn't
	 * exist. This is probably not a kv dbase; refuse to unlink it just to
	 * be safe.
	 */
	ViceLog(0, ("okv: Directory %s does not look like an okv database "
		    "(accessing %s failed with errno %d). Refusing to unlink "
		    "it.\n",
		    dir_path, ent_path, errno));
	code = EISDIR;
	goto done;
    }

    free(ent_path);
    ent_path = NULL;

    for (;;) {
	char *name;
	struct dirent *dent;
	dent = readdir(dirp);
	if (dent == NULL) {
	    break;
	}

	name = dent->d_name;

	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
	    continue;
	}

	nbytes = asprintf(&ent_path, "%s/%s", dir_path, name);
	if (nbytes < 0) {
	    ent_path = NULL;
	    code = ENOMEM;
	    goto done;
	}

	code = unlink(ent_path);
	if (code != 0 && errno == ENOENT) {
	    code = 0;
	}
	if (code != 0) {
	    ViceLog(0, ("okv: Cannot unlink %s, errno=%d\n",
			ent_path, errno));
	    code = EIO;
	    goto done;
	}

	free(ent_path);
	ent_path = NULL;
    }

    closedir(dirp);
    dirp = NULL;

    code = rmdir(dir_path);
    if (code != 0) {
	ViceLog(0, ("okv: Cannot rmdir %s, errno=%d\n", dir_path, errno));
	code = EIO;
	goto done;
    }

 done:
    free(ent_path);
    if (dirp != NULL) {
	closedir(dirp);
    }
    return code;
}

/**
 * Create an okv dbase, and return the okv_disk for it.
 *
 * @param[in] dir_path	The dir to create
 * @param[in] ops	The backend to use. If NULL, we'll pick a default.
 * @param[out] a_kvd	The opened okv_disk for the dbase.
 *
 * @return errno error codes
 */
static int
kvd_create(char *dir_path, struct okv_ops *ops, struct okv_disk **a_kvd)
{
    int code;
    int dir_created = 0;
    char *conf_path = NULL;
    FILE *fh = NULL;
    struct okv_disk *kvd = NULL;
    struct okv_devino devino;

    memset(&devino, 0, sizeof(devino));

    init_okv();

    if (ops == NULL) {
	ops = default_ops;
    }

    opr_mutex_enter(&kvdlist_lock);

    code = mkdir(dir_path, 0700);
    if (code != 0) {
	ViceLog(0, ("okv: Cannot create dir %s, errno=%d\n", dir_path, errno));
	code = EIO;
	goto done;
    }

    dir_created = 1;

    code = devino_init(dir_path, &devino);
    if (code != 0) {
	goto done;
    }

    /* We just created this dir; it should be impossible for it to exist on the
     * kvdlist. */
    opr_Assert(!kvdlist_exists(&devino));

    code = get_conf_path(dir_path, &conf_path);
    if (code != 0) {
	goto done;
    }

    fh = fopen(conf_path, "wex");
    if (fh == NULL) {
	ViceLog(0, ("okv: Cannot create %s, errno=%d\n", conf_path, errno));
	code = EIO;
	goto done;
    }

    opr_Assert(ops->kvo_name != NULL);

    fprintf(fh, "[oafs_okv]\n");
    fprintf(fh, "engine = %s\n", ops->kvo_name);

    code = kvd_alloc(ops, &kvd);
    if (code != 0) {
	goto done;
    }

    opr_Assert(ops->kvo_create != NULL);
    code = (*ops->kvo_create)(kvd, dir_path, fh);
    if (code != 0) {
	goto done;
    }

    code = fclose(fh);
    fh = NULL;
    if (code != 0) {
	ViceLog(0, ("okv: Failed to close %s, errno=%d\n",
		    conf_path, errno));
	code = EIO;
	goto done;
    }

    kvdlist_store(&devino, kvd);

    *a_kvd = kvd;
    kvd = NULL;

 done:
    /* Note that we must hold kvdlist_lock while we run kvd_rele() here. (See
     * comments in kvd_open().) */
    kvd_rele(&kvd, 1);

    if (fh != NULL) {
	fclose(fh);
	fh = NULL;
    }

    if (code != 0 && dir_created) {
	if (unlink_db(dir_path) != 0) {
	    ViceLog(0, ("okv: Failed to destroy partially-created db %s\n",
			dir_path));
	}
    }

    opr_mutex_exit(&kvdlist_lock);

    free(conf_path);

    return code;
}

/**
 * Set/clear flags for a okv_dbhandle.
 *
 * Note that if a database on disk is open in multiple dbhandles in this
 * process, this function affects all of them.
 *
 * @param[in] dbh   The dbhandle to set flags for.
 * @param[in] flags Bitmask of OKV_DBH_* flags.
 * @param[in] onoff Nonzero to set flags, zero to clear flags.
 *
 * @returns errno error codes
 * @retval EINVAL   invalid flags
 * @retval ENOTSUP  flags not supported by db
 */
int
okv_dbhandle_setflags(struct okv_dbhandle *dbh, int flags, int onoff)
{
    struct okv_disk *kvd = dbh->dbh_disk;

    if ((flags & OKV_DBH_FLAGMASK) != flags) {
	return EINVAL;
    }

    if (kvd->kvd_ops->kvo_setflags == NULL) {
	return ENOTSUP;
    }

    return (*kvd->kvd_ops->kvo_setflags)(kvd, flags, onoff);
}

/*
 * Obtain a new ref for the given okv_dbhandle.
 *
 * Note that okv_close() cannot complete until all refs on the dbh are dropped.
 * Use okv_dbhandle_rele to release a ref obtained with okv_dbhandle_ref.
 *
 * @param[in] dbh   The dbhandle to get a ref for. The caller must already hold
 *		    a valid ref on dbh.
 * @returns The 'dbh' given (for convenience)
 */
struct okv_dbhandle *
okv_dbhandle_ref(struct okv_dbhandle *dbh)
{
    if (dbh != NULL) {
	int refcnt = rx_atomic_inc_and_read(&dbh->dbh_refcnt);
	/* The caller had better already hold a ref on the given 'dbh', so
	 * going from 0 refs to 1 ref should be impossible. Check that here. */
	opr_Assert(refcnt > 1);
    }
    return dbh;
}

/**
 * Release a ref obtained with okv_dbhandle_ref.
 *
 * @param[inout] a_dbh  The ref to drop. If NULL, this is a no-op. Set to NULL
 *			on return.
 */
void
okv_dbhandle_rele(struct okv_dbhandle **a_dbh)
{
    int refcnt;
    struct okv_dbhandle *dbh = *a_dbh;
    if (dbh == NULL) {
	return;
    }
    *a_dbh = NULL;

    refcnt = rx_atomic_dec_and_read(&dbh->dbh_refcnt);
    if (refcnt == 0) {
	/*
	 * We just put the last ref for 'dbh'. Somebody must be running
	 * okv_close() on it (otherwise the refcnt should never drop to zero),
	 * and so they're waiting for us to signal to them that the refcnt has
	 * dropped to zero. So signal them now.
	 */
	opr_mutex_enter(&dbh->dbh_lock);
	opr_Assert(dbh->dbh_closewait);
	opr_cv_broadcast(&dbh->dbh_cv);
	opr_mutex_exit(&dbh->dbh_lock);
    }
}

static int
dbh_alloc(struct okv_dbhandle **a_dbh)
{
    struct okv_dbhandle *dbh;
    dbh = calloc(1, sizeof(*dbh));
    if (dbh == NULL) {
	return ENOMEM;
    }

    opr_mutex_init(&dbh->dbh_lock);
    opr_cv_init(&dbh->dbh_cv);
    rx_atomic_set(&dbh->dbh_refcnt, 1);

    *a_dbh = dbh;
    return 0;
}

static void
dbh_free(struct okv_dbhandle **a_dbh)
{
    struct okv_dbhandle *dbh = *a_dbh;
    if (dbh == NULL) {
	return;
    }
    *a_dbh = NULL;

    kvd_rele(&dbh->dbh_disk, 0);

    opr_mutex_destroy(&dbh->dbh_lock);
    opr_cv_destroy(&dbh->dbh_cv);

    free(dbh);
}

/**
 * Close a dbh.
 *
 * If there are other references to the dbh open (running transactions, or
 * okv_dbhandle_ref callers), okv_close will wait until the other refs all go
 * away before closing.
 *
 * @param[inout] a_dbh	The dbh to close. If NULL, this is a no-op. Set to NULL
 *			on return.
 */
void
okv_close(struct okv_dbhandle **a_dbh)
{
    /*
     * In here, we close the given dbh, and free it. This waits for all
     * transactions using the dbh to go away before returning.
     *
     * The way we achieve that last part is with some careful use of atomics and
     * locks, in order to avoid needing to lock the dbh on every get/put:
     *
     * - In okv_dbhandle_ref, we can inc the refcount without any extra locks.
     * This is safe, because the caller must already own a ref on dbh.
     *
     * - In okv_close, we lock the dbh and dec the refcount.
     * -- If the dec'd refcount is 0, we know nobody else is using dbh, and
     * nobody else should be able to see it. So we can destroy/free the dbh
     * without waiting.
     * -- If the dec'd refcount is not 0, then somebody else is still using the
     * dbh. We then wait on dbh_cv to be notified when the refcount drops to 0.
     * When we are signalled, when we can destroy/free the dbh, since the
     * refcount has dropped to 0.
     *
     * - In okv_dbhandle_rele, we dec the refcount. If we dec'd it to 0, that
     * means somebody must be running okv_close (otherwise the refcount can't
     * drop to 0). So we lock the dbh, and signal the cv, waking up okv_close
     * to finish destroying the dbh.
     *
     * Q: Why not simply free/close the dbh when okv_dbhandle_rele drops the
     * refcount to 0?
     * A: If we did that, then okv_close could return immediately, while the
     * dbh effectively stays open. If someone else is using a transaction for
     * the dbh that stays open for a long time, that means the relevant
     * resources will stick around, and the underlying disk files will stay
     * (even if we unlink them) for an indefinite amount of time. Current
     * callers use okv_close() as a way to release resources, or as something
     * to run before unlinking the underlying files on disk. So the current
     * scheme allows for that; when okv_close() returns success, then we know
     * the underlying resources have actually been closed and aren't in use
     * anymore.
     *
     * We could allow for different semantics in the future. If we have a
     * caller that doesn't want to wait for the close to complete, add a new
     * variant of okv_close that does so.
     */
    int refcnt;
    struct okv_dbhandle *dbh = *a_dbh;
    if (dbh == NULL) {
	return;
    }

    *a_dbh = NULL;

    opr_mutex_enter(&dbh->dbh_lock);
    refcnt = rx_atomic_dec_and_read(&dbh->dbh_refcnt);
    dbh->dbh_closewait = 1;
    while (refcnt != 0) {
	/* Wait for the last ref to go away. */
	opr_cv_wait(&dbh->dbh_cv, &dbh->dbh_lock);
	refcnt = rx_atomic_read(&dbh->dbh_refcnt);
    }
    dbh->dbh_closewait = 0;
    opr_mutex_exit(&dbh->dbh_lock);

    dbh_free(&dbh);
}

/**
 * Open a dbhandle.
 *
 * @param[in] dir_path	Path to the okv db dir to open.
 * @param[out] a_dbh	On success, set to the dbhandle for the requested dir.
 *
 * @return errno error codes
 * @retval ENOENT   'dir_path' does not exist
 * @retval ENOTBLK  'dir_path' does exist, but does not appear to be a valid
 *		    okv dbase
 */
int
okv_open(char *dir_path, struct okv_dbhandle **a_dbh)
{
    int code;
    struct okv_dbhandle *dbh = NULL;
    struct okv_disk *kvd = NULL;

    code = kvd_open(dir_path, &kvd);
    if (code != 0) {
	goto done;
    }

    code = dbh_alloc(&dbh);
    if (code != 0) {
	goto done;
    }

    dbh->dbh_disk = kvd;
    kvd = NULL;

    *a_dbh = dbh;
    dbh = NULL;

 done:
    kvd_rele(&kvd, 0);
    okv_close(&dbh);
    return code;
}

/**
 * Create a new okv db.
 *
 * @param[in] dir_path	Path to a nonexistent dir that will contain the kv
 *			dbase.
 * @param[in] c_opts	Various options. Optional; if NULL, we'll choose some
 *			defaults.
 * @param[out] a_dbh	On success, set to a context for the newly-created okv
 *			db, as would be returned from 'okv_open'.
 *
 * @return errno error codes
 * @retval EINVAL   invalid 'c_opts->engine'
 * @retval EEXIST   'dir_path' already exists
 */
int
okv_create(char *dir_path, struct okv_create_opts *c_opts, struct okv_dbhandle **a_dbh)
{
    int code;
    struct okv_dbhandle *dbh = NULL;
    struct okv_disk *kvd = NULL;
    struct okv_ops *ops = NULL;

    if (c_opts != NULL && c_opts->engine != NULL) {
	ops = engine_lookup(c_opts->engine);
	if (ops == NULL) {
	    ViceLog(0, ("okv: Invalid engine '%s'\n", c_opts->engine));
	    code = EINVAL;
	    goto done;
	}
    }

    code = kvd_create(dir_path, ops, &kvd);
    if (code != 0) {
	goto done;
    }

    code = dbh_alloc(&dbh);
    if (code != 0) {
	goto done;
    }

    dbh->dbh_disk = kvd;
    kvd = NULL;

    *a_dbh = dbh;
    dbh = NULL;

 done:
    kvd_rele(&kvd, 0);
    okv_close(&dbh);
    return code;
}

/**
 * Delete an okv db on disk.
 *
 * At least for now, this is a somewhat simplistic implementation; it's just a
 * single-level-deep unlink on the contents of the given dir. But if it looks
 * like the given dir isn't an okv db (and there are some contents in the dir),
 * we'll bail out with an error instead of deleting it.
 *
 * @param[in] dir_path	Path to the okv db dir to delete.
 *
 * @return errno error codes
 * @retval EISDIR 'dir_path' looks like a dir, but does not look like an okv db
 */
int
okv_unlink(char *dir_path)
{
    int code;

    init_okv();

    opr_mutex_enter(&kvdlist_lock);
    code = unlink_db(dir_path);
    opr_mutex_exit(&kvdlist_lock);

    return code;
}

/**
 * Get a human-readable description of the underlying storage engine for the
 * db. This tends to return some version information or other brief details on
 * the relevant library. For example: "LMDB 0.9.29: (March 16, 2021)".
 *
 * @param[in] dbh   The dbhandle for the db
 *
 * @returns A string describing the storage engine. This always returns
 *	    non-NULL, even if a NULL dbh is given. The caller must not free
 *	    this string.
 */
char *
okv_dbhandle_descr(struct okv_dbhandle *dbh)
{
    if (dbh == NULL || dbh->dbh_disk == NULL) {
	return "<NULL>";
    }
    return dbh->dbh_disk->kvd_ops->kvo_descr;
}

/**
 * Get the name of the underlying storage engine for the db.
 *
 * @param[in] dbh   The dbhandle for the db
 *
 * @returns The internal name for the storage engine (as also used for 'struct
 *	    okv_create_opts.engine'). The caller must not free this string. If
 *	    a NULL dbh is given, we'll return NULL.
 */
char *
okv_dbhandle_engine(struct okv_dbhandle *dbh)
{
    if (dbh == NULL || dbh->dbh_disk == NULL) {
	return NULL;
    }
    return dbh->dbh_disk->kvd_ops->kvo_name;
}

/**
 * A wrapper around rename() for okv databases.
 *
 * This must be used any time the caller may be moving an okv database file on
 * disk, to avoid races with our kvdlist lookup. See the comments surrouning
 * the kvdlist_* routines for more information.
 *
 * This is also required for KV backends that reference the underlying database
 * by path. For those, we need to update what path we use to access that
 * database. (Currently no KV backends need this, but some may in the future;
 * sqlite may need this.)
 *
 * @returns errno error codes
 */
int
okv_rename(const char *oldpath, const char *newpath)
{
    int code;

    init_okv();

    opr_mutex_enter(&kvdlist_lock);

    code = rename(oldpath, newpath);
    if (code != 0) {
	code = errno;
    }

    opr_mutex_exit(&kvdlist_lock);

    return code;
}

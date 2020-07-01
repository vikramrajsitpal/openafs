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

#include <afsconfig.h>
#include <afs/param.h>

#include "okv_internal.h"

#include <lmdb.h>

/* okv_lmdb.c - Glue for the lmdb backend for okv. */

struct okv_lmdb_dbase {
    struct MDB_env *env;
    MDB_dbi dbi;
};

struct okv_lmdb_trans {
    struct MDB_txn *txn;

    /*
     * The same as the 'dbi' in our okv_lmdb_dbase. This just exists in this
     * struct for convenience, so we don't need to back through the dbh to get
     * the dbi.
     */
    MDB_dbi dbi;

    pthread_t creator_tid;
    int creator_tid_set;

    struct MDB_cursor *cursor;
};

static_inline void
log_lmdb_error(const char *func, int code)
{
    ViceLog(0, ("lmdb: %s returned %d: %s\n", func, code, mdb_strerror(code)));
}

static_inline void
buf2lmdb(const struct rx_opaque *buf, struct MDB_val *m_val)
{
    m_val->mv_size = buf->len;
    m_val->mv_data = buf->val;
}

static_inline void
lmdb2buf(const struct MDB_val *m_val, struct rx_opaque *buf)
{
    buf->len = m_val->mv_size;
    buf->val = m_val->mv_data;
}

static int
db_open(struct okv_disk *kvd, char *dir_path)
{
    int code = 0;
    struct okv_lmdb_dbase *ld = NULL;
    size_t mapsize;
    MDB_dbi dbi;
    struct MDB_txn *txn = NULL;
    struct MDB_env *env = NULL;

    code = mdb_env_create(&env);
    if (code != 0) {
	log_lmdb_error("mdb_env_create", code);
	goto eio;
    }

    if (sizeof(mapsize) >= 8) {
	/* Set our arbitrary db size limit to 1 TiB. */
	mapsize  = 1024;
	mapsize *= 1024;
	mapsize *= 1024;
	mapsize *= 1024;

    } else {
	/* Except if we're on a 32-bit arch, set the limit to 100MiB. */
	static int warned;
	mapsize  = 1024;
	mapsize *= 1024;
	mapsize *= 100;

	if (!warned) {
	    warned = 1;
	    ViceLog(0, ("lmdb: Warning, db size limited to 100 MiB on this "
			"platform.\n"));
	}
    }

    code = mdb_env_set_mapsize(env, mapsize);
    if (code != 0) {
	log_lmdb_error("mdb_env_set_mapsize", code);
	goto eio;
    }

    code = mdb_env_open(env, dir_path, MDB_NOTLS, 0700);
    if (code != 0) {
	ViceLog(0, ("lmdb: mdb_env_open(%s) returned %d: %s\n", dir_path, code,
		mdb_strerror(code)));
	goto eio;
    }

    code = mdb_reader_check(env, NULL);
    if (code != 0) {
	log_lmdb_error("mdb_reader_check", code);
	goto eio;
    }

    /* Create a temporary transaction to open our dbi handle. */

    code = mdb_txn_begin(env, NULL, 0, &txn);
    if (code != 0) {
	log_lmdb_error("mdb_txn_begin", code);
	goto eio;
    }

    code = mdb_dbi_open(txn, NULL, 0, &dbi);
    if (code != 0) {
	log_lmdb_error("mdb_dbi_open", code);
	goto eio;
    }

    code = mdb_txn_commit(txn);
    txn = NULL;
    if (code != 0) {
	log_lmdb_error("initial mdb_txn_commit", code);
	goto eio;
    }

    ld = calloc(1, sizeof(*ld));
    if (ld == NULL) {
	code = ENOMEM;
	goto done;
    }

    ld->env = env;
    ld->dbi = dbi;
    env = NULL;

    kvd->kvd_rock = ld;

 done:
    if (txn != NULL) {
	mdb_txn_abort(txn);
    }
    if (env != NULL) {
	mdb_env_close(env);
    }
    return code;

 eio:
    code = EIO;
    goto done;
}

static int
okv_lmdb_open(struct okv_disk *kvd, char *dir_path, cmd_config_section *config)
{
    return db_open(kvd, dir_path);
}

static int
okv_lmdb_create(struct okv_disk *kvd, char *dir_path, FILE *config_fh)
{
    return db_open(kvd, dir_path);
}

static void
okv_lmdb_close(struct okv_disk *kvd)
{
    struct okv_lmdb_dbase *ld = kvd->kvd_rock;
    if (ld == NULL) {
	return;
    }

    kvd->kvd_rock = NULL;

    mdb_env_close(ld->env);
    ld->env = NULL;
    free(ld);
}

static int
okv_lmdb_setflags(struct okv_disk *kvd, int flags, int onoff)
{
    struct okv_lmdb_dbase *ld = kvd->kvd_rock;
    unsigned int m_flags = 0;
    int code = 0;

    if ((flags & OKV_DBH_NOSYNC) != 0) {
	flags &= ~OKV_DBH_NOSYNC;
	m_flags |= MDB_NOSYNC;
    }

    if (flags != 0) {
	/* There are still some flags set, which we apparently don't support. */
	ViceLog(0, ("lmdb: Error: Unknown flags given to okv_dbhandle_setflags: "
		"0x%x\n", flags));
	code = ENOTSUP;
	goto done;
    }

    code = mdb_env_set_flags(ld->env, m_flags, onoff);
    if (code != 0) {
	log_lmdb_error("mdb_env_set_flags", code);
	code = EIO;
	goto done;
    }

 done:
    return code;
}

static int
okv_lmdb_begin(struct okv_trans *tx)
{
    struct okv_disk *kvd = tx->kvt_dbh->dbh_disk;
    struct okv_lmdb_dbase *ld = kvd->kvd_rock;
    unsigned int m_flags = 0;
    struct MDB_txn *m_txn = NULL;
    struct okv_lmdb_trans *ltx;
    int code;

    if (tx->kvt_ro) {
	m_flags |= MDB_RDONLY;
    }

    code = mdb_txn_begin(ld->env, NULL, m_flags, &m_txn);
    if (code != 0) {
	log_lmdb_error("mdb_txn_begin", code);
	code = EIO;
	goto done;
    }

    ltx = calloc(1, sizeof(*ltx));
    if (ltx == NULL) {
	code = ENOMEM;
	goto done;
    }

    ltx->txn = m_txn;
    ltx->dbi = ld->dbi;
    m_txn = NULL;

    if (!tx->kvt_ro) {
	/* Remember what thread created the tx. */
	ltx->creator_tid = pthread_self();
	ltx->creator_tid_set = 1;
    }

    tx->kvt_rock = ltx;

 done:
    if (m_txn != NULL) {
	mdb_txn_abort(m_txn);
    }
    return code;
}

static void
ltx_close(struct okv_lmdb_trans *ltx)
{
    if (ltx->creator_tid_set) {
	/*
	 * lmdb write transactions must not be used across threads; lmdb uses
	 * pthread mutexes in shared memory with write txes. Try to check this
	 * here, so we abort if someone violates this, instead of getting weird
	 * hangs or consistency errors.
	 */
	opr_Assert(pthread_equal(pthread_self(), ltx->creator_tid));
    }
    if (ltx->cursor != NULL) {
	mdb_cursor_close(ltx->cursor);
	ltx->cursor = NULL;
    }
}

static int
okv_lmdb_commit(struct okv_trans *tx)
{
    int code = 0;
    struct okv_lmdb_trans *ltx = tx->kvt_rock;

    if (ltx == NULL) {
	return EBADF;
    }

    tx->kvt_rock = NULL;

    ltx_close(ltx);

    if (ltx->txn != NULL) {
	code = mdb_txn_commit(ltx->txn);
	ltx->txn = NULL;
	if (code != 0) {
	    log_lmdb_error("mdb_txn_commit", code);
	    code = EIO;
	}
    }

    free(ltx);

    return code;
}

static void
okv_lmdb_abort(struct okv_trans *tx)
{
    struct okv_lmdb_trans *ltx = tx->kvt_rock;

    if (ltx == NULL) {
	return;
    }

    tx->kvt_rock = NULL;

    ltx_close(ltx);

    if (ltx->txn != NULL) {
	mdb_txn_abort(ltx->txn);
	ltx->txn = NULL;
    }

    free(ltx);
}

static int
okv_lmdb_get(struct okv_trans *tx, struct rx_opaque *key,
	     struct rx_opaque *value)
{
    int code;
    struct MDB_val m_key;
    struct MDB_val m_data;
    struct okv_lmdb_trans *ltx = tx->kvt_rock;

    buf2lmdb(key, &m_key);

    code = mdb_get(ltx->txn, ltx->dbi, &m_key, &m_data);
    if (code != 0) {
	if (code == MDB_NOTFOUND) {
	    memset(value, 0, sizeof(*value));
	    return 0;
	}
	log_lmdb_error("mdb_get", code);
	return EIO;
    }

    lmdb2buf(&m_data, value);
    return 0;
}

static int
okv_lmdb_put(struct okv_trans *tx, struct rx_opaque *key,
	     struct rx_opaque *value, int flags)
{
    int code;
    struct MDB_val m_key;
    struct MDB_val m_data;
    unsigned int m_flags = 0;
    struct okv_lmdb_trans *ltx = tx->kvt_rock;
    int replace = 0;

    buf2lmdb(key, &m_key);
    buf2lmdb(value, &m_data);

    if ((flags & OKV_PUT_REPLACE) != 0) {
	replace = 1;
    } else {
	m_flags |= MDB_NOOVERWRITE;
    }

    if ((flags & OKV_PUT_BULKSORT) != 0) {
	m_flags |= MDB_APPEND;
    }

    code = mdb_put(ltx->txn, ltx->dbi, &m_key, &m_data, m_flags);
    if (code == MDB_KEYEXIST && !replace) {
	return EEXIST;
    }
    if (code != 0) {
	log_lmdb_error("mdb_put", code);
	return EIO;
    }
    return 0;
}

static int
cursor_open(struct okv_lmdb_trans *ltx, struct MDB_cursor **a_cursor)
{
    if (ltx->cursor == NULL) {
	int code = mdb_cursor_open(ltx->txn, ltx->dbi, &ltx->cursor);
	if (code != 0) {
	    log_lmdb_error("mdb_cursor_open", code);
	    ltx->cursor = NULL;
	    return EIO;
	}
    }
    *a_cursor = ltx->cursor;
    return 0;
}

static int
okv_lmdb_del(struct okv_trans *tx, struct rx_opaque *key, int *a_noent)
{
    int code;
    struct MDB_val m_key;
    struct okv_lmdb_trans *ltx = tx->kvt_rock;

    *a_noent = 0;

    buf2lmdb(key, &m_key);
    code = mdb_del(ltx->txn, ltx->dbi, &m_key, NULL);
    if (code == MDB_NOTFOUND) {
	goto eof;
    }
    if (code != 0) {
	log_lmdb_error("mdb_del", code);
	return EIO;
    }

    return 0;

 eof:
    *a_noent = 1;
    return 0;
}

static int
okv_lmdb_next(struct okv_trans *tx, struct rx_opaque *key,
	      struct rx_opaque *value)
{
    int code;
    MDB_cursor_op op;
    struct MDB_cursor *cursor = NULL;
    struct MDB_val orig_key;
    struct MDB_val m_key;
    struct MDB_val m_data;
    const char *op_str;
    struct okv_lmdb_trans *ltx = tx->kvt_rock;

    memset(&orig_key, 0, sizeof(orig_key));
    memset(&m_key, 0, sizeof(m_key));
    memset(&m_data, 0, sizeof(m_data));

    code = cursor_open(ltx, &cursor);
    if (code != 0) {
	goto done;
    }

    if (key->val == NULL) {
	op = MDB_FIRST;
	op_str = "mdb_cursor_get(MDB_FIRST)";

    } else {
	/* Get the first key greater than or equal to the given key. */
	op = MDB_SET_RANGE;
	buf2lmdb(key, &m_key);
	orig_key = m_key;
	op_str = "mdb_cursor_get(MDB_SET_RANGE)";
    }

    code = mdb_cursor_get(cursor, &m_key, &m_data, op);
    if (code == MDB_NOTFOUND) {
	goto eof;
    }
    if (code != 0) {
	log_lmdb_error(op_str, code);
	code = EIO;
	goto done;
    }

    if (op == MDB_SET_RANGE && mdb_cmp(ltx->txn, ltx->dbi,
				       &m_key, &orig_key) == 0) {
	/*
	 * LMDB only lets us search for a key greater than or equal to the
	 * given key, but we want the first key that's strictly greater (not
	 * equal to). So if we didn't get the very first key, check if we got
	 * back the same key as we were given; if so, we need to move on to the
	 * next key.
	 */
	code = mdb_cursor_get(cursor, &m_key, &m_data, MDB_NEXT);
	if (code == MDB_NOTFOUND) {
	    goto eof;
	}
	if (code != 0) {
	    log_lmdb_error("mdb_cursor_get(MDB_NEXT)", code);
	    code = EIO;
	    goto done;
	}
    }

    lmdb2buf(&m_key, key);
    lmdb2buf(&m_data, value);

 done:
    return code;

 eof:
    /* eof */
    memset(key, 0, sizeof(*key));
    memset(value, 0, sizeof(*value));
    return 0;
}

int
okv_lmdb_stat(struct okv_trans *tx, struct okv_statinfo *stat)
{
    int code;
    struct MDB_stat m_stat;
    struct okv_lmdb_trans *ltx = tx->kvt_rock;

    memset(&m_stat, 0, sizeof(m_stat));

    code = mdb_stat(ltx->txn, ltx->dbi, &m_stat);
    if (code != 0) {
	log_lmdb_error("mdb_stat", code);
	code = EIO;
	goto done;
    }

    stat->os_entries = &stat->os_s.os_entries_s;
    *stat->os_entries = m_stat.ms_entries;

 done:
    return code;
}

struct okv_ops okv_lmdb_ops = {
    .kvo_name = "lmdb",
    .kvo_descr = MDB_VERSION_STRING,
    .kvo_txthread_rw = 1,

    .kvo_open = okv_lmdb_open,
    .kvo_create = okv_lmdb_create,
    .kvo_close = okv_lmdb_close,
    .kvo_setflags = okv_lmdb_setflags,

    .kvo_begin = okv_lmdb_begin,
    .kvo_commit = okv_lmdb_commit,
    .kvo_abort = okv_lmdb_abort,

    .kvo_get = okv_lmdb_get,
    .kvo_next = okv_lmdb_next,
    .kvo_stat = okv_lmdb_stat,

    .kvo_put = okv_lmdb_put,
    .kvo_del = okv_lmdb_del,
};

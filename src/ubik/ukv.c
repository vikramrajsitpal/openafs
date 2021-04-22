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

#include <roken.h>

#include <afs/opr.h>
#include <afs/afsutil.h>
#include <afs/okv.h>
#include "ubik_internal.h"

/*
 * This file implements the key/value (KV) enhancements to ubik.
 *
 * What is KV? Traditionally, a ubik-using application stores its data in a
 * custom database format, and calls ubik_Read/ubik_Write to read and write
 * bytes in a pseudo-file that is managed by ubik. In here, we refer to this as
 * the "flat-file" mode of operation.
 *
 * When a database is KV-enabled (ubik_KVDbase and ubik_KVTrans return true),
 * an application can no longer use ubik_Read/ubik_Write (they will return an
 * error), and there is no underlying flat file. Instead, ubik provides a
 * key/value store, where the application can store arbitrary values that are
 * identified by arbitrary keys. Keys and values are both just blobs of opaque
 * data, but they must be serialized in some platform-independent way, since
 * the blobs may be read by other db sites running on a different platform (so,
 * you need to htonl() stuff inside the key/value blobs).
 *
 * This allows an application to store data in the local database without
 * needing to implement a scheme to lookup data by key; for example, an
 * application no longer needs to implement a hash table or b-tree algorithm,
 * since any record can be looked up by a key directly.
 *
 * The actual implementation of the underlying KV store is performed by a KV
 * storage engine, for example: lmdb, berkeleydb, *dbm. Different db sites may
 * use different storage engines, and so ubik (in KV mode) no longer deals with
 * the raw bytes in the underlying storage. That is, even during a recovery
 * synchronization event, we don't ship the raw database files between sites;
 * ubik only deals with the data at a logical level of "key X is paired with
 * value Y". However the KV storage engine implements that is up to them.
 *
 * Specifically, we expect the KV engine to support the following:
 *
 * - A KV database transaction can be used across different threads (but not at
 * the same time; no parallel access).
 *
 * - Keys and values are arbitrary byte arrays, of course.
 *
 * - There are no specific length limits, but we do not use 0-length keys or
 * values. Our keys are currently all under 100 bytes long, and our largest
 * value is around 8k (the vldb4 extent block).
 *
 * - We do not use any multi-valued keys (no duplicates).
 *
 * - We can iterate through all key/value pairs in the database.
 *
 * - Keys are sorted according to simple byte-to-byte comparison, and iteration
 * occurs in this order.
 *
 * - We do not use any multi-database functionality; everything goes in one KV
 * store (for a single ubik database). Ubik itself has always had some partial
 * support for multiple database "file"s, but it is unused (and incomplete).
 *
 * An application using KV-enhanced ubik should keep the following in mind:
 *
 * - Each key is conventionally prefixed by some constant to indicate what
 * database the key is for (at least the first byte). e.g. for VLDB version 4,
 * each key starts with 0x04.
 *
 * - Ubik itself uses keys that start with 0x55 (ascii 'U'), to store ubik
 * metadata (e.g. the database version). The application cannot use keys that
 * start with 0x55, and ubik will throw errors if it tries to do so.
 *
 * On startup, ubik detects whether KV storage exists for the given database,
 * and tries to use it if one exists. Otherwise, if the flat-file .DB0 db
 * exists, use that. If neither exists, we'll create a new db of course; which
 * one we create is controllable by the application. By default, we'll create a
 * new flat-file .DB0, but if the application sets
 * 'struct ubik_serverinit_opts'.default_kv during startup, we'll default to
 * creating a KV database instead.
 *
 * For flat-file databases, the database file is stored in e.g. vldb.DB0. For a
 * KV database, vldb.DB0 is a symlink to a directory,
 * vldb.DB.d/vldb.1234.0.DB0. We deliberately put something in the same .DB0
 * path as the old flat-file databases, to make sure that if an older version
 * tries to use the db, it will throw an error instead of just ignoring our
 * data and creating a blank db. The symlink is there in order to perform an
 * atomic pivot when installing a new databae (see ukv_db_prepinstall for some
 * details about this scheme).
 *
 * The ubik db label is stored in the KV store (under a key prefixed with the
 * ubik-reserved 'U' prefix), and so committing involves another write to the
 * KV store. We don't need to journal data operations to the db log (.DBSYS),
 * since we rely on the KV storage engine to ensure writes are consistent and
 * durable.
 *
 * Inside the KV db dir, there is a file, oafs-storage.conf, which is part of
 * the okv mini file format. We also use this config file to indicate that the
 * contained db is of the "ubik_okv" format, in case we want to use some other
 * scheme of storing ubik database data in the future.
 */

#define DB_ENGINE "ubik_okv"
#define STORAGE_CONF_FILENAME "oafs-storage.conf"

static struct rx_opaque ukv_key_label = {
    .val = "\x55\x00\x6C\x62", /* U<NUL>lb */
    .len = 4,
};

/* Is 'key' an ubik-private key? (Not for application use.) */
static int
key_private(struct rx_opaque *key)
{
    char c;

    if (key->val == NULL || key->len < 1) {
	return 0;
    }
    memcpy(&c, key->val, 1);

    if (c == 'U') {
	return 1;
    }
    return 0;
}

static int
check_key(struct rx_opaque *key)
{
    if (key->val == NULL || key->len < 1) {
	ViceLog(0, ("ubik-kv: Error: invalid blank key.\n"));
	return UINTERNAL;
    }
    return 0;
}

/* Check that this key a valid key for applications to use. (Applications
 * cannot use keys starting with 'U'.) */
static int
check_key_app(struct rx_opaque *key)
{
    int code;
    code = check_key(key);
    if (code != 0) {
	return code;
    }

    if (key_private(key)) {
	ViceLog(0, ("ubik-kv: Error: application tried to use ubik-private "
		    "key.\n"));
	return UINTERNAL;
    }

    return 0;
}

static int
check_value(struct rx_opaque *value)
{
    if (value->val == NULL || value->len < 1) {
	ViceLog(0, ("ubik-kv: Error: invalid blank value.\n"));
	return UINTERNAL;
    }
    return 0;
}

/* Check that this trans is okay to use for kv ops. */
static int
check_trans(struct ubik_trans *trans)
{
    if (!ubik_KVTrans(trans)) {
	return UBADTYPE;
    }
    if (trans->kv_tx == NULL) {
	return UDONE;
    }
    return 0;
}

/* Is this a KV dbase? */
int
ubik_KVDbase(struct ubik_dbase *adbase)
{
    if (adbase->kv_dbh != NULL) {
	return 1;
    }
    return 0;
}

/* Is this a KV trans? */
int
ubik_KVTrans(struct ubik_trans *trans)
{
    if ((trans->flags & TRKEYVAL) != 0) {
	return 1;
    }
    return 0;
}

/* Wrapper around okv return codes, to make sure we always return ubik error
 * codes, and don't leak errno codes to our callers. */
static int
check_okv(int okv_code)
{
    if (okv_code != 0) {
	return UIOERROR;
    }
    return 0;
}

/**
 * Fetch a key/value from the db.
 *
 * Note that the contents of 'value' are only guaranteed to be valid until the
 * next operation on this transaction. If you need to keep the data around for
 * longer, make a copy!
 *
 * @param[in] trans ubik transaction
 * @param[in] key   The key to retrieve
 * @param[out] value	On success and if the key exists, set to the value
 *			associated with the given key
 * @param[out] a_noent	Optional. On success, set to 0 if the given key exists,
 *			or set to 1 if the key does not exist. If NULL and the
 *			given key does not exist, it is considered an error.
 *
 * @return ubik error codes
 */
int
ubik_KVGet(struct ubik_trans *trans, struct rx_opaque *key,
	   struct rx_opaque *value, int *a_noent)
{
    int code;

    code = check_trans(trans);
    if (code != 0) {
	return code;
    }

    code = check_key_app(key);
    if (code != 0) {
	return code;
    }

    return check_okv(okv_get(trans->kv_tx, key, value, a_noent));
}

/**
 * Like ubik_KVGet, but copies the value into the given buffer, for
 * convenience.
 *
 * @param[in] trans ubik transaction
 * @param[in] key   The key to retrieve
 * @param[out] dest On success and if the key exists, filled with the value
 *		    associated with the given key
 * @param[in] len   Size of 'dest'
 * @param[out] a_noent	Same as ubik_KVGet
 *
 * @see ubik_KVGet
 *
 * @return ubik error codes
 */
int
ubik_KVGetCopy(struct ubik_trans *trans, struct rx_opaque *key, void *dest,
	       size_t len, int *a_noent)
{
    int code;

    code = check_trans(trans);
    if (code != 0) {
	return code;
    }

    code = check_key_app(key);
    if (code != 0) {
	return code;
    }

    return check_okv(okv_get_copy(trans->kv_tx, key, dest, len, a_noent));
}

static int
common_KVPut(struct ubik_trans *trans, struct rx_opaque *key,
	     struct rx_opaque *value, int replace)
{
    int code;
    int flags = 0;

    code = check_trans(trans);
    if (code != 0) {
	return code;
    }

    code = check_key_app(key);
    if (code != 0) {
	return code;
    }

    code = check_value(value);
    if (code != 0) {
	return code;
    }

    if (replace) {
	flags |= OKV_PUT_REPLACE;
    }
    return check_okv(okv_put(trans->kv_tx, key, value, flags));
}

/**
 * Store a key/value to the db. If the key already exists, an error is
 * returned.
 *
 * @param[in] trans ubik transaction
 * @param[in] key   The key to store
 * @param[in] value The value to store
 *
 * @return ubik error codes
 */
int
ubik_KVPut(struct ubik_trans *trans, struct rx_opaque *key,
	   struct rx_opaque *value)
{
    return common_KVPut(trans, key, value, 0);
}

/**
 * Store a key/value to the db. If the key already exists, the existing entry
 * is replaced.
 *
 * @param[in] trans ubik transaction
 * @param[in] key   The key to store
 * @param[in] value The value to store
 *
 * @return ubik error codes
 */
int
ubik_KVReplace(struct ubik_trans *trans, struct rx_opaque *key,
	       struct rx_opaque *value)
{
    return common_KVPut(trans, key, value, 1);
}

/**
 * Delete a key/value from the db.
 *
 * @param[in] trans	ubik transaction
 * @param[in] key	The key to delete
 * @param[out] a_noent	Optional. On success, set to 1 if the given key did not
 *			exist, or set to 0 if the given key did exist. If NULL,
 *			it is considered an error for the key to not exist.
 *
 * @return ubik error codes
 */
int
ubik_KVDelete(struct ubik_trans *trans, struct rx_opaque *key, int *a_noent)
{
    int code;

    code = check_trans(trans);
    if (code != 0) {
	return code;
    }

    code = check_key_app(key);
    if (code != 0) {
	return code;
    }

    return check_okv(okv_del(trans->kv_tx, key, a_noent));
}

int
ukv_next(struct okv_trans *tx, struct rx_opaque *key,
	 struct rx_opaque *value, int *a_eof)
{
    int code;

    opr_Assert(tx != NULL);

    *a_eof = 0;

    do {
	code = check_okv(okv_next(tx, key, value, a_eof));
	if (code != 0) {
	    return code;
	}

	if (*a_eof) {
	    return 0;
	}

	/* If a key is 'private' to ubik (that is, not visible to the
	 * application), skip the key and get the next one. */
    } while (key_private(key));

    return 0;
}

/**
 * Get the next key/value from the db.
 *
 * Note that the contents of 'key' and 'value' are only guaranteed to be valid
 * until the next operation on this transaction. If you need to keep the data
 * around for longer, make a copy!
 *
 * @param[in] trans	ubik transaction
 * @param[inout] key	The current key that we should start from. Set to
 *			0,NULL to get the first key in the database. On
 *			success, set to the next key if there is one (that is,
 *			the first key that is strictly greater than the given
 *			key).
 * @param[out] value	On success, set to the value for the key returned in
 *			'key', if there is one.
 * @param[out] a_eof	On success, set to 1 when there are no more keys.
 *
 * @return ubik error codes
 */
int
ubik_KVNext(struct ubik_trans *trans, struct rx_opaque *key,
	    struct rx_opaque *value, int *a_eof)
{
    int code;

    code = check_trans(trans);
    if (code != 0) {
	return code;
    }

    return ukv_next(trans->kv_tx, key, value, a_eof);
}

/* Get the ubik label from an okv_trans */
int
ukv_getlabel(struct okv_trans *tx, struct ubik_version *version)
{
    int code;
    int noent = 0;
    struct rx_opaque value;
    struct ubik_version64 label;
    XDR xdrs;

    memset(&value, 0, sizeof(value));
    memset(&label, 0, sizeof(label));
    memset(&xdrs, 0, sizeof(xdrs));

    code = check_okv(okv_get(tx, &ukv_key_label, &value, &noent));
    if (code != 0) {
	goto done;
    }

    if (noent) {
	/* Label doesn't exist? */
	ViceLog(0, ("ubik-kv: Failed to get ubik label (key not found).\n"));
	code = UIOERROR;
	goto done;
    }

    if (value.len != sizeof(label)) {
	/* Label exists, but it's the wrong size? That's weird. */
	ViceLog(0, ("ubik-kv: Error reading label: weird label size: %d != %d\n",
		    (int)value.len, (int)sizeof(label)));
	code = UIOERROR;
	goto done;
    }

    xdrmem_create(&xdrs, value.val, value.len, XDR_DECODE);
    if (!xdr_ubik_version64(&xdrs, &label)) {
	ViceLog(0, ("ubik-kv: Internal error: failed to decode db label\n"));
	code = UINTERNAL;
	goto done;
    }

    code = udb_v64to32("ukv_getlabel", &label, version);
    if (code != 0) {
	goto done;
    }

 done:
    return code;
}

int
ukv_getlabel_db(struct ubik_dbase *dbase, struct ubik_version *version)
{
    struct okv_trans *tx = NULL;
    struct okv_dbhandle *dbh = dbase->kv_dbh;
    int code;

    if (dbh == NULL) {
	code = UBADTYPE;
	goto done;
    }

    code = check_okv(okv_begin(dbh, OKV_BEGIN_RO, &tx));
    if (code != 0) {
	goto done;
    }

    code = ukv_getlabel(tx, version);
    if (code != 0) {
	goto done;
    }

 done:
    okv_abort(&tx);
    return code;
}

/* Set the ubik label for an okv_trans */
int
ukv_setlabel(struct okv_trans *tx, struct ubik_version *version)
{
    struct ubik_version64 label;
    struct ubik_version64 tmp;
    struct rx_opaque value;
    XDR xdrs;

    memset(&label, 0, sizeof(label));
    memset(&value, 0, sizeof(value));
    memset(&xdrs, 0, sizeof(xdrs));

    if (version->epoch == 0) {
	ViceLog(0, ("ubik-kv: Refusing to set invalid db version %d.%d\n",
		version->epoch, version->counter));
	return UBADTYPE;
    }

    udb_v32to64(version, &label);

    xdrmem_create(&xdrs, (void*)&tmp, sizeof(tmp), XDR_ENCODE);
    if (!xdr_ubik_version64(&xdrs, &label)) {
	ViceLog(0, ("ubik-kv: Internal error: failed to encode db label\n"));
	return UINTERNAL;
    }

    value.val = &tmp;
    value.len = sizeof(tmp);

    return check_okv(okv_put(tx, &ukv_key_label, &value, OKV_PUT_REPLACE));
}

int
ukv_setlabel_path(char *path, struct ubik_version *version)
{
    int code;
    struct okv_dbhandle *dbh = NULL;
    struct okv_trans *tx = NULL;

    code = ukv_open(path, &dbh, NULL);
    if (code != 0) {
	goto done;
    }

    code = check_okv(okv_begin(dbh, OKV_BEGIN_RW, &tx));
    if (code != 0) {
	goto done;
    }

    code = ukv_setlabel(tx, version);
    if (code != 0) {
	goto done;
    }

    code = check_okv(okv_commit(&tx));
    if (code != 0) {
	goto done;
    }

 done:
    okv_abort(&tx);
    okv_close(&dbh);
    return code;
}

int
ukv_setlabel_db(struct ubik_dbase *dbase, struct ubik_version *version)
{
    struct okv_trans *tx = NULL;
    struct okv_dbhandle *dbh = dbase->kv_dbh;
    int code;

    if (dbh == NULL) {
	code = UBADTYPE;
	goto done;
    }

    code = check_okv(okv_begin(dbh, OKV_BEGIN_RW, &tx));
    if (code != 0) {
	goto done;
    }

    code = ukv_commit(&tx, version);
    if (code != 0) {
	goto done;
    }

 done:
    okv_abort(&tx);
    return code;
}

/* Create the underlying okv transaction for a KV ubik trans. */
int
ukv_begin(struct ubik_trans *trans, struct okv_trans **a_tx)
{
    int kv_flags = 0;
    if (trans->type == UBIK_READTRANS) {
	kv_flags |= OKV_BEGIN_RO;
    } else {
	opr_Assert(trans->type == UBIK_WRITETRANS);
	kv_flags |= OKV_BEGIN_RW;
    }
    return check_okv(okv_begin(trans->kv_dbh, kv_flags, a_tx));
}

/* Commit an ubik transaction with the given version. */
int
ukv_commit(struct okv_trans **a_tx, struct ubik_version *version)
{
    int code;
    struct okv_trans *tx = *a_tx;

    if (tx == NULL) {
	return UTWOENDS;
    }

    *a_tx = NULL;

    code = ukv_setlabel(tx, version);
    if (code != 0) {
	okv_abort(&tx);
	return code;
    }

    return check_okv(okv_commit(&tx));
}

int
ukv_stat(char *path, struct ubik_stat *astat)
{
    int code;
    struct okv_dbhandle *dbh = NULL;
    struct okv_trans *tx = NULL;
    struct okv_statinfo kvstat;

    memset(&kvstat, 0, sizeof(kvstat));

    code = ukv_open(path, &dbh, NULL);
    if (code != 0) {
	goto done;
    }

    astat->kv = 1;

    code = check_okv(okv_begin(dbh, OKV_BEGIN_RO, &tx));
    if (code != 0) {
	goto done;
    }

    code = check_okv(okv_stat(tx, &kvstat));
    if (code != 0) {
	goto done;
    }

    if (kvstat.os_entries != NULL && *kvstat.os_entries > 0) {
	/*
	 * The ubik label itself uses a KV entry, so subtract one from the
	 * reported number of entries to account for that. If okv says that we
	 * have 0 entries, something probably isn't reporting the value
	 * correctly (or the db is missing a label or is otherwise messed up).
	 */
	astat->n_items = *kvstat.os_entries - 1;
    }

 done:
    okv_abort(&tx);
    okv_close(&dbh);
    return code;
}

static int
get_conf_path(char *dir_path, char **a_path)
{
    int code = 0;
    int nbytes;
    nbytes = asprintf(a_path, "%s/%s", dir_path, STORAGE_CONF_FILENAME);
    if (nbytes < 0) {
	*a_path = NULL;
	code = UNOMEM;
    }
    return code;
}

/* Create a new ubik KV database at the given path */
int
ukv_create(char *kvdir, char *okv_engine, struct okv_dbhandle **a_dbh)
{
    struct okv_dbhandle *dbh = NULL;
    char *conf_path = NULL;
    int code;
    struct okv_create_opts c_opts;
    FILE *fh = NULL;

    memset(&c_opts, 0, sizeof(c_opts));
    c_opts.engine = okv_engine;

    code = check_okv(okv_create(kvdir, &c_opts, &dbh));
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Cannot create dbase %s\n", kvdir));
	goto done;
    }

    code = get_conf_path(kvdir, &conf_path);
    if (code != 0) {
	goto done;
    }

    fh = fopen(conf_path, "ae");
    if (fh == NULL) {
	ViceLog(0, ("ubik-kv: Cannot open %s, errno=%d\n", conf_path, errno));
	code = UIOERROR;
	goto done;
    }

    if (fprintf(fh, "[ubik_db]\n") < 0 ||
	fprintf(fh, "engine = %s\n", DB_ENGINE) < 0) {
	ViceLog(0, ("ubik-kv: Cannot write to %s, errno=%d\n", conf_path, errno));
	code = UIOERROR;
	goto done;
    }

    code = fclose(fh);
    fh = NULL;
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Failed to close %s, errno=%d\n", conf_path,
		    errno));
	code = UIOERROR;
	goto done;
    }

    *a_dbh = dbh;
    dbh = NULL;

 done:
    if (fh != NULL) {
	fclose(fh);
    }

    free(conf_path);
    okv_close(&dbh);

    return code;
}

/**
 * Open a ubik KV database.
 *
 * @param[in] kvdir The path to the KV dir to open
 * @param[out] a_dbh	On success, set to the dbh for the dir
 * @param[out] version	If not NULL, on success, set to the ubik version of the
 *			database opened
 *
 * @return ubik error codes
 */
int
ukv_open(char *kvdir, struct okv_dbhandle **a_dbh, struct ubik_version *version)
{
    int code;
    const char *engine = NULL;
    char *conf_path = NULL;
    struct ubik_version version_s;
    cmd_config_section *conf = NULL;
    struct okv_dbhandle *dbh = NULL;
    struct okv_trans *tx = NULL;

    code = get_conf_path(kvdir, &conf_path);
    if (code != 0) {
	goto done;
    }

    code = cmd_RawConfigParseFile(conf_path, &conf);
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Cannot parse %s, code=%d\n", conf_path, code));
	code = UIOERROR;
	goto done;
    }

    engine = cmd_RawConfigGetString(conf, NULL, "ubik_db", "engine",
				    (char*)NULL);
    if (engine == NULL) {
	ViceLog(0, ("ubik-kv: Cannot get ubik_db engine from %s\n", conf_path));
	code = UIOERROR;
	goto done;
    }

    if (strcmp(engine, DB_ENGINE) != 0) {
	ViceLog(0, ("ubik-kv: Cannot open database %s: unknown ubik engine "
		    "%s\n", kvdir, engine));
	code = UIOERROR;
	goto done;
    }

    code = check_okv(okv_open(kvdir, &dbh));
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Cannot open okv dbase %s\n", kvdir));
	goto done;
    }

    /* Get the label, just to check if the db actually seems usable. */

    code = check_okv(okv_begin(dbh, OKV_BEGIN_RO, &tx));
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Cannot start transaction on dbase %s\n", kvdir));
	goto done;
    }

    if (version == NULL) {
	version = &version_s;
    }
    code = ukv_getlabel(tx, version);
    if (code != 0) {
	goto done;
    }

    if (a_dbh != NULL) {
	*a_dbh = dbh;
	dbh = NULL;
    }

 done:
    if (conf != NULL) {
	cmd_RawConfigFileFree(conf);
    }
    free(conf_path);
    okv_abort(&tx);
    okv_close(&dbh);

    return code;
}

/*
 * Create a readme file in our DB.d directory (if it doesn't already exist), to
 * try to avoid admins from putting their own stuff in there. Ignore errors
 * silently; if this fails, that's not the most important thing.
 */
static void
create_readme(char *parent_dir)
{
    static const char readme[] =
"This directory contains database files for OpenAFS ubik. Do NOT put your own\n"
"files in here (ubik will delete them), and don't modify or mess around with the\n"
"files in here, unless you really know what you're doing!\n";

    int fd = -1;
    char *path = NULL;
    int len = sizeof(readme)-1;
    ssize_t nbytes;

    if (asprintf(&path, "%s/README", parent_dir) < 0) {
	path = NULL;
	goto done;
    }

    if (access(path, F_OK) == 0) {
	goto done;
    }

    fd = open(path, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if (fd < 0) {
	goto done;
    }

    nbytes = write(fd, readme, len);
    if (nbytes != len) {
	goto done;
    }

 done:
    free(path);
    if (fd >= 0) {
	close(fd);
    }
}

static int
dbd_path(struct ubik_dbase *dbase, char **a_path)
{
    if (asprintf(a_path, "%s.DB.d", dbase->pathName) < 0) {
	*a_path = NULL;
	return UNOMEM;
    }
    return 0;
}

/* Try to remove the DB.d dir if we're switching away from a KV db; we
 * shouldn't need the dir anymore. */
void
ukv_cleanup_unused(struct ubik_dbase *dbase)
{
    int code;
    char *dbdotd = NULL;
    char *readme = NULL;

    code = dbd_path(dbase, &dbdotd);
    if (code != 0) {
	goto done;
    }

    if (asprintf(&readme, "%s/README", dbdotd) < 0) {
	readme = NULL;
	goto done;
    }

    (void)unlink(readme);
    (void)rmdir(dbdotd);

 done:
    free(dbdotd);
    free(readme);
}

struct cleanup_args {
    char *kvdir;    /**< The path to the actual kv db being used */
    int n_matched;  /**< How many entries in vldb.DB.d matched the dev and ino
		     *   of 'kvdir' */

    int db_stat_set;	/**< Is db_stat set? */
    struct stat db_stat;/**< stat info for 'kvdir' */
};

static int
dbdotd_cleanup_find(char *path_abs, char *path_rel, void *rock)
{
    struct cleanup_args *args = rock;
    struct stat st;
    int code = 0;

    memset(&st, 0, sizeof(st));

    if (!args->db_stat_set) {
	code = stat(args->kvdir, &args->db_stat);
	if (code != 0) {
	    ViceLog(0, ("ubik-kv: Failed to stat %s (errno %d)\n", args->kvdir,
		    errno));
	    code = UIOERROR;
	    goto done;
	}
	args->db_stat_set = 1;
    }

    code = lstat(path_abs, &st);
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Failed to lstat %s (errno %d)\n", path_abs,
		errno));
	code = UIOERROR;
	goto done;
    }

    if (st.st_dev == args->db_stat.st_dev && st.st_ino == args->db_stat.st_ino) {
	args->n_matched++;
    }

 done:
    return code;
}

static int
dbdotd_cleanup_del(char *path_abs, char *path_rel, void *rock)
{
    struct cleanup_args *args = rock;
    struct stat st;
    int code = 0;

    memset(&st, 0, sizeof(st));

    if (strcmp(path_rel, "README") == 0) {
	/* Don't delete the README. */
	goto done;
    }

    opr_Assert(args->db_stat_set);

    code = lstat(path_abs, &st);
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Failed to stat %s (errno %d)\n", path_abs,
		errno));
	code = UIOERROR;
	goto done;
    }

    if (st.st_dev == args->db_stat.st_dev && st.st_ino == args->db_stat.st_ino) {
	/* This is the .DB0 dbase; don't delete this one. */
	goto done;
    }

    ViceLog(0, ("ubik-kv: Cleaning up stale dbase %s\n", path_abs));

    /* Ignore any errors trying to delete the path, so we continue to try to
     * cleanup other files. */
    if (S_ISLNK(st.st_mode)) {
	(void)unlink(path_abs);
    } else {
	(void)udb_delpath(path_abs);
    }

 done:
    return code;
}

typedef int (*dbdotd_func)(char *path_abs, char *path_rel, void *rock);

/* For each entry in the vldb.DB.d dir, run the given callback function on it. */
static int
foreach_dbdotd(char *path, dbdotd_func func, void *rock)
{
    char *ent_path = NULL;
    DIR *dirp = NULL;
    int code = 0;

    dirp = opendir(path);
    if (dirp == NULL) {
	if (errno == ENOENT) {
	    /* Dir doesn't exist at all, so nothing to do. */
	    code = 0;
	} else {
	    ViceLog(0, ("ubik-kv: Error, cannot open %s (errno %d)\n", path,
		    errno));
	    code = UIOERROR;
	}
	goto done;
    }

    for (;;) {
	struct dirent *dent;
	char *name;

	dent = readdir(dirp);
	if (dent == NULL) {
	    /* eof */
	    break;
	}

	name = dent->d_name;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
	    /* ignore */
	    continue;
	}

	if (asprintf(&ent_path, "%s/%s", path, name) < 0) {
	    ent_path = NULL;
	    goto enomem;
	}

	code = (*func)(ent_path, name, rock);
	if (code != 0) {
	    goto done;
	}
    }

 done:
    if (dirp != NULL) {
	closedir(dirp);
    }
    free(ent_path);
    return code;

 enomem:
    code = UNOMEM;
    goto done;
}

/*
 * During startup, cleanup any stale files that might be lingering around in
 * our vldb.DB.d dir. Here's the general process:
 *
 * - Look for an entry that matches the vldb.DB0 database that we're actually
 *   using (dbdotd_cleanup_find).
 * - If we find exactly 1 match, then remove all of the other files
 *   (dbdotd_cleanup_del).
 * - If we find 0 matches, or more than 1 match, something is weird, so bail
 *   out to make sure we don't accidentally delete some actually useful data.
 */
static int
cleanup_files(struct ubik_dbase *dbase, char *kvdir)
{
    int code;
    int ret = 0;
    struct cleanup_args args;
    char *path = NULL;

    memset(&args, 0, sizeof(args));

    code = udb_del_suffixes(dbase, ".TMP", ".OLD");
    if (code != 0) {
	ret = code;
    }

    code = dbd_path(dbase, &path);
    if (code != 0) {
	ret = code;
	goto done;
    }

    args.kvdir = kvdir;
    code = foreach_dbdotd(path, dbdotd_cleanup_find, &args);
    if (code != 0) {
	ret = code;
	goto done;
    }

    if (args.n_matched != 1) {
	ViceLog(0, ("ubik-kv: Warning, could not find matching db in .DB.d dir "
		"(n_matched %d)\n", args.n_matched));
	ret = UINTERNAL;
	goto done;
    }

    code = foreach_dbdotd(path, dbdotd_cleanup_del, &args);
    if (code != 0) {
	ret = code;
    }

    create_readme(path);

 done:
    free(path);
    return ret;
}

int
ukv_init(struct ubik_dbase *dbase, int create_db)
{
    int code;
    int exists = 0;
    int isdir = 0;
    int islink = 0;
    char *kvdir = NULL;
    char *tmp_kvdir = NULL;
    struct okv_dbhandle *dbh = NULL;
    struct okv_trans *tx = NULL;
    struct ubik_version version;

    memset(&version, 0, sizeof(version));

    code = udb_path(dbase, NULL, &kvdir);
    if (code != 0) {
	goto done;
    }

    code = udb_dbinfo(kvdir, &exists, &isdir, &islink);
    if (code != 0) {
	goto done;
    }

    if (exists) {
	if (!isdir) {
	    /* A non-KV database exists; don't touch it. */
	    goto done;
	}

	if (!islink) {
	    /*
	     * Our .DB0 file is a KV dbase, but it's not a symlink (someone
	     * probably manually moved it into place). We expect KV dbases to
	     * always have a symlink for a .DB0 file, so replace it with a
	     * symlink to a .DB.d file.
	     */
	    code = ukv_db_prepinstall(dbase, kvdir);
	    if (code != 0) {
		ViceLog(0, ("ubik-kv: Failed to convert %s to a symlink (code "
			"%d)\n", kvdir, code));
		goto done;
	    }
	}

	/* KV database exists; we'll open it below. */

    } else {
	/* No database exists at all; possibly create a new one. */

	if (!create_db) {
	    /* Caller says not to create a new kv dbase, so don't do anything. */
	    goto done;
	}

	(void)udb_del_suffixes(dbase, NULL, ".NEW");
	code = udb_path(dbase, ".NEW", &tmp_kvdir);
	if (code != 0) {
	    goto done;
	}

	code = ukv_create(tmp_kvdir, NULL, &dbh);
	if (code != 0) {
	    goto done;
	}

	code = check_okv(okv_begin(dbh, OKV_BEGIN_RW, &tx));
	if (code != 0) {
	    ViceLog(0, ("ubik-kv: Error starting transaction on new dbase "
			"%s\n", tmp_kvdir));
	    goto done;
	}

	/* Set the initial version to 1.1, just like we do with flatfile
	 * databases. */
	version.epoch = 1;
	version.counter = 1;

	code = ukv_commit(&tx, &version);
	if (code != 0) {
	    ViceLog(0, ("ubik-kv: Error committing transaction on new dbase "
			"%s\n", tmp_kvdir));
	    goto done;
	}

	okv_close(&dbh);

	code = udb_install_simple(dbase, ".NEW", &version);
	if (code != 0) {
	    ViceLog(0, ("ubik-kv: Failed to install %s (code %d)\n",
			tmp_kvdir, code));
	    code = UIOERROR;
	    goto done;
	}
    }

    code = ukv_open(kvdir, &dbh, &version);
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Failed to open KV database %s, code=%d\n", kvdir,
		code));
	goto done;
    }

    opr_Assert(dbh != NULL);

    /* We opened our KV store just fine, so remove our temp files if any are
     * still lingering around. */
    code = cleanup_files(dbase, kvdir);
    if (code != 0) {
	ViceLog(0, ("ubik-kv: Failed to cleanup some stale database files. This "
		    "should not cause problems, but they may be using up some "
		    "extra disk space.\n"));
	code = 0;
    }

    ViceLog(0, ("ubik-kv: Successfully opened database %d.%d using %s (%s)\n",
		version.epoch, version.counter,
		okv_dbhandle_engine(dbh),
		okv_dbhandle_descr(dbh)));

    dbase->kv_dbh = dbh;
    dbh = NULL;

 done:
    okv_abort(&tx);
    okv_close(&dbh);
    free(kvdir);
    free(tmp_kvdir);
    return code;
}

/* Copy the entire KV database in 'src_path' to a newly-created db 'dest_path'. */
int
ukv_copydb(char *src_path, char *dest_path)
{
    int code;
    struct okv_dbhandle *src_dbh = NULL;
    struct okv_dbhandle *dest_dbh = NULL;

    code = ukv_open(src_path, &src_dbh, NULL);
    if (code != 0) {
	goto done;
    }

    code = ukv_create(dest_path, okv_dbhandle_engine(src_dbh), &dest_dbh);
    if (code != 0) {
	goto done;
    }

    code = check_okv(okv_copyall(src_dbh, dest_dbh));
    if (code != 0) {
	goto done;
    }

 done:
    okv_close(&src_dbh);
    okv_close(&dest_dbh);
    return code;
}

int
ukv_db_readlink(struct ubik_dbase *dbase, char *path_db, char **a_path)
{
    *a_path = realpath(path_db, NULL);
    if (*a_path == NULL) {
	ViceLog(0, ("ukv: Cannot get realpath for %s (errno %d)\n", path_db,
		errno));
	return UIOERROR;
    }
    return 0;
}

/*
 * Move 'path_orig' to a permanent internal path, and create a symlink from
 * 'path_orig' to the moved name. For example, if 'path_orig' is
 * /path/to/vldb.DB0.TMP, then we move it to
 * /path/to/vldb.DB.d/vldb.1234.0.DB0, and create a symlink
 * /path/to/vldb.DB0.TMP -> vldb.DB.d/vldb.1234.0.DB0.
 */
int
ukv_db_prepinstall(struct ubik_dbase *dbase, char *path_orig)
{
    afs_int64 now = time(NULL);
    char *dbdir_abs = NULL;
    char *path_abs = NULL;
    char *path_rel = NULL;
    int nbytes;
    int counter;
    int code;

    if (!ubik_KVDbase(dbase)) {
	/*
	 * Make sure .DB.d exists. To avoid doing this every single time, skip
	 * this if 'dbase' is already a KV dbase, since presumably we must have
	 * a .DB.d dir to have a KV dbase at all.
	 */
	code = dbd_path(dbase, &dbdir_abs);
	if (code != 0) {
	    goto done;
	}

	code = mkdir(dbdir_abs, 0700);
	if (code != 0 && errno != EEXIST) {
	    ViceLog(0, ("ukv: Cannot create %s (errno %d)\n", dbdir_abs, errno));
	    code = UIOERROR;
	    goto done;
	}
	if (code == 0) {
	    create_readme(dbdir_abs);
	}
	code = 0;
    }

    /*
     * Find a name inside .DB.d we can rename 'path_orig' to. Our current
     * heuristic is to make a name like 'vldb.DB.d/vldb.1234.0.DB0', where
     * '1234' is the current timestamp, and '0' is a counter. If the filename
     * somehow already exists, increment the counter and try again; give up
     * after 10000 tries. We should almost never actually collide with an
     * existing name (we'd have to be installing a db more than once per
     * second), so just a few tries should be enough.
     *
     * We could use something like mkdtemp() instead, but it's nice to have
     * names that have some kind of ordering. Using a timestamp also makes it
     * easier to see when the dir was created, maybe giving an extra clue as to
     * why a dir exists if we find some stray dirs cluttering up the .DB.d dir
     * later on.
     */
    for (counter = 0; ; counter++) {
	nbytes = asprintf(&path_abs, "%s.DB.d/%s.%lld.%d.DB0",
			  dbase->pathName, dbase->pathBase, now, counter);
	if (nbytes < 0) {
	    path_abs = NULL;
	    goto enomem;
	}

	if (access(path_abs, F_OK) != 0) {
	    break;
	}
	if (counter > 10000) {
	    ViceLog(0, ("ukv: Error, path %s somehow already exists\n",
		    path_abs));
	    code = UIOERROR;
	    goto done;
	}
    }

    nbytes = asprintf(&path_rel, "%s.DB.d/%s.%lld.%d.DB0",
		      dbase->pathBase, dbase->pathBase, now, counter);
    if (nbytes < 0) {
	path_rel = NULL;
	goto enomem;
    }

    code = okv_rename(path_orig, path_abs);
    if (code != 0) {
	ViceLog(0, ("ukv: Failed to rename %s -> %s (code %d)\n", path_orig,
		path_abs, code));
	code = UIOERROR;
	goto done;
    }

    code = symlink(path_rel, path_orig);
    if (code != 0) {
	ViceLog(0, ("ukv: Failed to symlink %s -> %s (errno %d)\n", path_orig,
		path_rel, errno));
	code = UIOERROR;
	goto done;
    }

 done:
    free(dbdir_abs);
    free(path_abs);
    free(path_rel);
    return code;

 enomem:
    code = UNOMEM;
    goto done;
}

int
ukv_senddb(char *path, struct rx_call *rxcall, struct ubik_version *version)
{
    int code;
    struct ubik_dbstream_kvitem kvitem;
    struct ubik_version disk_vers;
    struct okv_dbhandle *dbh = NULL;
    struct okv_trans *tx = NULL;
    XDR xdrs;
    int eof;

    memset(&kvitem, 0, sizeof(kvitem));
    memset(&disk_vers, 0, sizeof(disk_vers));

    code = ukv_open(path, &dbh, NULL);
    if (code != 0) {
	goto done;
    }

    code = okv_begin(dbh, OKV_BEGIN_RO, &tx);
    if (code != 0) {
	code = UIOERROR;
	goto done;
    }

    code = ukv_getlabel(tx, &disk_vers);
    if (code != 0) {
	goto done;
    }

    if (vcmp(disk_vers, *version) != 0) {
	ViceLog(0, ("ubik: Internal error: kv database version mismatch while "
		"sending db: %d.%d != %d.%d\n",
		disk_vers.epoch, disk_vers.counter,
		version->epoch, version->counter));
	code = UINTERNAL;
	goto done;
    }

    xdrrx_create(&xdrs, rxcall, XDR_ENCODE);

    eof = 0;
    while (!eof) {
	int success;

	/*
	 * Note that we use ukv_next here, not okv_next. That means we skip
	 * ubik-private keys; this function just sends the application-visible
	 * db data. Ubik-private data (such as the ubik version) is not sent here.
	 */
	code = ukv_next(tx, &kvitem.key, &kvitem.value, &eof);
	if (code != 0) {
	    code = UIOERROR;
	    goto done;
	}

	if (eof) {
	    /* For eof, just send a blank key/value item. */
	    memset(&kvitem, 0, sizeof(kvitem));
	}

	success = xdr_ubik_dbstream_kvitem(&xdrs, &kvitem);
	if (!success) {
	    code = UIOERROR;
	    goto done;
	}
    }

 done:
    okv_abort(&tx);
    okv_close(&dbh);
    return code;
}

int
ukv_recvdb(struct rx_call *rxcall, char *path, struct ubik_version *version)
{
    struct ubik_dbstream_kvitem kvitem;
    struct okv_dbhandle *dbh = NULL;
    struct okv_trans *tx = NULL;
    afs_int32 code;
    int success;
    XDR xdrs;

    memset(&kvitem, 0, sizeof(kvitem));

    code = ukv_create(path, NULL, &dbh);
    if (code != 0) {
	goto done;
    }

    code = okv_begin(dbh, OKV_BEGIN_RW, &tx);
    if (code != 0) {
	code = UIOERROR;
	goto done;
    }

    xdrrx_create(&xdrs, rxcall, XDR_DECODE);

    for (;;) {
	success = xdr_ubik_dbstream_kvitem(&xdrs, &kvitem);
	if (!success) {
	    code = UIOERROR;
	    goto done;
	}

	if (kvitem.key.len == 0 && kvitem.value.len == 0) {
	    /* EOF */
	    break;
	}

	if (check_key_app(&kvitem.key) != 0 || check_value(&kvitem.value) != 0) {
	    struct rx_opaque_stringbuf keybuf, valbuf;
	    /*
	     * We got an invalid key/value, or a ubik-private key in the stream
	     * of KV data. This shouldn't happen; this portion of the dbase
	     * stream should only contain application-visible data
	     * (ubik-private data, such as the db version, is handled
	     * elsewhere, such as in RPC arguments or the db stream header,
	     * etc). And of course, all of our keys and values should be valid.
	     */
	    ViceLog(0, ("ubik-kv: Internal error: invalid data in dbase stream "
		    "of KV data: key %s val %s.\n",
		    rx_opaque_stringify(&kvitem.key, &keybuf),
		    rx_opaque_stringify(&kvitem.value, &valbuf)));
	    code = UINTERNAL;
	    goto done;
	}

	code = okv_put(tx, &kvitem.key, &kvitem.value, OKV_PUT_BULKSORT);
	if (code != 0) {
	    code = UIOERROR;
	    goto done;
	}

	xdrfree_ubik_dbstream_kvitem(&kvitem);
    }

    code = ukv_commit(&tx, version);
    if (code != 0) {
	goto done;
    }

 done:
    xdrfree_ubik_dbstream_kvitem(&kvitem);
    okv_abort(&tx);
    okv_close(&dbh);

    return code;
}

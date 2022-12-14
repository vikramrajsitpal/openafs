/*
 * Copyright (c) 2022 Vikramraj Sitpal. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <afsconfig.h>
#include <afs/param.h>
#include <afs/afsutil.h>

#include "afs/okv.h"
#include "ri-db.h"
#include <afs/afsint.h>


#define RIDB_ENGINE "lmdb"

/* The 2 integers in this struct are in native endianness (host byte order)
 * and 'Name' may NOT be NULL terminated but ridb_get will NULL terminate it
 * when returning the value.
 */
struct ridb_key {
    afs_uint32 Vnode;
    afs_uint32 Unique;
    char Name[AFSNAMEMAX];
};


static void
user_to_ridb_key(struct AFSFid *user_key, struct ridb_key *key, char *name)
{
    key->Vnode = user_key->Vnode;
    key->Unique = user_key->Unique;

    if (strlen(name) > AFSNAMEMAX) {
	ViceLog(3,("WARNING: ridb_key:Filename '%s' too long: %lu."
		   "Truncated\n", name, strlen(name)));
    }

    strncpy(key->Name, name, AFSNAMEMAX);
}

/**
 * Create a new reverse index database
 *
 * @param[in]  dir_path	Path to a nonexistent dir that will contain the ridb
 * @param[out] a_dbh	On success, set to a context for the newly-created ridb
 *			as would be returned from 'ridb_open'.
 *
 * @return errno error codes
 * @retval EEXIST   'dir_path' already exists
 */
int
ridb_create(char *dir_path, struct okv_dbhandle **hdl)
{
    int code;
    struct okv_create_opts c_opts;

    opr_Assert(dir_path != NULL);

    ViceLog(5, ("ridb_create: Creating RIDB at '%s'\n", dir_path));

    memset(&c_opts, 0, sizeof(c_opts));
    c_opts.engine = RIDB_ENGINE;

    code = okv_create(dir_path, &c_opts, hdl);
    if (code != 0)
	ViceLog(0, ("ridb_create: BAD PATH: %s\n", dir_path));

    return code;
}


/**
 * Open a RI database handle.
 *
 * @param[in] dir_path	Path to the reverse index db directory to open.
 * @param[out] hdl	On success, set to the db handle for the requested dir.
 *
 * @return errno error codes
 * @retval ENOENT   'dir_path' does not exist
 * @retval ENOTBLK  'dir_path' does exist, but does not appear to be a valid
 *		    reverse index database
 */
int
ridb_open(char *dir_path, struct okv_dbhandle **hdl)
{
    int code;

    opr_Assert(dir_path != NULL);

    ViceLog(5, ("ridb_open: Opening RIDB at '%s'\n", dir_path));

    code = okv_open(dir_path, hdl);
    if (code != 0)
	ViceLog(0, ("ridb_open: BAD PATH: %s\n", dir_path));

    return code;
}


/**
 * Close the reverse index db.
 * Wrapper for okv_unlink()
 *
 * @param[inout] hdl	The db handle to close. If NULL, this is a no-op. 
 * 			Set to NULL on return.
 */
void
ridb_close(struct okv_dbhandle **hdl)
{
    ViceLog(5, ("ridb_close: Closing RIDB\n"));
    okv_close(hdl);
}


/**
 * Purge/delete the reverse index db from disk.
 * Wrapper for okv_unlink()
 *
 * @param[in] dir_path	Path to the okv db dir to delete.
 * 
 * @returns 0 on success, error from okv_unlink() otherwise.
 */
int
ridb_purge_db(char *dir_path)
{
    int code;

    opr_Assert(dir_path != NULL);
    ViceLog(5, ("ridb_purge_db: Purging RIDB at '%s'\n", dir_path));

    code = okv_unlink(dir_path);
    if (code != 0)
	ViceLog(0, ("ridb_purge_db: BAD PATH: %s\n", dir_path));

    return code;
}


/**
 * Read a key/value from the reverse index db.
 *
 * value is malloc'd and returned to the client. Its the client's 
 * responsibility to free it.
 *
 * @param[in]  hdl    DB Handle
 * @param[in]  key    The key to retrieve
 * @param[out] value  On success and if the key exists, set to the value
 *			          associated with the given key.
 * 
 * @returns following error code on any error, 0 on success.
 * @retval EINVAL     Missing/Invalid key
 * @retval EIO 	      Any other error
 * @retval ENOMEM     Malloc failed - No memory
 */
int
ridb_get(struct okv_dbhandle *hdl, struct AFSFid *key, char **name)
{
    struct okv_trans *txn = NULL;
    int code, eof = 0;
    struct rx_opaque dbkey, dbval;
    char *path = NULL;
    struct ridb_key rik, result_key;
    char temp_name[AFSNAMEMAX] = { 0 };

    if (hdl == NULL) {
	ViceLog(0, ("ridb_get: NULL Handle\n"));
	return EIO;
    }
    if (key == NULL) {
	ViceLog(0, ("ridb_get: NULL key\n"));
	return EIO;
    }
    if (name == NULL) {
	ViceLog(0, ("ridb_get: Nowhere to store the path\n"));
	return EIO;
    }

    *name = NULL;

    memset(&dbval, 0, sizeof(dbval));
    memset(&dbkey, 0, sizeof(dbkey));
    memset(&result_key, 0, sizeof(result_key));

    user_to_ridb_key(key, &rik, temp_name);

    dbkey.len = sizeof(rik);
    dbkey.val = &rik;

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RO, &txn);
    if (code != 0) {
	/* Handle begin txn error here */
	ViceLog(0, ("ridb_get: BAD handle\n"));
	return EIO;
    }

    code = okv_next(txn, &dbkey, &dbval, &eof);
    if (code != 0) {
	/* Handle get txn error here and abort, should not reach here */
	ViceLog(0, ("ridb_get: Internal error: TXN Abort!"));
	code = EIO;
	goto done;
    }
    
    if (eof != 0) {
	ViceLog(0, ("ridb_get: Missing Key: %u:%u\n",
		    key->Vnode, key->Unique));
	code = EINVAL;
	goto done;
    }

    if (dbkey.len != sizeof(result_key)) {
	ViceLog(0, ("ridb_get: LMDB error\n"));
	code = EIO;
	goto done;
    }
    
    if (dbval.len == 0) {
	ViceLog(0, ("ridb_get: empty value\n"));
	code = EIO;
	goto done;
    }

    memcpy(&result_key, dbkey.val, dbkey.len);

    /* There was at least one match! */
    if ((key->Vnode == result_key.Vnode) &&
	(key->Unique == result_key.Unique)) {
	path = calloc(dbval.len + 1, 1);
	if (path != NULL) {
	    memcpy(path, dbval.val, dbval.len);
	    *name = path;
	} else {
	    code = ENOMEM;
	    goto done;
	}
    } else {
	ViceLog(0, ("ridb_get: Invalid Key: %u:%u\n",
		    key->Vnode, key->Unique));
	code = EINVAL;
    }

 done:
    okv_abort(&txn);

    return code;
}


/**
 * Set a value for a given key in the reverse index db. Existing key will be
 * replaced.
 *
 *
 * @param[in]  hdl     DB Handle
 * @param[in]  key     The key to set
 * @param[in]  value   On success and if the key exists, set to the value
 *	               associated with the given key.
 * 
 * @returns EIO on any error
 */
int
ridb_set(struct okv_dbhandle *hdl, struct AFSFid *key, char *value)
{
    struct okv_trans *txn = NULL;
    int code;
    struct rx_opaque dbkey, dbval;
    struct ridb_key rik;
    size_t val_len = 0;

    if (hdl == NULL) {
	ViceLog(0, ("ridb_set: NULL Handle\n"));
	return EIO;
    }
    if (key == NULL) {
	ViceLog(0, ("ridb_set: NULL key\n"));
	return EIO;
    }
    if (value == NULL) {
	ViceLog(0, ("ridb_set: NULL value\n"));
	return EIO;
    }

    memset(&dbval, 0, sizeof(dbval));
    memset(&dbkey, 0, sizeof(dbkey));

    user_to_ridb_key(key, &rik, value);

    dbkey.len = sizeof(rik);
    dbkey.val = &rik;

    val_len = strlen(value);

    if (val_len == 0) {
	ViceLog(0, ("ridb_set: Value Length is zero (0)\n"));
	return EIO;
    }

    dbval.len = val_len;
    dbval.val = value;

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RW, &txn);
    if (code != 0) {
	/* Handle begin txn error here */
	ViceLog(0, ("ridb_set: Bad handle\n"));
	return EIO;
    }

    code = okv_put(txn, &dbkey, &dbval, OKV_PUT_REPLACE);
    if (code != 0) {
	/* Handle get txn error here and abort */
	okv_abort(&txn);
	ViceLog(0, ("ridb_set: Bad key\n"));
	return EIO;
    }

    /* Commit */
    code = okv_commit(&txn);
    if (code != 0) {
	ViceLog(0, ("ridb_set: Internal error occurred: %d\n", code));
	return EIO;
    }

    return code;
}


/**
 * Delete a given key in the reverse index db.
 *
 * @param[in]  hdl     DB Handle
 * @param[in]  key     The key to delete
 * @param[in]  name    Name of the entry to be deleted (added for links)
 * 
 * @returns EIO on any error
 */
int
ridb_del(struct okv_dbhandle *hdl, struct AFSFid *key, char *name)
{
    struct okv_trans *txn = NULL;
    int code;
    struct rx_opaque dbkey;
    struct ridb_key rik;

    if (hdl == NULL) {
	ViceLog(0, ("ridb_del: NULL Handle\n"));
	return EIO;
    }
    if (key == NULL) {
	ViceLog(0, ("ridb_del: NULL key\n"));
	return EIO;
    }

    if (name == NULL) {
	ViceLog(0, ("ridb_del: NULL name\n"));
	return EIO;
    }

    memset(&dbkey, 0, sizeof(dbkey));

    user_to_ridb_key(key, &rik, name);

    dbkey.len = sizeof(struct ridb_key);
    dbkey.val = &rik;

    if (strlen(name) == 0) {
	ViceLog(0, ("ridb_del: Value Length is zero (0)\n"));
	return EIO;
    }

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RW, &txn);
    if (code != 0) {
	/* Handle begin txn error here */
	ViceLog(0, ("ridb_del: Bad handle\n"));
	return EIO;
    }

    /* Delete key */
    code = okv_del(txn, &dbkey, NULL);
    if (code != 0) {
	/* Handle get txn error here and abort */
	okv_abort(&txn);
	ViceLog(0, ("ridb_del: Bad key\n"));
	return EIO;
    }

    /* Commit */
    code = okv_commit(&txn);
    if (code != 0) {
	ViceLog(0, ("ridb_set: Internal error occurred: %d\n", code));
	return EIO;
    }

    return code;
}

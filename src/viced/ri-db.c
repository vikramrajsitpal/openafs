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

#include "okv/okv.h"
#include "ri-db.h"

/* Just for ignoring the 'Volume' field in AFSFid */
struct ridb_key {
    afs_uint32 Vnode;
	afs_uint32 Unique;
};

static int
ridb_get_vol_rel_path(struct AFSFid* key, char** path) {

}

int
ridb_create(const char* dir_path, struct okv_dbhandle** hdl) {
    
    int retcode = 0;
    opr_Assert(dir_path != NULL);
    retcode = okv_create(dir_path, NULL, hdl);

    if (retcode)
        return RIDB_BAD_PATH;

    return RIDB_SUCCESS;
}

int
ridb_open(const char* dir_path, struct okv_dbhandle** hdl) {
    int retcode = 0;
    opr_Assert(dir_path != NULL);

    retcode = okv_open(dir_path, hdl);

    if (retcode)
        return RIDB_BAD_PATH;
    
    return RIDB_SUCCESS;
}

void
ridb_close(struct db_handle** hdl) {
    okv_close(hdl);
}

int
ridb_purge_db(char* dir_path) {
    
    int retcode = 0;
    opr_Assert(dir_path != NULL);

    retcode = okv_unlink(dir_path);

    if (retcode)
        return RIDB_BAD_PATH;
    
    return RIDB_SUCCESS;
}


/*
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
 * @returns errno error codes
 * @retval RIDB_BAD_KEY   The given key does not exist or is invalid
 * @retval RIDB_BAD_HDL   The given handle is not valid
 * @retval RIDB_BAD_VAL   The value arg is NULL or Database returned a 0 length
 *                        value.
 */

int
ridb_get(struct okv_dbhandle* hdl, struct AFSFid* key, char** value) {
    
    struct okv_trans *txn = NULL;
    int code;
    struct rx_opaque dbkey, dbval;
    char *path = NULL;

    if (!hdl)
        return RIDB_BAD_HDL;
    if (!key)
        return RIDB_BAD_KEY;
    if (!value)
        return RIDB_BAD_VAL;

    memset(&dbval, 0, sizeof(dbval));
    memset(&dbkey, 0, sizeof(dbkey));

    struct ridb_key rik = {
        .Vnode = key->Vnode,
        .Unique = key->Unique,
    };

    dbkey.len = sizeof(struct ridb_key);
    dbkey.val = (void *) (&rik);

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RO, &txn);
    if (code != 0)
    {
    /* Handle begin txn error here */
    return RIDB_BAD_HDL;
    }

    code = okv_get(txn, &dbkey, &dbval, NULL);
    if (code != 0) {
    /* Handle get txn error here and abort */
    okv_abort(&txn);
    return RIDB_BAD_KEY;
    }
    
    /* Commit */
    code = okv_commit(&txn);
    if( dbval.len == 0 ) {
    return RIDB_BAD_VAL;
    }

    path = (char *) calloc(dbval.len, sizeof(char)); 
    memcpy(path, dbval.val, dbval.len);
    *value = path;

    return RIDB_SUCCESS;
}


/*
 * Set a value for a given key in the reverse index db. Existing key will be
 * replaced.
 *
 *
 * @param[in]  hdl     DB Handle
 * @param[in]  key     The key to set
 * @param[in]  value   On success and if the key exists, set to the value
 *			           associated with the given key.
 * @param[in]  val_len Length of 'value' buffer.
 * 
 * @returns errno error codes
 * @retval RIDB_BAD_KEY   The given key does not exist or is invalid
 * @retval RIDB_BAD_HDL   The given handle is not valid
 * @retval RIDB_BAD_VAL   The value arg is NULL or is of 0 length
 */


int
ridb_set(struct okv_dbhandle* hdl, struct AFSFid* key, char* value,
         size_t val_len){
    
    struct okv_trans *txn = NULL;
    int code;
    struct rx_opaque dbkey, dbval;
    char *path = NULL;

    if (!hdl)
        return RIDB_BAD_HDL;
    if (!key)
        return RIDB_BAD_KEY;
    if (!value)
        return RIDB_BAD_VAL;

    memset(&dbval, 0, sizeof(dbval));
    memset(&dbkey, 0, sizeof(dbkey));

    struct ridb_key rik = {
        .Vnode = key->Vnode,
        .Unique = key->Unique,
    };

    dbkey.len = sizeof(struct ridb_key);
    dbkey.val = (void *) (&rik);

    if ( val_len == 0 ) {
    return RIDB_BAD_VAL;
    }

    dbval.len = val_len;
    dbval.val = (void *) value;

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RW, &txn);
    if (code != 0)
    {
    /* Handle begin txn error here */
    return RIDB_BAD_HDL;
    }

    code = okv_put(txn, &dbkey, &dbval, OKV_PUT_REPLACE);
    if (code != 0) {
    /* Handle get txn error here and abort */
    okv_abort(&txn);
    return RIDB_BAD_KEY;
    }

    /* Commit */
    code = okv_commit(&txn);

    if( dbval.len == 0 ) {
    return RIDB_BAD_VAL;
    }

    return RIDB_SUCCESS;
}


/*
 * Delete a given key in the reverse index db.
 *
 * @param[in]  hdl     DB Handle
 * @param[in]  key     The key to delete
 
 * 
 * @returns errno error codes
 * @retval RIDB_BAD_KEY   The given key does not exist or is invalid
 * @retval RIDB_BAD_HDL   The given handle is not valid
 */
int
ridb_del(struct okv_dbhandle* hdl, struct AFSFid* key) {

    struct okv_trans *txn = NULL;
    int code;
    struct rx_opaque dbkey;
    char *path = NULL;

    if (!hdl)
        return RIDB_BAD_HDL;
    if (!key)
        return RIDB_BAD_KEY;
    
    memset(&dbkey, 0, sizeof(dbkey));

    struct ridb_key rik = {
        .Vnode = key->Vnode,
        .Unique = key->Unique,
    };

    dbkey.len = sizeof(struct ridb_key);
    dbkey.val = (void *) (&rik);

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RW, &txn);
    if (code != 0)
    {
    /* Handle begin txn error here */
    return RIDB_BAD_HDL;
    }
   
    /* Delete key */
    code = okv_del(txn, &dbkey, NULL);

    if (code != 0) {
    /* Handle get txn error here and abort */
    okv_abort(&txn);
    return RIDB_BAD_KEY;
    }

    /* Commit */
    code = okv_commit(&txn);

    return RIDB_SUCCESS;
}
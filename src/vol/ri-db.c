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


#define RIDB_log(str) ViceLog(0,str)
#define RIDB_ENGINE "lmdb\0"


struct ridb_key {
    afs_uint32 Vnode;
    afs_uint32 Unique;
    char Name[AFSNAMEMAX];
};


static void user_to_ridb_key(struct AFSFid *user_key, struct ridb_key * key, 
                            char *name) {

    key->Vnode = user_key->Vnode;
    key->Unique = user_key->Unique;

    if (strlen(name) > AFSNAMEMAX) {
    RIDB_log(("ridb_key: Filename too long: %lu\n", strlen(name)));
    }

    strncpy(key->Name, name, AFSNAMEMAX);
}

int
ridb_create(char* dir_path, struct okv_dbhandle** hdl) {
    int retcode = 0;
    struct okv_create_opts c_opts;

    opr_Assert(dir_path != NULL);

    RIDB_log(("ridb_create: Creating RIDB at '%s'\n", dir_path));

    memset(&c_opts, 0, sizeof(c_opts));
    c_opts.engine = RIDB_ENGINE;

    retcode = okv_create(dir_path, &c_opts, hdl);

    if (retcode)
        RIDB_log(("ridb_create: BAD PATH: %s\n",dir_path));

    return retcode;
}

int
ridb_open(char* dir_path, struct okv_dbhandle** hdl) {
    int retcode = 0;
    opr_Assert(dir_path != NULL);

    RIDB_log(("ridb_open: Opening RIDB at '%s'\n", dir_path));

    retcode = okv_open(dir_path, hdl);

    if (retcode)
        RIDB_log(("ridb_open: BAD PATH: %s\n",dir_path));
    
    return retcode;
}

void
ridb_close(struct okv_dbhandle** hdl) {
    RIDB_log(("ridb_close: Closing RIDB\n"));
    okv_close(hdl);
}

int
ridb_purge_db(char* dir_path) {
    
    int retcode = 0;
    opr_Assert(dir_path != NULL);
    RIDB_log(("ridb_purge_db: Purging RIDB at '%s'\n", dir_path));
    
    retcode = okv_unlink(dir_path);

    if (retcode)
        RIDB_log(("ridb_purge_db: BAD PATH: %s\n",dir_path));
    
    return retcode;
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
ridb_get(struct okv_dbhandle* hdl, struct AFSFid* key, char** name) {
    
    struct okv_trans *txn = NULL;
    int code, eof = 0;
    struct rx_opaque dbkey, dbval;
    char *path = NULL;
    struct ridb_key rik;
    char temp_name[AFSNAMEMAX] = {0};

    if (!hdl) {
    RIDB_log(("ridb_get: NULL Handle\n"));
    return EIO;
    }
    if (!key) {
    RIDB_log(("ridb_get: NULL key\n"));
    return EIO;
    }
    if (!name) {
    RIDB_log(("ridb_get: Nowhere to store the path\n"));
    return EIO;
    }

    memset(&dbval, 0, sizeof(dbval));
    memset(&dbkey, 0, sizeof(dbkey));

    user_to_ridb_key(key, &rik, temp_name);

    dbkey.len = sizeof(struct ridb_key);
    dbkey.val = &rik;

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RO, &txn);
    if (code != 0) {
    /* Handle begin txn error here */
    RIDB_log(("ridb_get: BAD handle\n"));
    return EIO;
    }
    
    code = okv_next(txn, &dbkey, &dbval, &eof);
    if (code != 0) {
    /* Handle get txn error here and abort, should not reach here */
    RIDB_log(("ridb_get: Internal error: TXN Abort!"));
    okv_abort(&txn);
    return EIO;
    }

    /* There was at least one match! */
    if ((!eof) &&
        (key->Vnode == ((struct ridb_key *)dbkey.val)->Vnode) &&
        (key->Unique == ((struct ridb_key *)dbkey.val)->Unique)
        ) {
    path = (char *) calloc(dbval.len+1, sizeof(char)); 
    if (path)
        memcpy(path, dbval.val, dbval.len);
    
    *name = path;
    }
    else {
    RIDB_log(("ridb_get: Invalid Key: %u:%u\n", key->Vnode, key->Unique));
    *name = NULL;
    }

    /* Commit */
    code = okv_commit(&txn);

    if (code) {
    RIDB_log(("ridb_get: Internal error occurred: %d\n", code));
    return EIO;
    }
    
   if (NULL == (*name))
      code = EINVAL;

   return code;
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
 * 
 * @returns errno error codes
 * @retval RIDB_BAD_KEY   The given key does not exist or is invalid
 * @retval RIDB_BAD_HDL   The given handle is not valid
 * @retval RIDB_BAD_VAL   The value arg is NULL or is of 0 length
 */


int
ridb_set(struct okv_dbhandle* hdl, struct AFSFid* key, char* value){
    
    struct okv_trans *txn = NULL;
    int code;
    struct rx_opaque dbkey, dbval;
    struct ridb_key rik;
    size_t val_len = 0;

    if (!hdl) {
    RIDB_log(("ridb_set: NULL Handle\n"));
    return EIO;
    }
    if (!key) {
    RIDB_log(("ridb_set: NULL key\n"));
    return EIO;
    }
    if (!value) {
    RIDB_log(("ridb_set: NULL value\n"));
    return EIO;
    }

    memset(&dbval, 0, sizeof(dbval));
    memset(&dbkey, 0, sizeof(dbkey));

    user_to_ridb_key(key, &rik, value);

    dbkey.len = sizeof(struct ridb_key);
    dbkey.val = &rik;

    val_len = strlen(value);

    if ( val_len == 0 ) {
    RIDB_log(("ridb_set: Value Length is zero (0)\n"));
    return EIO;
    }

    dbval.len = val_len;
    dbval.val = value;

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RW, &txn);
    if (code != 0)
    {
    /* Handle begin txn error here */
    RIDB_log(("ridb_set: Bad handle\n"));
    return EIO;
    }

    code = okv_put(txn, &dbkey, &dbval, OKV_PUT_REPLACE);
    if (code != 0) {
    /* Handle get txn error here and abort */
    okv_abort(&txn);
    RIDB_log(("ridb_set: Bad key\n"));
    return EIO;
    }

    /* Commit */
    code = okv_commit(&txn);

    if (code) {
    RIDB_log(("ridb_set: Internal error occurred: %d\n", code));
    return EIO;
    }

    return code;
}


/*
 * Delete a given key in the reverse index db.
 *
 * @param[in]  hdl     DB Handle
 * @param[in]  key     The key to delete
 * @param[in]  name    Name of the entry to be deleted (added for links)
 * 
 * @returns EIO on any error
 */
int
ridb_del(struct okv_dbhandle* hdl, struct AFSFid* key, char *name) {

    struct okv_trans *txn = NULL;
    int code;
    struct rx_opaque dbkey;
    struct ridb_key rik;

    if (!hdl) {
    RIDB_log(("ridb_del: NULL Handle\n"));
    return EIO;
    }
    if (!key) {
    RIDB_log(("ridb_del: NULL key\n"));
    return EIO;
    }

    if (!name) {
    RIDB_log(("ridb_del: NULL name\n"));
    return EIO;
    }

    
    memset(&dbkey, 0, sizeof(dbkey));
    
    user_to_ridb_key(key, &rik, name);

    dbkey.len = sizeof(struct ridb_key);
    dbkey.val = &rik;

    if ( strlen(name) == 0 ) {
    RIDB_log(("ridb_del: Value Length is zero (0)\n"));
    return EIO;
    }

    /* Start txn */
    code = okv_begin(hdl, OKV_BEGIN_RW, &txn);
    if (code != 0)
    {
    /* Handle begin txn error here */
    RIDB_log(("ridb_del: Bad handle\n"));
    return EIO;
    }
   
    /* Delete key */
    code = okv_del(txn, &dbkey, NULL);

    if (code != 0) {
    /* Handle get txn error here and abort */
    okv_abort(&txn);
    RIDB_log(("ridb_del: Bad key\n"));
    return EIO;
    }

    /* Commit */
    code = okv_commit(&txn);

    if (code) {
    RIDB_log(("ridb_set: Internal error occurred: %d\n", code));
    return EIO;
    }

    return code;
}

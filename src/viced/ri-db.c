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


int
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

int
ridb_get(struct okv_dbhandle* hdl, struct AFSFid* key, char** value) {
    
}

int
ridb_set(struct okv_dbhandle* hdl, struct AFSFid* key, char* value) {

}

int
ridb_del(struct okv_dbhandle* hdl, struct AFSFid* key, char** del_value) {

}
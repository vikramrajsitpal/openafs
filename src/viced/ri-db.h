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

#ifndef AFS_SRC_VICED_RIDB_H
#define AFS_SRC_VICED_RIDB_H

#include <afs/afsint.h>

struct okv_dbhandle;

/* ERROR CODES - FOR INTERNAL PURPOSES ONLY */

enum ridb_status_code {
RIDB_SUCCESS = 0,    
RIDB_BAD_KEY,
RIDB_BAD_VAL,
RIDB_BAD_HDL,
RIDB_BAD_PATH,
RIDB_BAD_OPTS,
RIDB_ALREADY_OPEN
};

/* PROTOTYPES */

int ridb_create(const char *dir_path, struct okv_dbhandle** hdl);

int ridb_open(const char *dir_path, struct okv_dbhandle** hdl);

void ridb_close(struct db_handle** hdl);

int ridb_purge_db(char* dir_path);

int ridb_get(struct okv_dbhandle* hdl, struct AFSFid* key, char** value);

int ridb_set(struct okv_dbhandle* hdl, struct AFSFid* key, char* value);

int ridb_del(struct okv_dbhandle* hdl, struct AFSFid* key, char** del_value);



#endif

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

struct okv_dbhandle;
struct AFSFid;

typedef struct {
    char **names;
    size_t names_len;
} results;

/* PROTOTYPES */

int ridb_create(char *dir_path, struct okv_dbhandle **hdl);

int ridb_open(char *dir_path, struct okv_dbhandle **hdl);

void ridb_close(struct okv_dbhandle **hdl);

int ridb_purge_db(char *dir_path);

int ridb_get(struct okv_dbhandle *hdl, struct AFSFid *key, char **path);

int ridb_set(struct okv_dbhandle *hdl, struct AFSFid *key, char *value);

int ridb_del(struct okv_dbhandle *hdl, struct AFSFid *key, char *name);



#endif

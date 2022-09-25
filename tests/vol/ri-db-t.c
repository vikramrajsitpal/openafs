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
#include "src/vol/ri-db.h"
#include "common.h"

static char *prefix;
static char *dbdir;

struct AFSFid {
	afs_uint32 Volume;
	afs_uint32 Vnode;
	afs_uint32 Unique;
};

#define KEY_FID(vol, vnode, vunique) { \
    .Volume = vol,                     \
    .Vnode = vnode,                    \
    .Unique = vunique                  \
}


/* Prototypes */

void test1(void);
void test2(void);
void test3(void);
void test4(void);

/* Basic get-set-del tests */
void
test1(void)
{   
    int code;
    struct okv_dbhandle *dbh = NULL;
    char *name = NULL;

    struct AFSFid k1 = KEY_FID(1, 2, 2);
    struct AFSFid k2 = KEY_FID(1, 2, 4);

    code = ridb_open(dbdir, &dbh);
    is_int(code, 0, "test1: ridb_open");
    if (code)
        sysbail("test1 ridb_open failed: %d", code);

    code = ridb_set(dbh, &k1, "key1");
    is_int(code, 0, "test1: ridb_set key1");

    code = ridb_set(dbh, &k2, "key2");
    is_int(code, 0, "test1: ridb_set key2");

    code = ridb_get(dbh, &k1, &name);
    is_int(code, 0, "test1: ridb_get key1");
    is_string("key1", name, "test1:name check key1");
    free(name);

    code = ridb_get(dbh, &k2, &name);
    is_int(code, 0, "test1: ridb_get key2");
    is_string("key2", name, "test1:name check key2");
    free(name);

    code = ridb_del(dbh, &k1, "key1");
    is_int (code, 0, "test1: ridb_del key1");

    code = ridb_get(dbh, &k1, &name);
    is_string(name, NULL, "test1: ridb_get confirm key1 is deleted");

    code = ridb_del(dbh, &k2, "key2");
    is_int (code, 0, "test1: ridb_del key2");

    code = ridb_get(dbh, &k2, &name);
    is_string(name, NULL, "test1: ridb_get confirm key2 is deleted");

    ridb_close(&dbh);

}


/* Link get-set-del tests */
void
test2(void)
{
    int code;
    struct okv_dbhandle *dbh = NULL;
    char *name = NULL;

    struct AFSFid k1 = KEY_FID(1, 2, 2);
    struct AFSFid k2 = KEY_FID(1, 2, 4);


    code = ridb_open(dbdir, &dbh);
    is_int(code, 0, "test2: ridb_open");
    if (code)
        sysbail("test2 ridb_open failed: %d", code);

    code = ridb_set(dbh, &k1, "key1");
    is_int(code, 0, "test2: ridb_set key1");

    code = ridb_set(dbh, &k2, "key2");
    is_int(code, 0, "test2: ridb_set key2");

    code = ridb_set(dbh, &k1, "key3");
    is_int(code, 0, "test2: ridb_set key3");

    code = ridb_get(dbh, &k2, &name);
    is_int(code, 0, "test2: ridb_get key2");
    is_string("key2", name, "test2: name check key2");
    free(name);

    code = ridb_get(dbh, &k1, &name);
    is_int(code, 0, "test2: ridb_get ");
    is_string("key1", name, "test2: name check key1");
    free(name);

    code = ridb_del(dbh, &k1, "key1");
    is_int (code, 0, "test2: ridb_del key1");

    code = ridb_get(dbh, &k1, &name);
    is_int(code, 0, "test2: ridb_get key3");
    is_string("key3", name, "test2: name check key3");
    free(name);
    
    code = ridb_del(dbh, &k1, "key3");
    is_int (code, 0, "test2: ridb_del key3");

    code = ridb_del(dbh, &k2, "key2");
    is_int (code, 0, "test2: ridb_del key2");


    ridb_close(&dbh);

}

/* Overwrite key value tests */
void
test3(void)
{
    int code;
    struct okv_dbhandle *dbh = NULL;
    char *name = NULL;

    struct AFSFid k1 = KEY_FID(1, 2, 2);
    code = ridb_open(dbdir, &dbh);
    is_int(code, 0, "test3: ridb_open");
    if (code)
        sysbail("test3 ridb_open failed: %d", code);

    code = ridb_set(dbh, &k1, "key1");
    is_int(code, 0, "test3: ridb_set key1");

    code = ridb_get(dbh, &k1, &name);
    is_int(code, 0, "test3: ridb_get key1");
    is_string(name, "key1", "test3: name check key1");
    free(name);

    code = ridb_del(dbh, &k1, "key1");
    is_int (code, 0, "test3: ridb_del key1");

    code = ridb_set(dbh, &k1, "key4");
    is_int(code, 0, "test3: ridb_set key4");

    code = ridb_get(dbh, &k1, &name);
    is_int(code, 0, "test3: ridb_get key4");
    is_string("key4", name, "test3:name check key4");
    free(name);

    ridb_close(&dbh);

}

/* Invalid get-set-del tests */
void
test4(void)
{
    int code;
    struct okv_dbhandle *dbh = NULL;
    char *name = NULL;
    struct AFSFid k1 = KEY_FID(1, 2, 2);
    struct AFSFid k2 = KEY_FID(1, 2, 5);

    code = ridb_purge_db(dbdir);
    is_int(code, 0, "test4: ridb_purge_db");
    if (code)
        sysbail("test4: purge failed");

    code = ridb_create(dbdir, &dbh);
    is_int(code, 0, "test4: ridb_create");
     if (code)
        sysbail("test4: create failed");

    code = ridb_open(dbdir, &dbh);
    is_int(code, 0, "test4: ridb_open");
    if (code)
        sysbail("test4 ridb_open failed: %d", code);

    code = ridb_get(dbh, &k1, &name);
    is_int(code, EINVAL, "test4: ridb_get key1");
    is_string(NULL, name, "test4:name check key1");

    code = ridb_set(dbh, &k1, NULL);
    is_int(code, EIO, "test4: ridb_set key1 NULL name");

    code = ridb_set(NULL, &k1, "key1");
    is_int(code, EIO, "test4: ridb_set key1 NULL handle");

    code = ridb_set(dbh, NULL, "key1");
    is_int(code, EIO, "test4: ridb_set key1 NULL key");

    code = ridb_get(dbh, &k1, NULL);
    is_int(code, EIO, "test4: ridb_get key1 NULL name");
    
    code = ridb_del(dbh, &k1, NULL);
    is_int(code, EIO, "test4: ridb_del key1 NULL name");
    
    code = ridb_del(dbh, &k2, "haha");
    is_int(code, EIO, "test4: ridb_del key2 invalid key");
    
    ridb_close(&dbh);

    code = ridb_del(dbh, &k2, "haha");
    is_int(code, EIO, "test4: ridb_del key2 INVALID handle");

}


int
main(void)
{
    struct okv_dbhandle *dbh = NULL;
    int code;

    prefix = afstest_mkdtemp();
    opr_Assert(prefix != NULL);

    dbdir = afstest_asprintf("%s/dbase", prefix);

    code = ridb_open(dbdir, &dbh);
    is_int(ENOENT, code, "ridb_open fails with ENOENT");

    code = ridb_create(dbdir, &dbh);
    is_int(code, 0, "ridb_create");

    ridb_close(&dbh);

    test1();
    test2();
    test3();
    test4();

    code = ridb_purge_db(dbdir);
    is_int(code, 0, "ridb_purge_db");
    
    afstest_rmdtemp(prefix);

    return 0;
}

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
#include <afs/volume.h>
#include "common.h"

static char *prefix;
static char *dbdir;

#define KEY_FID(vol, vnode, vunique) { \
    .Volume = vol,                     \
    .Vnode = vnode,                    \
    .Unique = vunique                  \
}

/* Basic get-set-del tests */
void
test1()
{
    struct AFSFid k1 = KEY_FID(1, 2, 2);
    struct AFSFid k2 = KEY_FID(1, 2, 4);
}


/* Link get-set-del tests */
void
test2()
{


}

/* Overwrite key value tests */
void
test3()
{

}

/* Invalid get-set-del tests */
void
test4()
{

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
    ok(code == 0, "ridb_create successful");



    return 0;
}

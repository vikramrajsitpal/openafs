/*
 * Copyright (c) 2021 Sine Nomine Associates
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

#include "prtest.h"

static struct frztest_ops prdb_ops = {
    .suite = "pt",
    .freeze_envname = "PT",

    .use_db = "prdb0",
    .blankdb = { .flat_path = "tests/ptserver/db.blank/prdb.DB0" },
};

int
main(int argc, char **argv)
{
    char *pts = NULL;

    afstest_SkipTestsIfNoCtl();
    prtest_init(argv);

    plan(316);

    pts = afstest_obj_path("src/ptserver/pts");
    prdb_ops.blank_cmd =
	afstest_asprintf("%s listentries -users -groups -localauth", pts);

    prdb_ops.blank_cmd_stdout =
	"Name                          ID  Owner Creator\n"
	"system:administrators       -204   -204    -204 \n"
	"system:backup               -205   -204    -204 \n"
	"system:anyuser              -101   -204    -204 \n"
	"system:authuser             -102   -204    -204 \n"
	"system:ptsviewers           -203   -204    -204 \n"
	"anonymous                  32766   -204    -204 \n";

    frztest_runtests(&prtiny, &prdb_ops);

    return 0;
}

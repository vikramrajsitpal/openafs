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

#include "vltest.h"

static struct frztest_ops vldb4_ops = {
    .suite = "vl",
    .freeze_envname = "VL",

    .use_db = "vldb4",
    .blankdb = { .flat_path = "tests/vlserver/db.blank/vldb4.DB0", },
};

int
main(int argc, char **argv)
{
    char *vos = NULL;

    afstest_SkipTestsIfNoCtl();
    vltest_init(argv);

    plan(246);

    vos = afstest_obj_path("src/volser/vos");

    vldb4_ops.blank_cmd =
	afstest_asprintf("%s listvldb -noresolv -noauth -nosort", vos);

    vldb4_ops.blank_cmd_stdout =
	"VLDB entries for all servers \n\nTotal entries: 0\n";

    frztest_runtests(&vlsmall, &vldb4_ops);

    return 0;
}

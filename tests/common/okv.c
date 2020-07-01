/*
 * Copyright (c) 2020 Sine Nomine Associates. All rights reserved.
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

#include <roken.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <tests/tap/basic.h>

#include <afs/opr.h>

#include "common.h"

#ifdef WORDS_BIGENDIAN
# define ENDIAN_STR "be"
#else
# define ENDIAN_STR "le"
#endif

/*!
 * Common okv-related functions for testing programs
 */

/**
 * Get the path to an in-tree okv db.
 *
 * Some database formats are not platform-agnostic, so we have some logic
 * in here to 'search' for a db that matches our platform. For a database
 * called e.g. "test.db", the following must exist:
 *
 * - test.db (just a blank file)
 * - test.db.<suffix> (for the actual db data)
 *
 * Current suffixes we try are, for example:
 *
 * - test.db.le64 (little-endian 64-bit)
 * - test.db.generic
 *
 * If we cannot find a database with a matching suffix, then we don't have a
 * database for the current platform, so the caller is expected to skip that
 * set of tests.
 *
 * The suffix-less database file must exist so we can make sure our paths
 * are set correctly. Otherwise, an error with the paths or giving a bad db
 * name would cause us to just skip the tests, instead of flagging an
 * error.
 *
 * If a database format is platform-agnostic, then use the fallback
 * ".generic" suffix.
 *
 * If AFSTEST_OKV_NODB is set in the environment, we skip checking all suffxes
 * (except for .generic), in order to allow for easier testing of what happens
 * if the given db is not available for a platform.
 *
 * @param[out] a_path	Set to the absolute path of the db to use, or NULL if
 *			no db is suitable for this platform.
 * @param[in] filename	Path to the non-suffixed db file ("test.db" in the
 *			example above). This is relative to the top of the
 *			source tree, e.g.  "tests/okv/foo.db".
 * @return errno error codes. If the given database does not exist for this
 * platform, 0 is returned, and *a_path is NULL.
 */
int
afstest_okv_dbpath(char **a_path, char *filename)
{
    char *prefix = NULL;
    char *endian_bits = NULL;
    char **suffix;
    char *suffixes[3];
    int code;

    *a_path = NULL;
    memset(suffixes, 0, sizeof(suffixes));

    prefix = afstest_src_path(filename);

    code = access(prefix, F_OK);
    if (code != 0) {
	code = errno;
	goto done;
    }

    endian_bits = afstest_asprintf("%s%d", ENDIAN_STR, (int)sizeof(void*)*8);

    suffixes[0] = endian_bits;
    suffixes[1] = ".generic";
    suffixes[2] = NULL;

    suffix = suffixes;
    if (getenv("AFSTEST_OKV_NODB") != NULL) {
	/*
	 * If this env var is set, we skip over the platform-specific db. We
	 * may still have a db (if there's a .generic file), or this may mean
	 * we don't match any db.
	 */
	suffix++;
    }

    for (; suffix[0] != NULL; suffix++) {
	*a_path = afstest_asprintf("%s.%s", prefix, suffix[0]);
	if (access(*a_path, F_OK) == 0) {
	    break;
	} else if (errno == ENOENT) {
	    code = 0;
	} else {
	    code = errno;
	}

	free(*a_path);
	*a_path = NULL;

	if (code != 0) {
	    goto done;
	}
    }

 done:
    free(endian_bits);
    free(prefix);
    return code;
}

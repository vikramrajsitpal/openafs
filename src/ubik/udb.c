/*
 * Copyright (c) 2020 Sine Nomine Associates
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

#include <roken.h>

#include <afs/opr.h>
#include <opr/time64.h>
#include <afs/afsutil.h>
#include "ubik_internal.h"

void
udb_v32to64(struct ubik_version *from, struct ubik_version64 *to)
{
    /* opr_time64_fromSecs cannot fail here, since we're converting from a
     * 32-bit time value. */
    opr_Verify(opr_time64_fromSecs(from->epoch, &to->epoch64) == 0);
    to->counter64 = from->counter;
}

int
udb_vcmp64(struct ubik_version64 *vers_a, struct ubik_version64 *vers_b)
{
    int res;
    res = opr_time64_cmp(&vers_a->epoch64, &vers_b->epoch64);
    if (res != 0) {
	return res;
    }
    if (vers_a->counter64 > vers_b->counter64) {
	return 1;
    }
    if (vers_a->counter64 < vers_b->counter64) {
	return -1;
    }
    return 0;
}

/**
 * Calculate the path on disk for the given databse file.
 *
 * @param[in] dbase ubik db
 * @param[in] suffix    db suffix (e.g. ".TMP" for "vldb.DB0.TMP")
 * @param[out] apath    On success, set to the full path name of the db. Must
 *                      be freed by the caller.
 * @return ubik error codes
 */
int
udb_path(struct ubik_dbase *dbase, char *suffix, char **apath)
{
    int nbytes;
    int file = 0;

    if (suffix == NULL) {
	suffix = "";

    } else if (strchr(suffix, '/') != NULL) {
	/* Make sure a caller doesn't accidentally specify a suffix that
	 * possibly escapes to some other dir. */
	ViceLog(0, ("ubik: Refusing to use dbase suffix '%s'\n", suffix));
	return UINTERNAL;
    }

    nbytes = asprintf(apath, "%s.DB%d%s", dbase->pathName, file, suffix);
    if (nbytes < 0) {
	*apath = NULL;
	return UNOMEM;
    }
    return 0;
}

int
udb_delpath(char *path)
{
    int code;
    code = unlink(path);
    if (code != 0 && errno == ENOENT) {
	code = 0;
    }
    if (code != 0) {
	ViceLog(0, ("ubik: Failed to unlink %s, errno=%d\n", path, errno));
	return UIOERROR;
    }
    return 0;
}

/**
 * Prepare a dbase file for installation with udb_install_finish. For now, this
 * just checks that the db version matches 'vers_new'.
 *
 * @param[in] dbase	    The ubik database
 * @param[in] suffix_new    The suffix of the dbase file getting installed
 *			    (e.g. ".TMP")
 * @param[in] vers_new	    The version of the dbase file getting installed
 * @return ubik error codes
 */
static int
udb_install_prep(struct ubik_dbase *dbase, char *suffix_new,
		 struct ubik_version *vers_new)
{
    int code;
    struct ubik_version disk_vers;
    char *path_new = NULL;

    memset(&disk_vers, 0, sizeof(disk_vers));

    code = udb_path(dbase, suffix_new, &path_new);
    if (code != 0) {
	goto done;
    }

    code = uphys_getlabel_path(path_new, &disk_vers);
    if (code != 0) {
	goto done;
    }

    if (vcmp(disk_vers, *vers_new) != 0) {
	ViceLog(0, ("ubik: Error: tried to install new db %s, but "
		"version doesn't match (%d.%d != %d.%d).\n",
		path_new,
		disk_vers.epoch, disk_vers.counter,
		vers_new->epoch, vers_new->counter));
	code = UINTERNAL;
	goto done;
    }

 done:
    free(path_new);
    return code;
}

/**
 * Finish installing a new dbase file. This just handles the actual file
 * operations needed to install a new dbase file into place; dealing with locks
 * and transactions etc is handled by the caller.
 *
 * For now, things are pretty simple: we can simply rename() the new .DB0 into
 * place and clobber the old .DB0.
 *
 * @param[in] dbase	    The ubik database
 * @param[in] suffix_new    The suffix of the dbase file to install (e.g.
 *			    ".TMP")
 * @return ubik error codes
 */
static int
udb_install_finish(struct ubik_dbase *dbase, char *suffix_new)
{
    char *path_db = NULL;
    char *path_new = NULL;
    int code;

    code = udb_path(dbase, NULL, &path_db);
    if (code != 0) {
	goto done;
    }

    code = udb_path(dbase, suffix_new, &path_new);
    if (code != 0) {
	goto done;
    }

    /* Now we rename() .DB0.TMP into .DB0.  */
    code = rename(path_new, path_db);
    if (code != 0) {
	ViceLog(0, ("ubik: Failed to rename %s -> %s (errno %d)\n", path_new,
		path_db, errno));
	code = UIOERROR;
	goto done;
    }

 done:
    free(path_db);
    free(path_new);
    return code;
}

/**
 * Pivot a ubik db file into place.
 *
 * @param[in] dbase ubik db
 * @param[in] suffix_new    The db suffix for the new db file to install (e.g. ".TMP")
 * @param[in] new_vers	    Version of the db in 'suffix_new'
 *
 * @return ubik error codes
 */
int
udb_install(struct ubik_dbase *dbase, char *suffix_new,
	    struct ubik_version *new_vers)
{
    int code;
    int file = 0;

    code = udb_install_prep(dbase, suffix_new, new_vers);
    if (code != 0) {
	goto done;
    }

    DBHOLD(dbase);

    urecovery_AbortAll(dbase);

    UBIK_VERSION_LOCK;

    code = udb_install_finish(dbase, suffix_new);
    if (code != 0) {
	goto done_locked;
    }

    uphys_invalidate(dbase, file);
    udisk_Invalidate(dbase, file);

    dbase->version = *new_vers;

    UBIK_VERSION_UNLOCK;
    DBRELE(dbase);

 done:
    return code;

 done_locked:
    UBIK_VERSION_UNLOCK;
    DBRELE(dbase);
    goto done;
}

/**
 * Delete the given database files
 *
 * @param[in] dbase ubik db
 * @param[in] suffix_new    Filename suffix for the "new" tmp db (e.g.
 *			    ".TMP").
 * @param[in] suffix_spare  Filename suffix for the spare db file (e.g. ".OLD")
 *
 * @return ubik error codes
 */
int
udb_del_suffixes(struct ubik_dbase *dbase, char *suffix_new,
		 char *suffix_spare)
{
    char *suffixes[] = {suffix_new, suffix_spare};
    int n_suffixes = sizeof(suffixes)/sizeof(suffixes[0]);
    int suffix_i;
    int code;

    for (suffix_i = 0; suffix_i < n_suffixes; suffix_i++) {
	char *path = NULL;
	char *suffix = suffixes[suffix_i];
	if (suffix == NULL) {
	    continue;
	}
	code = udb_path(dbase, suffix, &path);
	if (code != 0) {
	    return code;
	}

	code = udb_delpath(path);
	free(path);
	if (code != 0) {
	    return code;
	}
    }

    return 0;
}

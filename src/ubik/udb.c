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

static int
udb_dbinfo(char *path, int *a_exists)
{
    struct stat st;
    int code;

    memset(&st, 0, sizeof(st));

    *a_exists = 0;

    code = stat(path, &st);
    if (code != 0) {
	if (errno == ENOENT) {
	    return 0;
	}
	ViceLog(0, ("ubik: Failed to stat %s (errno %d)\n", path, errno));
	return UIOERROR;
    }

    *a_exists = 1;

    if (S_ISREG(st.st_mode)) {
	/* Good, db file looks like a normal db. */

    } else {
	/*
	 * The given db file isn't a regular file? So it's a dir or
	 * socket or something weird like that; bail out.
	 */
	ViceLog(0, ("ubik: Error, weird file mode 0x%x for db %s.\n",
		    st.st_mode, path));
	return UIOERROR;
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
 * place and clobber the old .DB0. If we need to retain the old database, then
 * we create a link() for it first (named .DB0.OLD), so we don't lose the file
 * when it gets clobbered.
 *
 * @param[in] dbase	    The ubik database
 * @param[in] suffix_new    The suffix of the dbase file to install (e.g.
 *			    ".TMP")
 * @param[in] keep_old	    If set to nonzero, make sure we don't delete the
 *			    old database (e.g. by clobbering the file via a
 *			    rename()). If set to nonzero and there is no
 *			    existing dbase file, UINTERNAL is returned.
 * @param[out] a_path_old   If NULL, the existing dbase file must not exist (or
 *			    UINTERNAL is returned). Otherwise, this is set to
 *			    the path of the existing database on return. If
 *			    'keep_old' is 0, this may be set to NULL, if the
 *			    existing dbase file doesn't exist, or it was
 *			    deleted via rename(). The caller may delete the
 *			    given file (if the old dbase doesn't need to be
 *			    saved), or can move it to another permanent path.
 * @return ubik error codes
 */
static int
udb_install_finish(struct ubik_dbase *dbase, char *suffix_new,
		   int keep_old, char **a_path_old)
{
    char *path_db = NULL;
    char *path_new = NULL;
    char *path_old = NULL;
    int exists_db = 0;
    int exists_new = 0;
    int code;

    if (a_path_old != NULL) {
	*a_path_old = NULL;
    }

    code = udb_path(dbase, NULL, &path_db);
    if (code != 0) {
	goto done;
    }

    code = udb_path(dbase, suffix_new, &path_new);
    if (code != 0) {
	goto done;
    }

    code = udb_dbinfo(path_db, &exists_db);
    if (code != 0) {
	goto done;
    }

    code = udb_dbinfo(path_new, &exists_new);
    if (code != 0) {
	goto done;
    }

    if (!exists_new) {
	ViceLog(0, ("ubik: Error, tried to install db %s, but it doesn't "
		    "exist.\n", path_new));
	code = UINTERNAL;
	goto done;
    }

    if (keep_old && !exists_db) {
	ViceLog(0, ("ubik: Error, cannot install new db %s and save existing "
		"db; existing db doesn't exist.\n", suffix_new));
	code = UINTERNAL;
	goto done;
    }

    if (exists_db && a_path_old == NULL) {
	ViceLog(0, ("ubik: Internal error: dbase exists, but a_path_old "
		"unset\n"));
	code = UINTERNAL;
	goto done;
    }

    if (keep_old) {
	/*
	 * If we need to keep the old db, we can't just rename() over it, since
	 * then we'll lose it. So before we rename() over it, make a hard link
	 * to .OLD, so the old db is still accessible via the .OLD suffix
	 * afterwards. Don't simply rename the db into the .OLD file, since
	 * that leaves open a window of time where no .DB0 exists at all.
	 */
	code = udb_path(dbase, ".OLD", &path_old);
	if (code != 0) {
	    goto done;
	}

	code = link(path_db, path_old);
	if (code != 0) {
	    ViceLog(0, ("ubik: Failed to link %s -> %s (errno %d)\n", path_db,
		    path_old, errno));
	    code = UIOERROR;
	    goto done;
	}

	/* Give the old db path to our caller to handle. */
	*a_path_old = path_old;
	path_old = NULL;
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
    free(path_old);
    return code;
}

/**
 * Pivot a ubik db file into place.
 *
 * @param[in] dbase ubik db
 * @param[in] suffix_new    The db suffix for the new db file to install (e.g. ".TMP")
 * @param[in] suffix_old    The db suffix to move the existing db to (e.g.
 *			    ".OLD"), or NULL to delete the existing db
 * @param[in] new_vers	    Version of the db in 'suffix_new'
 *
 * @return ubik error codes
 */
int
udb_install(struct ubik_dbase *dbase, char *suffix_new,
	    char *suffix_old, struct ubik_version *new_vers)
{
    int code;
    int file = 0;
    char *old_path_orig = NULL;
    int keep_old = 0;

    if (suffix_old != NULL) {
	keep_old = 1;
    }

    code = udb_install_prep(dbase, suffix_new, new_vers);
    if (code != 0) {
	goto done;
    }

    DBHOLD(dbase);

    urecovery_AbortAll(dbase);

    UBIK_VERSION_LOCK;

    code = udb_install_finish(dbase, suffix_new, keep_old, &old_path_orig);
    if (code != 0) {
	goto done_locked;
    }

    uphys_invalidate(dbase, file);
    udisk_Invalidate(dbase, file);

    dbase->version = *new_vers;

    UBIK_VERSION_UNLOCK;
    DBRELE(dbase);

    if (old_path_orig != NULL) {
	/* Move the old db file to the given suffix (or delete it, if no suffix
	 * was given). */
	if (suffix_old != NULL) {
	    char *old_path = NULL;
	    code = udb_path(dbase, suffix_old, &old_path);
	    if (code == 0) {
		code = rename(old_path_orig, old_path);
	    }
	    if (code != 0) {
		ViceLog(0, ("ubik: Error, failed to move old db %s (code "
			"%d/%d).\n", old_path_orig, code, errno));
	    }
	    free(old_path);

	} else {
	    code = udb_delpath(old_path_orig);
	    if (code != 0) {
		ViceLog(0, ("ubik: Warning, failed to cleanup old db %s "
			"(code %d). Ignoring error, but beware disk space "
			"being used up by the lingering files.\n",
			old_path_orig, code));
		code = 0;
	    }
	}
    }

 done:
    free(old_path_orig);
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

/**
 * Check if a db path looks like a valid db.
 *
 * If the application has registered a dbcheck_func callback, create a raw
 * transaction for the given path, and call dbcheck_func with that transaction.
 *
 * @param[in] dbase ubik db
 * @param[in] path  Path to the database file to check
 *
 * @return ubik error codes
 */
int
udb_check_contents(struct ubik_dbase *dbase, char *path)
{
    int code;
    struct ubik_dbase *rawdb = NULL;
    struct ubik_trans *trans = NULL;

    if (dbase->dbcheck_func == NULL) {
	code = 0;
	goto done;
    }

    code = ubik_RawInit(path, NULL, &rawdb);
    if (code != 0) {
	goto done;
    }

    code = ubik_BeginTrans(rawdb, UBIK_READTRANS, &trans);
    if (code != 0) {
	goto done;
    }

    code = (*dbase->dbcheck_func)(trans);
    if (code != 0) {
	goto done;
    }

 done:
    if (trans != NULL) {
	ubik_AbortTrans(trans);
    }
    ubik_RawClose(&rawdb);
    return code;
}

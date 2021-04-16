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
#include <afs/okv.h>
#include "ubik_internal.h"

/*
 * udb.c - Routines for handling low-level access to ubik db files. This file
 * is for routines agnostic to the db format (flat-file or KV);
 * flat-file-specific routines go in phys.c, and KV-specific routines go in
 * ukv.c.
 */

int
udb_v64to32(char *descr, struct ubik_version64 *from, struct ubik_version *to)
{
    afs_int64 epoch = opr_time64_toSecs(&from->epoch64);

    if (epoch > MAX_AFS_INT32 || epoch < MIN_AFS_INT32 ||
	from->counter64 > MAX_AFS_INT32 || from->counter64 < MIN_AFS_INT32) {
	ViceLog(0, ("ubik: %s failed: ubik db version %lld.%lld not supported "
		"(out of range)\n",
		descr, from->epoch64.clunks, from->counter64));
	return UINTERNAL;
    }

    to->epoch = epoch;
    to->counter = from->counter64;
    return 0;
}

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

/**
 * Get db metadata.
 *
 * @param[in] path  Path to ubik db
 * @param[out] a_exists	Optional. Set to 1 if path exists at all; 0 otherwise
 * @param[out] a_iskv	Optional. Set to 1 if path is a KV db; 0 otherwise
 * @param[out] a_islink	Optional. Set to 1 if path is a symlink; 0 otherwise
 *
 * @return ubik error codes
 */
int
udb_dbinfo(char *path, int *a_exists, int *a_iskv, int *a_islink)
{
    struct stat st;
    int code;

    memset(&st, 0, sizeof(st));

    *a_iskv = 0;

    code = stat(path, &st);
    if (code != 0) {
	if (errno == ENOENT && a_exists != NULL) {
	    *a_exists = 0;
	    return 0;
	}
	ViceLog(0, ("ubik: Failed to stat %s (errno %d)\n", path, errno));
	return UIOERROR;
    }

    if (a_exists != NULL) {
	*a_exists = 1;
    }

    if (S_ISREG(st.st_mode)) {
	*a_iskv = 0;
    } else if (S_ISDIR(st.st_mode)) {
	*a_iskv = 1;
    } else {
	/*
	 * The given db file isn't a regular file or dir. So it's a fifo or
	 * socket or something weird like that; bail out.
	 */
	ViceLog(0, ("ubik: Error, weird file mode 0x%x for db %s.\n",
		    st.st_mode, path));
	return UIOERROR;
    }

    if (a_islink != NULL) {
	memset(&st, 0, sizeof(st));
	code = lstat(path, &st);
	if (code != 0) {
	    ViceLog(0, ("ubik: Failed to lstat %s (errno %d)\n", path, errno));
	    return UIOERROR;
	}

	*a_islink = 0;
	if (S_ISLNK(st.st_mode)) {
	    *a_islink = 1;
	}
    }

    return 0;
}

int
udb_stat(char *path, struct ubik_stat *astat)
{
    int code;

    memset(astat, 0, sizeof(*astat));

    code = udb_dbinfo(path, NULL, &astat->kv, NULL);
    if (code != 0) {
	return code;
    }

    if (astat->kv) {
	return ukv_stat(path, astat);

    } else {
	return uphys_stat_path(path, astat);
    }
}

static int
opendb_path(char *path, struct okv_dbhandle **a_dbh, struct ubik_version *version)
{
    int code;
    int iskv = 0;

    if (a_dbh != NULL) {
	*a_dbh = NULL;
    }

    code = udb_dbinfo(path, NULL, &iskv, NULL);
    if (code != 0) {
	return code;
    }

    if (iskv) {
	return ukv_open(path, a_dbh, version);
    } else {
	return uphys_getlabel_path(path, version);
    }
}

int
udb_getlabel_path(char *path, struct ubik_version *aversion)
{
    return opendb_path(path, NULL, aversion);
}

int
udb_getlabel_db(struct ubik_dbase *dbase, struct ubik_version *version)
{
    if (ubik_KVDbase(dbase)) {
	return ukv_getlabel_db(dbase, version);
    } else {
	return uphys_getlabel(dbase, 0, version);
    }
}

int
udb_setlabel_path(char *path, struct ubik_version *version)
{
    int code;
    int iskv = 0;

    code = udb_dbinfo(path, NULL, &iskv, NULL);
    if (code != 0) {
	return code;
    }

    if (iskv) {
	return ukv_setlabel_path(path, version);
    } else {
	return uphys_setlabel_path(path, version);
    }
}

int
udb_setlabel_trans(struct ubik_trans *trans, struct ubik_version *version)
{
    if (ubik_KVTrans(trans)) {
	return ukv_setlabel(trans->kv_tx, version);
    } else {
	return uphys_setlabel(trans->dbase, 0, version);
    }
}

int
udb_setlabel_db(struct ubik_dbase *dbase, struct ubik_version *version)
{
    if (ubik_KVDbase(dbase)) {
	return ukv_setlabel_db(dbase, version);
    } else {
	return uphys_setlabel(dbase, 0, version);
    }
}

int
udb_delpath(char *path)
{
    int code;
    int exists = 0;
    int isdir = 0;
    int islink = 0;
    char *path_free = NULL;

    code = udb_dbinfo(path, &exists, &isdir, &islink);
    if (code != 0) {
	goto done;
    }

    if (!exists) {
	goto done;
    }

    if (isdir) {
	if (islink) {
	    path_free = realpath(path, NULL);
	    if (path_free == NULL) {
		ViceLog(0, ("ubik: Failed to resolve %s before deleting (errno "
			"%d)\n", path, errno));
		code = UIOERROR;
		goto done;
	    }

	    code = unlink(path);
	    if (code != 0) {
		ViceLog(0, ("ubik: Warning: failed to unlink symlink %s (errno "
			"%d)\n", path, errno));
		code = 0;
	    }
	    path = path_free;
	}

	code = okv_unlink(path);
	if (code != 0) {
	    code = UIOERROR;
	    goto done;
	}
    } else {
	code = unlink(path);
	if (code != 0) {
	    ViceLog(0, ("ubik: Failed to unlink %s, errno=%d\n", path, errno));
	    code = UIOERROR;
	    goto done;
	}
    }

 done:
    free(path_free);
    return code;
}

/**
 * Prepare a dbase file for installation with udb_install_finish. This checks
 * that the db version matches 'vers_new', and for KV dbases, replaces the .DB0
 * dir with a symlink to a dir inside DB.d (see ukv_db_prepinstall).
 *
 * @param[in] dbase	    The ubik database
 * @param[in] suffix_new    The suffix of the dbase file getting installed
 *			    (e.g. ".TMP")
 * @param[in] vers_new	    The version of the dbase file getting installed
 * @param[out] a_dbh	    The okv dbhandle of the dbase file getting
 *			    installed (if it's a KV dbase)
 * @return ubik error codes
 */
static int
udb_install_prep(struct ubik_dbase *dbase, char *suffix_new,
		 struct ubik_version *vers_new, struct okv_dbhandle **a_dbh)
{
    int code;
    int is_kv = 0;
    char *path_new = NULL;
    struct okv_dbhandle *dbh = NULL;
    struct ubik_version disk_vers;

    memset(&disk_vers, 0, sizeof(disk_vers));

    code = udb_path(dbase, suffix_new, &path_new);
    if (code != 0) {
	goto done;
    }

    code = udb_getlabel_path(path_new, &disk_vers);
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

    code = udb_dbinfo(path_new, NULL, &is_kv, NULL);
    if (code != 0) {
	goto done;
    }

    if (is_kv) {
	/* Move .DB0.TMP to .DB.d/foo.DB0, then create a symlink for
	 * .DB0.TMP -> .DB.d/foo.DB0. */
	code = ukv_db_prepinstall(dbase, path_new);
	if (code != 0) {
	    goto done;
	}

	code = ukv_open(path_new, &dbh, &disk_vers);
	if (code != 0) {
	    goto done;
	}

	if (vcmp(disk_vers, *vers_new) != 0) {
	    ViceLog(0, ("ubik: Internal error: post-prepinstall db %s "
		    "version doesn't match (%d.%d != %d.%d).\n",
		    path_new,
		    disk_vers.epoch, disk_vers.counter,
		    vers_new->epoch, vers_new->counter));
	    code = UINTERNAL;
	    goto done;
	}
    }

    if (a_dbh != NULL) {
	*a_dbh = dbh;
	dbh = NULL;
    }

 done:
    okv_close(&dbh);
    free(path_new);
    return code;
}

/**
 * Finish installing a new dbase file. This pivots the new dbase file into
 * place, and optionally returns some information about the pre-existing dbase
 * to the caller.
 *
 * We try to pivot the new dbase into place as atomically as possible, to try
 * and ensure that a valid .DB0 file is always in place, even though our
 * process may be killed at any point.
 *
 * For flatfile databases, this means we just rename() the new dbase into
 * place. If we need the old dbase file to stay around, we create a link() for
 * it first, so it doesn't get clobbered by the rename().
 *
 * For KV databases, we rename() a symlink to the actual dbase dir into place.
 * The given suffix must already be a symlink (which is handled by
 * udb_install_prep). For example:
 *
 * If our new dbase is a KV dir (call it vldb.DB0.TMP), udb_install_prep
 * renames it to, say, vldb.DB.d/vldb.1234.0.DB0, and creates a symlink
 * vldb.DB0.TMP -> vldb.DB.d/vldb.1234.0.DB0 (see ukv_db_prepinstall). Then we
 * atomically rename() the symlink into place, so we end up with a vldb.DB0 ->
 * vldb.DB.d/vldb.1234.0.DB0.
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
int
udb_install_finish(struct ubik_dbase *dbase, char *suffix_new,
		   int keep_old, char **a_path_old)
{
    char *path_db = NULL;
    char *path_new = NULL;
    char *path_old = NULL;
    int code;
    int exists_db = 0;
    int isdir_db = 0;
    int exists_new = 0;
    int isdir_new = 0;
    int islink_new = 0;

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

    code = udb_dbinfo(path_db, &exists_db, &isdir_db, NULL);
    if (code != 0) {
	goto done;
    }

    code = udb_dbinfo(path_new, &exists_new, &isdir_new, &islink_new);
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

    if (isdir_new && !islink_new) {
	/* If the new db is KV, it must be a symlink, for the rename() below to
	 * atomically point to the new db. */
	ViceLog(0, ("ubik: Internal error: new db dir %s is non-symlink\n",
		path_new));
	code = UINTERNAL;
	goto done;
    }

    if (keep_old && !isdir_db) {
	/*
	 * If we need to keep the old db and the old db is a plain file, we
	 * can't just rename() over it, since then we'll lose it. So before we
	 * rename() over it, make a hard link to .OLD, so the old db is still
	 * accessible via the .OLD suffix afterwards. Don't simply rename the
	 * db into the .DB0 file, since that leaves open a window of time where
	 * no .DB0 exists at all.
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

    if (isdir_db) {
	/*
	 * If we have a KV dbase, our .DB0 file is a symlink to the actual okv
	 * dbase path inside .DB0.d. Give the resolved name to our caller for
	 * them to process the old db.
	 */
	code = ukv_db_readlink(dbase, path_db, a_path_old);
	if (code != 0) {
	    goto done;
	}
    }

    /*
     * Now we rename() .DB0.TMP into .DB0. Because of the steps taken above,
     * this should atomically point .DB0 to the new database, regardless of
     * whether the old or new db is a KV dbase or a flatfile dbase.
     */
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

/*
 * Like udb_install, but for installing a db during server startup, not while
 * ubik is actually running. This skips some processing and log messages, and
 * assumes there is no existing dbase in place.
 */
int
udb_install_simple(struct ubik_dbase *dbase, char *suffix_new,
		   struct ubik_version *vers_new)
{
    int code = udb_install_prep(dbase, suffix_new, vers_new, NULL);
    if (code != 0) {
	return code;
    }

    return udb_install_finish(dbase, suffix_new, 0, NULL);
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
    struct okv_dbhandle *dbh = NULL;
    char *old_path_orig = NULL;
    int keep_old = 0;
    int flat2kv = 0;
    int kv2flat = 0;

    if (suffix_old != NULL) {
	keep_old = 1;
    }

    code = udb_install_prep(dbase, suffix_new, new_vers, &dbh);
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

    if (dbase->kv_dbh == NULL && dbh != NULL) {
	flat2kv = 1;
	ViceLog(0, ("ubik: Switching from flat-file to KV database.\n"));
    }
    if (dbase->kv_dbh != NULL && dbh == NULL) {
	kv2flat = 1;
	ViceLog(0, ("ubik: Switching from KV to flat-file database.\n"));
    }

    {
	/* Swap the old and new dbh */
	struct okv_dbhandle *old_dbh = dbase->kv_dbh;
	dbase->kv_dbh = dbh;
	dbh = old_dbh;
    }

    dbase->version = *new_vers;

    UBIK_VERSION_UNLOCK;
    DBRELE(dbase);

    okv_close(&dbh);

    if (old_path_orig != NULL) {
	/* Move the old db file to the given suffix (or delete it, if no suffix
	 * was given). */
	if (suffix_old != NULL) {
	    char *old_path = NULL;
	    code = udb_path(dbase, suffix_old, &old_path);
	    if (code == 0) {
		code = okv_rename(old_path_orig, old_path);
	    }
	    if (code != 0) {
		ViceLog(0, ("ubik: Error, failed to move old db %s (code "
			"%d).\n", old_path_orig, code));
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

    /*
     * If we're switching from a flat dbase to KV (or vice versa), try to
     * cleanup some cruft from the old dbase type. For flat files, we no longer
     * need the .DBSYS1 file; for KV dbases, we no longer need the .DB.d
     * directory if it's empty. This is just a nice-to-have, so don't even log
     * anything if we fail to remove the relevant item.
     */
    if (flat2kv) {
	char *dbsys1;
	if (asprintf(&dbsys1, "%s.DBSYS1", dbase->pathName) > 0) {
	    (void)unlink(dbsys1);
	    free(dbsys1);
	}
    }
    if (kv2flat) {
	ukv_cleanup_unused(dbase);
    }

 done:
    okv_close(&dbh);
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

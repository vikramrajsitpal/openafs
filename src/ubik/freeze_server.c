/*
 * Copyright (c) 2021 Sine Nomine Associates. All rights reserved.
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

#ifdef AFS_CTL_ENV

# include <roken.h>

# include <afs/opr.h>
# include <opr/time64.h>
# include <opr/lock.h>
# include <afs/afsutil.h>
# include <afs/afsctl.h>

# include "ubik_internal.h"

# define TIMEOUT_MIN 5
# define TIMEOUT_MAX (1000*60*60*24*21) /* 21 days */

/*
 * Data for the active freeze. We usually only have one of these instantiated
 * (ufreeze_active_frz), since only one freeze can exist at a time.
 */
struct ufreeze_ctx {
    afs_uint64 freezeid;

    struct afsctl_call *ctl;	/**< The UFRZCTL_OP_FREEZEDB ctl call for this
				 *   freeze. */

    /*
     * When someone calls frz_end(), 'ending' is set. Then when the freeze
     * actually ends, 'ended' is set. We may not set 'ended' until some time
     * later, if a long-running rpc (running_ctl) is running when someone tries
     * to end the freeze. We can't end the freeze immediately in that case,
     * because that rpc is using the freeze; when the rpc finally finishes,
     * then the freeze will end and 'ended' will get set.
     */
    int ending;
    int ended;

    int successful; /**< Did the freeze end successfully? (that is, someone
		     *   called ufreeze_end without aborting) */

    int freeze_rw;  /**< Can a new db get installed during this freeze? */
    int db_changed; /**< Has someone installed a new db during this freeze? */

    /*
     * If this is non-NULL, a long-running request is running for the freeze
     * (which is running without ufreeze_lock held). When the freeze is ending,
     * we must wait for this to go away before we can end and free the freeze.
     */
    struct afsctl_call *running_ctl;

    /*
     * When a new dbase has been installed during a freeze, we save the
     * original db in 'backup_suffix', which is db version 'backup_vers'. If
     * 'unlink_backup' is set, we delete this backup copy when the freeze has
     * ended successfully. 'backup_suffix' must be freed when we free the
     * freeze.
     */
    char *backup_suffix;
    struct ubik_version backup_vers;
    int unlink_backup;

    int dbflags_set;	/**< Which flags we've set on ubik_dbase (e.g.
			 *   DBSENDING) */

    afs_uint32 timeout_ms;  /**< Max time the freeze can run for (or 0, for no
			     *   limit) */
    struct afs_time64 start_time;
};

static opr_mutex_t ufreeze_lock;
static opr_cv_t ufreeze_cv;

/* Protected by ufreeze_lock */
static struct ufreeze_ctx *ufreeze_active_frz;

/**
 * End the given freeze.
 *
 * Ending the freeze means we clear any db flags we've set during the freeze,
 * and we clear ufreeze_active_frz. If the freeze was not successful, this is
 * also when we revert the database back to the original version. If the freeze
 * was successful, we delete the backup copy of the database if 'unlink_backup'
 * is set.
 *
 * @param[in] frz   The freeze we are ending.
 * @param[in] wait  If there's a long-running rpc using the freeze
 *		    (running_ctl), and 'wait' is zero, we'll return EBUSY. If
 *		    'wait' is non-zero, we'll wait until running_ctl goes away,
 *		    and then end the freeze.
 * @return errno error codes
 * @retval EBUSY    There's a long-running rpc using the freeze, and 'wait' is
 *		    0.
 *
 * @pre ufreeze_lock held
 */
static int
frz_end(struct ufreeze_ctx *frz, int wait)
{
    int code;

    if (frz->ended) {
	return 0;
    }

    frz->ending = 1;

    while (frz->running_ctl != NULL) {
	if (!wait) {
	    ViceLog(0, ("ubik: Deferring ending freeze %llu until request (%s) "
		    "finishes\n",
		    frz->freezeid, afsctl_call_describe(frz->running_ctl)));
	    return EBUSY;
	}

	ViceLog(0, ("ubik: Waiting for request (%s) to finish before ending "
		"freeze %llu\n",
		afsctl_call_describe(frz->running_ctl), frz->freezeid));
	opr_cv_wait(&ufreeze_cv, &ufreeze_lock);
    }

    /* Re-check; someone may have ended the freeze while we were waiting. */
    if (frz->ended) {
	return 0;
    }

    if (!frz->successful && frz->db_changed) {
	/*
	 * The freeze was aborted for whatever reason. If we installed a new db
	 * during the freeze, try to revert the db back to the original
	 * version.
	 */
	opr_Assert(frz->backup_suffix != NULL);
	ViceLog(0, ("ubik: Reverting db to original frozen version (%s, %d.%d)\n",
		frz->backup_suffix,
		frz->backup_vers.epoch,
		frz->backup_vers.counter));
	code = udb_install(ubik_dbase, frz->backup_suffix, NULL, &frz->backup_vers);
	if (code != 0) {
	    ViceLog(0, ("ubik: Failed to revert db (code %d); proceeding with "
		    "new db from aborted freeze.\n", code));
	} else {
	    frz->db_changed = 0;
	    frz->unlink_backup = 0;
	}
    }

    if (frz->unlink_backup) {
	opr_Assert(frz->backup_suffix != NULL);
	code = udb_del_suffixes(ubik_dbase, NULL, frz->backup_suffix);
	if (code != 0) {
	    ViceLog(0, ("ubik: warning: failed to cleanup old dbase suffix %s "
		    "(code %d)\n", frz->backup_suffix, code));
	}
    }

    if (frz->dbflags_set != 0) {
	DBHOLD(ubik_dbase);
	ubik_clear_db_flags(ubik_dbase, frz->dbflags_set);
	DBRELE(ubik_dbase);
	frz->dbflags_set = 0;
    }

    frz->ended = 1;

    if (frz->successful) {
	ViceLog(0, ("ubik: Freeze %llu ended successfully.\n", frz->freezeid));
    } else {
	ViceLog(0, ("ubik: Freeze %llu failed.\n", frz->freezeid));
    }

    opr_Assert(ufreeze_active_frz == frz);
    ufreeze_active_frz = NULL;

    return 0;
}

/**
 * Free the freeze. Only do this in ufreeze_freezedb; for everyone else, if you
 * want a freeze to go away, frz_end() it, and get ufreeze_freezedb to wakeup
 * so it can free the freeze.
 *
 * @pre ufreeze_lock held
 */
static void
frz_destroy(struct ufreeze_ctx **a_frz)
{
    struct ufreeze_ctx *frz = *a_frz;
    if (frz == NULL) {
	return;
    }
    *a_frz = NULL;

    opr_Assert(frz->running_ctl == NULL);

    (void)frz_end(frz, 1);

    free(frz->backup_suffix);
    free(frz);
}

/**
 * Get the current freeze context.
 *
 * Note that the caller can only use the returned a_frz context while
 * ufreeze_lock is held. If you need to drop ufreeze_lock, use ufreeze_getfrz
 * instead.
 *
 * @param[in] freezeid	If not NULL, check that the active freeze matches this
 *			freezeid. If it doesn't, return USYNC.
 * @param[out] a_frz	On success, set to the active freeze.
 * @return ubik error codes
 *
 * @pre ufreeze_lock held
 */
static int
ufreeze_peekfrz_r(afs_uint64 *freezeid, struct ufreeze_ctx **a_frz)
{
    struct ufreeze_ctx *frz = ufreeze_active_frz;

    *a_frz = NULL;

    if (frz == NULL) {
	return UNOENT;
    }
    if (freezeid != NULL && frz->freezeid != *freezeid) {
	return USYNC;
    }

    *a_frz = frz;
    return 0;
}

/**
 * Get a reference to the current freeze context for a long-running rpc.
 *
 * Use this instead of ufreeze_peekfrz_r to get a reference to the current
 * freeze context, and to be able to use it without holding ufreeze_lock. Only
 * one such rpc can use the freeze context at a time; if another rpc is already
 * using the freeze, we'll return USYNC.
 *
 * @param[in] func  Name of the calling function/rpc (for logging).
 * @param[in] ctl   The afsctl call for the rpc using the freeze.
 * @param[in] freezeid	The freezeid of the freeze.
 * @param[out] a_frz	On success, set to the active freeze.
 *
 * @return ubik error codes
 * @retval USYNC    Another rpc is already using the freeze.
 */
static int
ufreeze_getfrz(char *func, struct afsctl_call *ctl, afs_uint64 freezeid,
	       struct ufreeze_ctx **a_frz)
{
    int code;
    const char *caller = afsctl_call_describe(ctl);
    struct ufreeze_ctx *frz = NULL;

    opr_mutex_enter(&ufreeze_lock);

    code = ufreeze_peekfrz_r(&freezeid, &frz);
    if (code != 0) {
	goto done;
    }

    if (frz->ending) {
	code = UDONE;
	goto done;
    }

    if (frz->running_ctl != NULL) {
	ViceLog(0, ("%s(%s): Failed for freezeid %llu: another request "
		"for the freeze is still running (%s).\n",
		func, caller, frz->freezeid,
		afsctl_call_describe(frz->running_ctl)));
	code = USYNC;
	goto done;
    }

    frz->running_ctl = ctl;
    *a_frz = frz;

 done:
    opr_mutex_exit(&ufreeze_lock);
    return code;
}

/**
 * Put back a freeze obtained from ufreeze_getfrz.
 *
 * @param[in] ctl   The afsctl call for the rpc using the freeze.
 * @param[inout] a_frz	The freeze context to put back. If NULL, this is a
 *			no-op. Set to NULL on return.
 */
static void
ufreeze_putfrz(struct afsctl_call *ctl, struct ufreeze_ctx **a_frz)
{
    int code;
    struct ufreeze_ctx *frz = *a_frz;
    struct ufreeze_ctx *active_frz = NULL;

    if (frz == NULL) {
	return;
    }

    *a_frz = NULL;

    opr_mutex_enter(&ufreeze_lock);

    /*
     * If our caller has an 'frz', it had better be the active freeze.
     * Otherwise, what is it? Our caller must have gotten its 'frz' from
     * ufreeze_getfrz, and nobody can end the freeze until we put back our ref.
     * So if the active frz is something else, is our 'frz' referencing freed
     * memory somehow?
     *
     * So, ufreeze_peekfrz_r really must succeed here, and the frz we get back
     * must be ours, and it must reference our 'ctl' call. If any of that isn't
     * true, something is seriously wrong, so assert immediately.
     */
    code = ufreeze_peekfrz_r(NULL, &active_frz);
    opr_Assert(code == 0);
    opr_Assert(active_frz == frz);
    opr_Assert(frz->running_ctl == ctl);

    frz->running_ctl = NULL;

    if (frz->ending) {
	/*
	 * Someone probably tried to end the freeze and couldn't, because we
	 * were still running with an active reference. Now that we've put our
	 * reference back, the freeze should be able to end now; do so now, to
	 * make it end as soon as possible.
	 */
	(void)frz_end(frz, 0);
    }

    opr_cv_broadcast(&ufreeze_cv);

    opr_mutex_exit(&ufreeze_lock);
}

/**
 * Check if it's okay to start a freeze on the db.
 *
 * @param[in] need_sync	Whether the freeze needs the sync site.
 * @return ubik error codes
 *
 * @pre DBHOLD held
 */
static int
ufreeze_checkdb_r(int need_sync)
{
    int readAny = need_sync ? 0 : 1;
    if (need_sync && !ubeacon_AmSyncSite()) {
	return UNOTSYNC;
    }
    if (!urecovery_AllBetter(ubik_dbase, readAny)) {
	return UNOQUORUM;
    }
    return 0;
}

/* @pre ufreeze_lock held */
static afs_uint64
freezeid_gen(void)
{
    static afs_uint64 counter;
    if (counter == 0) {
	counter++;
    }
    return counter++;
}

static int
ufreeze_freezedb(struct afsctl_call *ctl,
		 json_t *in_args, json_t **out_args)
{
    int code;
    int freeze_rw = 0;
    int need_sync = 0;
    int timeout_ms = 0;
    int no_timeout = 0;
    struct ufreeze_ctx *frz = NULL;
    struct ubik_version disk_vers32;
    struct ubik_version64 version;
    const char *caller = afsctl_call_describe(ctl);
    char *db_path = NULL;
    json_error_t jerror;

    memset(&disk_vers32, 0, sizeof(disk_vers32));
    memset(&version, 0, sizeof(version));

    code = json_unpack_ex(in_args, &jerror, 0,
			  "{s:b, s?:i, s?:b, s?:b}",
			  "need_sync", &need_sync,
			  "timeout_ms", &timeout_ms,
			  "no_timeout", &no_timeout,
			  "readwrite", &freeze_rw);
    if (code != 0) {
	ViceLog(0, ("ufreeze_freezedb(%s): Error unpacking in_args: %s\n",
		caller, jerror.text));
	return UINTERNAL;
    }

    if (freeze_rw) {
	need_sync = 1;
    }

    if (no_timeout) {
	if (timeout_ms != 0) {
	    ViceLog(0, ("ufreeze_freezedb(%s): Error: both no_timeout and "
		    "timeout_ms (%d) given.\n",
		    caller, timeout_ms));
	    return UINTERNAL;
	}
    } else {
	if (timeout_ms < TIMEOUT_MIN || timeout_ms > TIMEOUT_MAX) {
	    ViceLog(0, ("ufreeze_freezedb(%s): bad timeout %u\n",
		    caller, timeout_ms));
	    return UINTERNAL;
	}
    }

    opr_mutex_enter(&ufreeze_lock);

    if (ufreeze_active_frz != NULL) {
	ViceLog(0, ("ufreeze_freezedb(%s): Cannot start freeze; "
		    "existing freeze %llu is still running (started at "
		    "%lld).\n",
		    caller, ufreeze_active_frz->freezeid,
		    ufreeze_active_frz->start_time.clunks));
	code = USYNC;
	goto done;
    }

    DBHOLD(ubik_dbase);

    code = ufreeze_checkdb_r(need_sync);
    if (code != 0) {
	goto done_dblocked;
    }

    if (ubik_wait_db_flags(ubik_dbase, DBWRITING | DBSENDING)) {
	ViceLog(0, ("ufreeze_freezedb(%s): Error: unexpected db flags 0x%x.\n",
		    caller, ubik_dbase->dbFlags));
	code = UINTERNAL;
	goto done_dblocked;
    }

    code = ufreeze_checkdb_r(need_sync);
    if (code != 0) {
	goto done_dblocked;
    }

    frz = calloc(1, sizeof(*frz));
    if (frz == NULL) {
	code = UNOMEM;
	goto done_dblocked;
    }

    frz->freeze_rw = freeze_rw;

    if (freeze_rw) {
	frz->dbflags_set = DBRECEIVING;
    } else {
	frz->dbflags_set = DBSENDING;
    }
    ubik_set_db_flags(ubik_dbase, frz->dbflags_set);

    frz->freezeid = freezeid_gen();
    opr_Assert(frz->freezeid != 0);

    frz->ctl = ctl;
    frz->timeout_ms = timeout_ms;

    code = opr_time64_now(&frz->start_time);
    if (code != 0) {
	code = UINTERNAL;
	goto done_dblocked;
    }

    /* Set this frz as the 'active' freeze. */
    opr_Assert(ufreeze_active_frz == NULL);
    ufreeze_active_frz = frz;

    code = udb_getlabel_db(ubik_dbase, &disk_vers32);
    if (code != 0) {
	ViceLog(0, ("ufreeze_freezedb(%s): Cannot get db label, code %d\n",
		    caller, code));
	goto done_dblocked;
    }

    udb_v32to64(&disk_vers32, &version);

    ViceLog(0, ("ufreeze_freedb(%s): Freeze id %llu started, version %lld.%lld "
	    "timeout %u ms.\n",
	    caller, frz->freezeid, version.epoch64.clunks, version.counter64,
	    timeout_ms));

    DBRELE(ubik_dbase);

    code = udb_path(ubik_dbase, NULL, &db_path);
    if (code != 0) {
	goto done;
    }

    /* db is now frozen; send info about the freeze to our peer. */

    code = afsctl_send_pack(ctl, "{s:I, s:{s:I, s:I}, s:s}",
			    "freeze_id", (json_int_t)frz->freezeid,
			    "version", "epoch64", (json_int_t)version.epoch64.clunks,
				       "counter", (json_int_t)version.counter64,
			    "db_path", db_path);
    if (code != 0) {
	goto done;
    }

    /*
     * Wait for either the caller to die (shutting down the socket), or for
     * another thread to interrupt us (ufreeze_end), or for the timeout to
     * trigger.
     */
    opr_mutex_exit(&ufreeze_lock);
    code = afsctl_wait_recv(ctl, timeout_ms);
    opr_mutex_enter(&ufreeze_lock);

    if (!frz->ending) {
	if (code == 0) {
	    code = UIOERROR;
	    ViceLog(0, ("ufreeze_freezedb(%s): Aborting freeze %llu: peer died\n",
			caller, frz->freezeid));

	} else if (code == ETIMEDOUT) {
	    ViceLog(0, ("ufreeze_freezedb(%s): Aborting freeze %llu: timed out\n",
			caller, frz->freezeid));

	} else {
	    ViceLog(0, ("ufreeze_freezedb(%s): Aborting freeze %llu: wait_recv returned %d\n",
			caller, frz->freezeid, code));
	}
    }

    (void)frz_end(frz, 1);

    if (code == 0 && !frz->successful) {
	/* If the freeze was not successful (e.g. it was aborted), make sure we
	 * return an error to the caller. */
	code = UDONE;
    }

 done:
    frz_destroy(&frz);
    opr_mutex_exit(&ufreeze_lock);
    free(db_path);
    return code;

 done_dblocked:
    DBRELE(ubik_dbase);
    goto done;
}

static int
ufreeze_end(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    int code;
    struct ufreeze_ctx *frz = NULL;
    const char *caller = afsctl_call_describe(ctl);
    afs_uint64 freezeid;
    json_int_t j_freezeid = 0;
    json_error_t jerror;
    int success = 0;
    int abort = 0;
    int force_abort = 0;

    code = json_unpack_ex(in_args, &jerror, 0,
			  "{s?:I, s?:b, s?:b, s?:b}",
			  "freeze_id", &j_freezeid,
			  "success", &success,
			  "abort", &abort,
			  "force_abort", &force_abort);
    if (code != 0) {
	ViceLog(0, ("ufreeze_end(%s): Error unpacking in_args: %s\n",
		    caller, jerror.text));
	return UINTERNAL;
    }

    freezeid = j_freezeid;

    if (force_abort) {
	abort = 1;

    } else if (freezeid == 0) {
	ViceLog(0, ("ufreeze_end(%s): Missing freeze_id\n", caller));
	return UINTERNAL;
    }

    if (success == abort) {
	ViceLog(0, ("ufreeze_end(%s): Invalid success/abort (%d/%d)\n",
		    caller, success, abort));
	return UINTERNAL;
    }

    opr_mutex_enter(&ufreeze_lock);

    if (force_abort) {
	/* Don't check the freezeid, just abort whatever frz we find. */
	code = ufreeze_peekfrz_r(NULL, &frz);
    } else {
	code = ufreeze_peekfrz_r(&freezeid, &frz);
    }
    if (code != 0) {
	goto done;
    }

    if (frz->ending) {
	code = UTWOENDS;
	goto done;
    }

    /* Shutdown the socket for reading, so ufreeze_freezedb wakes up and sees
     * that the freeze has ended. */
    code = afsctl_call_shutdown_read(frz->ctl);
    if (code != 0) {
	ViceLog(0, ("ufreeze_end(%s): failed to shutdown socket "
		"(code %d).\n", caller, code));
	code = UIOERROR;
	goto done_end;
    }

    frz->successful = 0;
    if (force_abort) {
	ViceLog(0, ("ufreeze_end(%s): Forcibly aborting freeze %llu\n",
		    caller, frz->freezeid));

    } else if (abort) {
	ViceLog(0, ("ufreeze_end(%s): Aborting freeze %llu\n", caller,
		    frz->freezeid));

    } else {
	frz->successful = 1;
	ViceLog(0, ("ufreeze_end(%s): Ending freeze %llu\n", caller, frz->freezeid));
    }

 done_end:
    {
	int code_end = frz_end(frz, 0);
	if (code == 0) {
	    code = code_end;
	}
    }

 done:
    opr_mutex_exit(&ufreeze_lock);
    return code;
}

static int
ufreeze_install(struct afsctl_call *ctl,
		json_t *in_args, json_t **out_args)
{
    int code;
    int keep_old;
    struct ubik_version64 disk_vers;
    struct ubik_version64 old_version;
    struct ubik_version64 new_version;
    struct ubik_version disk_vers32;
    struct ubik_version old_vers32;
    struct ubik_version new_vers32;
    int others_exist;
    struct ubik_server *ts;
    char *path = NULL;
    char *new_suffix = NULL;
    char *backup_path = NULL;
    char *backup_suffix = NULL;
    char *j_backup_suffix = NULL;
    json_error_t jerror;
    const char *caller = afsctl_call_describe(ctl);
    struct ubik_dbase *dbase = ubik_dbase;
    struct ufreeze_ctx *frz = NULL;
    afs_uint64 freezeid;
    afs_uint64 now;

    memset(&disk_vers, 0, sizeof(disk_vers));
    memset(&old_version, 0, sizeof(old_version));
    memset(&new_version, 0, sizeof(new_version));
    memset(&disk_vers32, 0, sizeof(disk_vers32));
    memset(&old_vers32, 0, sizeof(old_vers32));
    memset(&new_vers32, 0, sizeof(new_vers32));

    {
	json_int_t j_freezeid;
	json_int_t old_epoch, old_counter;
	json_int_t new_epoch, new_counter;

	code = json_unpack_ex(in_args, &jerror, 0,
			      "{s:I, s:{s:I, s:I}, s:{s:I, s:I}, s:s, s?:s}",
			      "freeze_id", &j_freezeid,
			      "old_version", "epoch64", &old_epoch,
					     "counter", &old_counter,
			      "new_version", "epoch64", &new_epoch,
					     "counter", &new_counter,
			      "new_suffix", &new_suffix,
			      "backup_suffix", &j_backup_suffix);
	if (code != 0) {
	    ViceLog(0, ("ufreeze_install(%s): Error unpacking in_args: %s\n",
		    caller, jerror.text));
	    code = UINTERNAL;
	    goto done;
	}

	freezeid = j_freezeid;
	old_version.epoch64.clunks = old_epoch;
	old_version.counter64 = old_counter;
	new_version.epoch64.clunks = new_epoch;
	new_version.counter64 = new_counter;
    }

    if (udb_vcmp64(&old_version, &new_version) >= 0) {
	/* If we're changing the db contents, the new version had better be
	 * newer than the old version. */
	ViceLog(0, ("ufreeze_install(%s): Cannot install db: nonsense "
		    "versions %lld.%lld -> %lld.%lld\n",
		    caller,
		    old_version.epoch64.clunks,
		    old_version.counter64,
		    new_version.epoch64.clunks,
		    new_version.counter64));
	code = UINTERNAL;
	goto done;
    }

    now = time(NULL);
    if (opr_time64_toSecs(&new_version.epoch64) >= now) {
	/*
	 * Don't let the caller install a db with an epoch of 'now' or newer,
	 * to make sure that any db labelled after this will get a 'newer'
	 * version.
	 */
	ViceLog(0, ("ufreeze_install(%s): Cannot install db: new db version "
		    "looks too new (%lld)\n",
		    caller, new_version.epoch64.clunks));
	code = UINTERNAL;
	goto done;
    }

    if (new_suffix[0] == '\0') {
	ViceLog(0, ("ufreeze_install(%s): Cannot install db: blank db suffix.\n",
		    caller));
	code = UINTERNAL;
	goto done;
    }

    if (j_backup_suffix == NULL || j_backup_suffix[0] == '\0') {
	j_backup_suffix = ".OLD";
	keep_old = 0;
    } else {
	keep_old = 1;
    }
    backup_suffix = strdup(j_backup_suffix);
    if (backup_suffix == NULL) {
	code = UNOMEM;
	goto done;
    }

    code = ufreeze_getfrz("ufreeze_install", ctl, freezeid, &frz);
    if (code != 0) {
	goto done;
    }

    if (!frz->freeze_rw || frz->dbflags_set != DBRECEIVING) {
	ViceLog(0, ("ufreeze_install(%s): Cannot install db for freezeid %llu; "
		    "freeze is readonly.\n", caller, frz->freezeid));
	code = UBADTYPE;
	goto done;
    }

    if (frz->db_changed) {
	/*
	 * Don't allow multiple installs/restores for the same freeze. This
	 * makes the backup/revert/etc logic simpler, and it's hard to see why
	 * anyone would want to do this; just start a new freeze.
	 */
	ViceLog(0, ("ufreeze_install(%s): Cannot install db for freezeid %llu: "
		    "a new db for this freeze has already been installed.\n",
		    caller, frz->freezeid));
	code = UINTERNAL;
	goto done;
    }

    /* Check that the existing db matches what we're given. */

    DBHOLD(dbase);
    code = udb_getlabel_db(dbase, &disk_vers32);
    DBRELE(dbase);
    if (code != 0) {
	goto done;
    }

    old_vers32 = disk_vers32;
    udb_v32to64(&disk_vers32, &disk_vers);
    if (udb_vcmp64(&old_version, &disk_vers) != 0) {
	ViceLog(0, ("ubik: Cannot install db for freezeid %llu: old_version "
		    "mismatch: %lld.%lld != %lld.%lld\n",
		    frz->freezeid,
		    old_version.epoch64.clunks,
		    old_version.counter64,
		    disk_vers.epoch64.clunks,
		    disk_vers.counter64));
	code = UINTERNAL;
	goto done;
    }

    frz->backup_vers = disk_vers32;

    code = udb_path(dbase, new_suffix, &path);
    if (code != 0) {
	goto done;
    }

    code = udb_path(dbase, backup_suffix, &backup_path);
    if (code != 0) {
	goto done;
    }

    if (access(backup_path, F_OK) == 0) {
	ViceLog(0, ("ubik: Cannot install new db with backup to %s; backup "
		"path already exists\n", backup_path));
	code = UIOERROR;
	goto done;
    }

    /* Check that the new db on disk matches the version we were given. */

    code = udb_getlabel_path(path, &disk_vers32);
    if (code != 0) {
	ViceLog(0, ("ubik: Cannot install new db for freezeid %llu: cannot open "
		    "new database suffix %s (code %d)\n",
		    frz->freezeid, new_suffix, code));
	code = UIOERROR;
	goto done;
    }

    new_vers32 = disk_vers32;
    udb_v32to64(&disk_vers32, &disk_vers);
    if (udb_vcmp64(&new_version, &disk_vers) != 0) {
	ViceLog(0, ("ubik: Cannot install new db for freezeid %llu: version "
		    "mismatch: %lld.%lld != %lld.%lld\n",
		    frz->freezeid,
		    new_version.epoch64.clunks,
		    new_version.counter64,
		    disk_vers.epoch64.clunks,
		    disk_vers.counter64));
	code = UINTERNAL;
	goto done;
    }

    code = udb_check_contents(dbase, path);
    if (code != 0) {
	ViceLog(0, ("ubik: Cannot install new db for freezeid %llu: db does not "
		    "look valid (code %d)\n",
		    frz->freezeid, code));
	code = UIOERROR;
	goto done;
    }

    ViceLog(0, ("ubik: Installing new database %d.%d for freezeid %llu\n",
	    disk_vers32.epoch,
	    disk_vers32.counter,
	    frz->freezeid));

    /*
     * Everything looks good, so go ahead and install the new database. Note
     * that udb_install can theoretically take some time to run (it needs to
     * wait for the transactions on the old db to go away), so we don't hold
     * ufreeze_lock during this.
     */
    code = udb_install(dbase, new_suffix, backup_suffix,
		       &disk_vers32);
    if (code != 0) {
	ViceLog(0, ("ubik: Error %d installing new db for freezeid %llu\n",
		code, frz->freezeid));
	goto done;
    }

    if (keep_old) {
	ViceLog(0, ("ufreeze_install(%s): Installed new db for freezeid %llu. "
		    "Database updated from %d.%d to %d.%d (old db saved to "
		    "%s).\n",
		    caller, frz->freezeid,
		    old_vers32.epoch,
		    old_vers32.counter,
		    new_vers32.epoch,
		    new_vers32.counter,
		    backup_suffix));
    } else {
	frz->unlink_backup = 1;
	ViceLog(0, ("ufreeze_install(%s): Installed new db for freezeid %llu. "
		    "Database updated from %d.%d to %d.%d.\n",
		    caller, frz->freezeid,
		    old_vers32.epoch,
		    old_vers32.counter,
		    new_vers32.epoch,
		    new_vers32.counter));
    }

    frz->db_changed = 1;
    frz->backup_suffix = backup_suffix;
    backup_suffix = NULL;

    /*
     * We've pivoted the new db into place; mark all other sites as having a
     * stale db, so the recovery thread will distribute the new db to other
     * sites (if we don't do it ourselves later during the freeze).
     */

    DBHOLD(dbase);

    others_exist = 0;
    for (ts = ubik_servers; ts; ts = ts->next) {
	others_exist = 1;
	ts->currentDB = 0;
    }
    if (others_exist) {
	/*
	 * Note that we only clear UBIK_RECSENTDB here; we don't do a full
	 * urecovery_ResetState(). We don't need to clear the other urecovery
	 * states, since we know we have the best db version, and we don't need
	 * to relabel the db (we know the installed db has a higher epoch than
	 * the previous db).
	 *
	 * We specifically do _not_ want to clear UBIK_RECLABELDB. If we
	 * cleared that, then on the next commit, we'd relabel the db using
	 * ubik_epochTime, which is _older_ than the epoch of the db we just
	 * installed. That would effectively make the db older, and we don't
	 * want that!
	 */
	urecovery_state &= ~UBIK_RECSENTDB;
    }

    DBRELE(ubik_dbase);

 done:
    free(path);
    free(backup_suffix);
    free(backup_path);
    ufreeze_putfrz(ctl, &frz);
    return code;
}

static int
ufreeze_dist(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    int code;
    struct ufreeze_ctx *frz = NULL;
    int db_disted = 0;
    json_int_t j_freezeid;
    json_error_t jerror;
    afs_uint64 freezeid;
    const char *caller = afsctl_call_describe(ctl);

    code = json_unpack_ex(in_args, &jerror, 0,
			  "{s:I}",
			  "freeze_id", &j_freezeid);
    if (code != 0) {
	ViceLog(0, ("ufreeze_dist(%s): Error unpacking in_args: %s\n",
		caller, jerror.text));
	code = UINTERNAL;
	goto done;
    }

    freezeid = j_freezeid;

    code = ufreeze_getfrz("ufreeze_dist", ctl, freezeid, &frz);
    if (code != 0) {
	goto done;
    }

    if (!frz->db_changed) {
	ViceLog(0, ("ufreeze_dist(%s): Freeze %llu failed; db hasn't "
		    "changed.\n", caller, frz->freezeid));
	code = UINTERNAL;
	goto done;
    }

    DBHOLD(ubik_dbase);

    /* 'db_changed' must be set, from a check above. If db_changed is set,
     * freeze_rw and dbflags_set had better also be set. */
    opr_Assert(frz->freeze_rw);
    opr_Assert(frz->dbflags_set != 0);

    /* Switch from DBRECEIVING to DBSENDING. Do this atomically under DBHOLD,
     * so we still prevent anyone from starting a write tx. */
    ubik_clear_db_flags(ubik_dbase, frz->dbflags_set);
    ubik_set_db_flags(ubik_dbase, DBSENDING);
    frz->dbflags_set = DBSENDING;

    db_disted = 1;

    if (ubik_servers == NULL) {
	/*
	 * No other sites exist, so we don't need to distribute the db to
	 * anyone, so there's no real work to do. We still clear db_changed
	 * below, to make it so we still avoid reverting the db back to the
	 * original db if the freeze fails.
	 */
	ViceLog(0, ("ufreeze_dist(%s): Marking newly-installed db for freezeid "
		    "%llu as distributed (we are the only site).\n",
		    caller, frz->freezeid));

    } else {
	int n_sent = 0;

	ViceLog(0, ("ufreeze_dist(%s): Distributing newly-installed db for freezeid "
		    "%llu.\n", caller, frz->freezeid));

	/*
	 * Distribution may fail to some sites. That's fine; the normal
	 * recovery thread will handle them eventually. But if we failed to
	 * send the db to some sites, return an error to the caller, so they
	 * know something went wrong. The caller can then decide whether or not
	 * they care.
	 *
	 * If we failed to send to _all_ sites, and didn't successfully send
	 * the db to anyone, allow the local db to be reverted if this freeze
	 * later aborts.
	 */
	code = urecovery_distribute_db(ubik_dbase, &n_sent);
	if (code != 0) {
	    ViceLog(0, ("ubik: Failed to distribute db for freezeid %llu to all "
		    "sites (code %d, n_sent %d).\n",
		    frz->freezeid, code, n_sent));
	    code = USYNC;
	    if (n_sent == 0) {
		db_disted = 0;
	    }
	} else {
	    ViceLog(0, ("ubik: Finished distributing db for freezeid %llu.\n",
			frz->freezeid));
	}
    }

    DBRELE(ubik_dbase);

    if (db_disted) {
	/*
	 * We've transmitted our new db to some other sites. So from now on,
	 * act like the db hasn't changed, to prevent someone from running
	 * FreezeDistribute again, and to make sure we don't try to revert the
	 * db back to the original version.
	 */
	frz->db_changed = 0;
    }

 done:
    ufreeze_putfrz(ctl, &frz);
    return code;
}

static struct afsctl_server_method ufreeze_methods[] = {
    { .name = "ufreeze.freeze",	    .func = ufreeze_freezedb },
    { .name = "ufreeze.end",	    .func = ufreeze_end },
    { .name = "ufreeze.install",    .func = ufreeze_install },
    { .name = "ufreeze.dist",	    .func = ufreeze_dist },
    {0}
};

void
ufreeze_Init(struct ubik_serverinit_opts *opts)
{
    int code;

    opr_mutex_init(&ufreeze_lock);
    opr_cv_init(&ufreeze_cv);

    if (opts->ctl_server == NULL) {
	return;
    }

    code = afsctl_server_reg(opts->ctl_server, ufreeze_methods);
    if (code != 0) {
	ViceLog(0, ("ubik: Failed to register ufreeze ctl ops (error %d); "
		"startup wil continue, but freeze functionality will be "
		"unavailable.\n", code));
    }
}

#endif /* AFS_CTL_ENV */

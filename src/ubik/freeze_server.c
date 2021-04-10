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

    int ended;	/**< Has the freeze ended? (that is, someone called
		 *   ufreeze_end, or the caller of ufreeze_freezedb died) */

    int successful; /**< Did the freeze end successfully? (that is, someone
		     *   called ufreeze_end without aborting) */

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
 * End the given freeze. Clear our db flags, clear ufreeze_active_frz, etc.
 *
 * @pre ufreeze_lock held
 */
static void
frz_end(struct ufreeze_ctx *frz)
{
    if (frz->ended) {
	return;
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

    frz_end(frz);

    free(frz);
}

/**
 * Get the current freeze context.
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
			  "{s:b, s?:i, s?:b}",
			  "need_sync", &need_sync,
			  "timeout_ms", &timeout_ms,
			  "no_timeout", &no_timeout);
    if (code != 0) {
	ViceLog(0, ("ufreeze_freezedb(%s): Error unpacking in_args: %s\n",
		caller, jerror.text));
	return UINTERNAL;
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

    frz->dbflags_set = DBSENDING;
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

    code = uphys_getlabel(ubik_dbase, 0, &disk_vers32);
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

    if (!frz->ended) {
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

    frz_end(frz);

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

    if (frz->ended) {
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
    frz_end(frz);

 done:
    opr_mutex_exit(&ufreeze_lock);
    return code;
}

static struct afsctl_server_method ufreeze_methods[] = {
    { .name = "ufreeze.freeze", .func = ufreeze_freezedb },
    { .name = "ufreeze.end",	.func = ufreeze_end },
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

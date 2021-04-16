/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>

#include <lock.h>
#include <rx/rx.h>
#include <afs/cellconfig.h>
#include <afs/afsutil.h>
#include <afs/okv.h>
#ifdef AFS_CTL_ENV
# include <afs/afsctl.h>
#endif


#include "ubik_internal.h"

#include <lwp.h>   /* temporary hack by klm */

#define ERROR_EXIT(code) do { \
    error = (code); \
    goto error_exit; \
} while (0)

/*!
 * \file
 * This system is organized in a hierarchical set of related modules.  Modules
 * at one level can only call modules at the same level or below.
 *
 * At the bottom level (0) we have R, RFTP, LWP and IOMGR, i.e. the basic
 * operating system primitives.
 *
 * At the next level (1) we have
 *
 * \li VOTER--The module responsible for casting votes when asked.  It is also
 * responsible for determining whether this server should try to become
 * a synchronization site.
 * \li BEACONER--The module responsible for sending keep-alives out when a
 * server is actually the sync site, or trying to become a sync site.
 * \li DISK--The module responsible for representing atomic transactions
 * on the local disk.  It maintains a new-value only log.
 * \li LOCK--The module responsible for locking byte ranges in the database file.
 *
 * At the next level (2) we have
 *
 * \li RECOVERY--The module responsible for ensuring that all members of a quorum
 * have the same up-to-date database after a new synchronization site is
 * elected.  This module runs only on the synchronization site.
 *
 * At the next level (3) we have
 *
 * \li REMOTE--The module responsible for interpreting requests from the sync
 * site and applying them to the database, after obtaining the appropriate
 * locks.
 *
 * At the next level (4) we have
 *
 * \li UBIK--The module users call to perform operations on the database.
 */


/* some globals */
afs_int32 ubik_quorum = 0;
struct ubik_dbase *ubik_dbase = 0;
struct ubik_stats ubik_stats;
afs_uint32 ubik_host[UBIK_MAX_INTERFACE_ADDR];
afs_int32 urecovery_state = 0;
int (*ubik_SyncWriterCacheProc) (void);
struct ubik_server *ubik_servers;
short ubik_callPortal;

/* These global variables were used to control the server security layers.
 * They are retained for backwards compatibility with legacy callers.
 *
 * The ubik_SetServerSecurityProcs() interface should be used instead.
 */

int (*ubik_SRXSecurityProc) (void *, struct rx_securityClass **, afs_int32 *);
void *ubik_SRXSecurityRock;
int (*ubik_CheckRXSecurityProc) (void *, struct rx_call *);
void *ubik_CheckRXSecurityRock;



static int BeginTrans(struct ubik_dbase *dbase, afs_int32 transMode,
	   	      struct ubik_trans **transPtr, int readAny);

static struct rx_securityClass **ubik_sc = NULL;
static void (*buildSecClassesProc)(void *, struct rx_securityClass ***,
				   afs_int32 *) = NULL;
static int (*checkSecurityProc)(void *, struct rx_call *) = NULL;
static void *securityRock = NULL;

struct version_data version_globals;

#define	CStampVersion	    1	/* meaning set ts->version */
#define	CCheckSyncAdvertised	    2	/* check if the remote knows we are the sync-site */

static_inline struct rx_connection *
Quorum_StartIO(struct ubik_trans *atrans, struct ubik_server *as)
{
    struct rx_connection *conn;

    UBIK_ADDR_LOCK;
    conn = as->disk_rxcid;

#ifdef AFS_PTHREAD_ENV
    rx_GetConnection(conn);
    UBIK_ADDR_UNLOCK;
    DBRELE(atrans->dbase);
#else
    UBIK_ADDR_UNLOCK;
#endif /* AFS_PTHREAD_ENV */

    return conn;
}

static_inline void
Quorum_EndIO(struct ubik_trans *atrans, struct rx_connection *aconn)
{
#ifdef AFS_PTHREAD_ENV
    DBHOLD(atrans->dbase);
    rx_PutConnection(aconn);
#endif /* AFS_PTHREAD_ENV */
}


/*
 * Iterate over all servers.  Callers pass in *ts which is used to track
 * the current server.
 * - Returns 1 if there are no more servers
 * - Returns 0 with conn set to the connection for the current server if
 *   it's up and current
 */
static int
ContactQuorum_iterate(struct ubik_trans *atrans, int aflags, struct ubik_server **ts,
			 struct rx_connection **conn, afs_int32 *rcode,
			 afs_int32 *okcalls, afs_int32 code, const char *procname)
{
    if (!*ts) {
	/* Initial call - start iterating over servers */
	*ts = ubik_servers;
	*conn = NULL;
	*rcode = 0;
	*okcalls = 0;
    } else {
	if (*conn) {
	    Quorum_EndIO(atrans, *conn);
	    *conn = NULL;
	    if (code) {		/* failure */
		char hoststr[16];

		*rcode = code;
		UBIK_BEACON_LOCK;
		(*ts)->up = 0;		/* mark as down now; beacons will no longer be sent */
		(*ts)->beaconSinceDown = 0;
		UBIK_BEACON_UNLOCK;
		(*ts)->currentDB = 0;
		urecovery_LostServer(*ts);	/* tell recovery to try to resend dbase later */
		ViceLog(0, ("Server %s is marked down due to %s code %d\n",
			    afs_inet_ntoa_r((*ts)->addr[0], hoststr), procname, *rcode));
	    } else {		/* success */
		if (!(*ts)->isClone)
		    (*okcalls)++;	/* count up how many worked */
		if (aflags & CStampVersion) {
		    (*ts)->version = atrans->dbase->version;
		}
	    }
	}
	*ts = (*ts)->next;
    }
    if (!(*ts))
	return 1;
    UBIK_BEACON_LOCK;
    if (!(*ts)->up || !(*ts)->currentDB ||
	/* do not call DISK_Begin until we know that lastYesState is set on the
	 * remote in question; otherwise, DISK_Begin will fail. */
	((aflags & CCheckSyncAdvertised) && !((*ts)->beaconSinceDown && (*ts)->lastVote))) {
	UBIK_BEACON_UNLOCK;
	(*ts)->currentDB = 0;	/* db is no longer current; we just missed an update */
	return 0;		/* not up-to-date, don't bother.  NULL conn will tell caller not to use */
    }
    UBIK_BEACON_UNLOCK;
    *conn = Quorum_StartIO(atrans, *ts);
    return 0;
}

static int
ContactQuorum_rcode(int okcalls, afs_int32 rcode)
{
    /*
     * return 0 if we successfully contacted a quorum, otherwise return error code.
     * We don't have to contact ourselves (that was done locally)
     */
    if (okcalls + 1 >= ubik_quorum)
	return 0;
    else
	return (rcode != 0) ? rcode : UNOQUORUM;
}

/*!
 * \brief Perform an operation at a quorum, handling error conditions.
 * \return 0 if all worked and a quorum was contacted successfully
 * \return otherwise mark failing server as down and return #UERROR
 *
 * \note If any server misses an update, we must wait #BIGTIME seconds before
 * allowing the transaction to commit, to ensure that the missing and
 * possibly still functioning server times out and stops handing out old
 * data.  This is done in the commit code, where we wait for a server marked
 * down to have stayed down for #BIGTIME seconds before we allow a transaction
 * to commit.  A server that fails but comes back up won't give out old data
 * because it is sent the sync count along with the beacon message that
 * marks it as \b really up (\p beaconSinceDown).
 */
static afs_int32
ContactQuorum_NoArguments(afs_int32 (*proc)(struct rx_connection *, ubik_tid *),
			  struct ubik_trans *atrans, int aflags, const char *procname)
{
    struct ubik_server *ts = NULL;
    afs_int32 code = 0, rcode, okcalls;
    struct rx_connection *conn;
    int done;

    done = ContactQuorum_iterate(atrans, aflags, &ts, &conn, &rcode, &okcalls, code, procname);
    while (!done) {
	if (conn)
	    code = (*proc)(conn, &atrans->tid);
	done = ContactQuorum_iterate(atrans, aflags, &ts, &conn, &rcode, &okcalls, code, procname);
    }
    return ContactQuorum_rcode(okcalls, rcode);
}


static afs_int32
ContactQuorum_DISK_Lock(struct ubik_trans *atrans, int aflags,afs_int32 file,
			afs_int32 position, afs_int32 length, afs_int32 type)
{
    struct ubik_server *ts = NULL;
    afs_int32 code = 0, rcode, okcalls;
    struct rx_connection *conn;
    int done;
    char *procname = "DISK_Lock";

    done = ContactQuorum_iterate(atrans, aflags, &ts, &conn, &rcode, &okcalls, code, procname);
    while (!done) {
	if (conn)
	    code = DISK_Lock(conn, &atrans->tid, file, position, length, type);
	done = ContactQuorum_iterate(atrans, aflags, &ts, &conn, &rcode, &okcalls, code, procname);
    }
    return ContactQuorum_rcode(okcalls, rcode);
}

static afs_int32
ContactQuorum_DISK_WriteV(struct ubik_trans *atrans, int aflags,
			  iovec_wrt * io_vector, iovec_buf *io_buffer)
{
    struct ubik_server *ts = NULL;
    afs_int32 code = 0, rcode, okcalls;
    struct rx_connection *conn;
    int done;
    char *procname = "DISK_WriteV";

    done = ContactQuorum_iterate(atrans, aflags, &ts, &conn, &rcode, &okcalls, code, procname);
    while (!done) {
	if (conn) {
	    procname = "DISK_WriteV";	/* in case previous fallback to DISK_Write */
	    code = DISK_WriteV(conn, &atrans->tid, io_vector, io_buffer);
	    if ((code <= -450) && (code > -500)) {
		/* An RPC interface mismatch (as defined in comerr/error_msg.c).
		 * Un-bulk the entries and do individual DISK_Write calls
		 * instead of DISK_WriteV.
		 */
		struct ubik_iovec *iovec = io_vector->val;
		char *iobuf = io_buffer->val;
		bulkdata tcbs;
		afs_int32 i, offset;

		procname = "DISK_Write";	/* for accurate error msg, if any */
		for (i = 0, offset = 0; i < io_vector->len; i++) {
		    /* Sanity check for going off end of buffer */
		    if ((offset + iovec[i].length) > io_buffer->len) {
			code = UINTERNAL;
			break;
		    }
		    tcbs.len = iovec[i].length;
		    tcbs.val = &iobuf[offset];
		    code = DISK_Write(conn, &atrans->tid, iovec[i].file,
			   iovec[i].position, &tcbs);
		    if (code)
			break;
		    offset += iovec[i].length;
		}
	    }
	}
	done = ContactQuorum_iterate(atrans, aflags, &ts, &conn, &rcode, &okcalls, code, procname);
    }
    return ContactQuorum_rcode(okcalls, rcode);
}


afs_int32
ContactQuorum_DISK_SetVersion(struct ubik_trans *atrans, int aflags,
			      ubik_version *OldVersion,
			      ubik_version *NewVersion)
{
    struct ubik_server *ts = NULL;
    afs_int32 code = 0, rcode, okcalls;
    struct rx_connection *conn;
    int done;
    char *procname = "DISK_SetVersion";

    done = ContactQuorum_iterate(atrans, aflags, &ts, &conn, &rcode, &okcalls, code, procname);
    while (!done) {
	if (conn)
	    code = DISK_SetVersion(conn, &atrans->tid, OldVersion, NewVersion);
	done = ContactQuorum_iterate(atrans, aflags, &ts, &conn, &rcode, &okcalls, code, procname);
    }
    return ContactQuorum_rcode(okcalls, rcode);
}

#ifdef AFS_CTL_ENV
static int
uctl_dbinfo(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    struct ubik_dbase *dbase = ubik_dbase;
    char *dbtype;
    char *engine;
    char *desc;
    char *path = NULL;
    struct ubik_version disk_vers32;
    struct ubik_version64 version;
    afs_int64 size_val;
    struct ubik_stat ustat;
    json_error_t jerror;
    int code;

    memset(&disk_vers32, 0, sizeof(disk_vers32));
    memset(&version, 0, sizeof(version));
    memset(&ustat, 0, sizeof(ustat));

    DBHOLD(dbase);

    code = udb_getlabel_db(dbase, &disk_vers32);
    if (code != 0) {
	ViceLog(0, ("uctl_dbinfo: Error %d getting db label\n", code));
	goto done_locked;
    }

    udb_v32to64(&disk_vers32, &version);

    code = udb_path(dbase, NULL, &path);
    if (code != 0) {
	goto done_locked;
    }

    code = udb_stat(path, &ustat);
    if (code != 0) {
	ViceLog(0, ("uctl_dbinfo: Error %d stating db\n", code));
	goto done_locked;
    }

    if (ustat.kv) {
	dbtype = "kv";
	engine = okv_dbhandle_engine(dbase->kv_dbh);
	desc = okv_dbhandle_descr(dbase->kv_dbh);
	size_val = ustat.n_items;

    } else {
	dbtype = "flat";
	engine = "udisk";
	desc = "traditional udisk/uphys storage";
	size_val = ustat.size;
    }

    DBRELE(dbase);

    *out_args = json_pack_ex(&jerror, 0,
			     "{s:s, s:{s:s, s:s}, s:I, s:{s:I, s:I}}",
			     "type", dbtype,
			     "engine", "name", engine,
				       "desc", desc,
			     "size", (json_int_t)size_val,
			     "version", "epoch64", (json_int_t)version.epoch64.clunks,
					"counter", (json_int_t)version.counter64);
    if (*out_args == NULL) {
	ViceLog(0, ("uctl_dbinfo: Error json_pack_ex failed: %s\n", jerror.text));
	code = EIO;
	goto done;
    }

 done:
    free(path);
    return code;

 done_locked:
    DBRELE(dbase);
    goto done;
}

static struct afsctl_server_method uctl_methods[] = {
    { .name = "ubik.dbinfo", .func = uctl_dbinfo },
    {0}
};

static void
uctl_Init(struct ubik_serverinit_opts *opts)
{
    int code;
    if (opts->ctl_server != NULL) {
	code = afsctl_server_reg(opts->ctl_server, uctl_methods);
	if (code != 0) {
	    ViceLog(0, ("ubik: Failed to register ubik ctl ops (error %d); ctl "
		    "functionality will be unavailable.\n", code));
	}
    }
}
#endif /* AFS_CTL_ENV */

#if defined(AFS_PTHREAD_ENV)
static int
ubik_thread_create(pthread_attr_t *tattr, pthread_t *thread, void *proc) {
    opr_Verify(pthread_attr_init(tattr) == 0);
    opr_Verify(pthread_attr_setdetachstate(tattr,
					   PTHREAD_CREATE_DETACHED) == 0);
    opr_Verify(pthread_create(thread, tattr, proc, NULL) == 0);
    return 0;
}
#endif

static void
init_locks(struct ubik_dbase *dbase)
{
#ifdef AFS_PTHREAD_ENV
    opr_mutex_init(&dbase->versionLock);
    opr_cv_init(&dbase->flags_cond);
#else
    Lock_Init(&dbase->versionLock);
#endif
}

/*!
 * \brief This routine initializes the ubik system for a set of servers.
 * \return 0 for success, or an error code on failure.
 *
 * \see ubik_ServerInit(), ubik_ServerInitByInfo(), struct ubik_serverinit_opts
 */
int
ubik_ServerInitByOpts(struct ubik_serverinit_opts *opts,
		      struct ubik_dbase **dbase)
{
    struct ubik_dbase *tdb;
    afs_int32 code;
#ifdef AFS_PTHREAD_ENV
    pthread_t rxServerThread;        /* pthread variables */
    pthread_t ubeacon_InteractThread;
    pthread_t urecovery_InteractThread;
    pthread_attr_t rxServer_tattr;
    pthread_attr_t ubeacon_Interact_tattr;
    pthread_attr_t urecovery_Interact_tattr;
#else
    PROCESS junk;
    extern int rx_stackSize;
#endif

    afs_int32 secIndex;
    struct rx_securityClass *secClass;
    int numClasses;

    struct rx_service *tservice;

    initialize_U_error_table();

    tdb = calloc(1, sizeof(*tdb));
    tdb->pathName = strdup(opts->pathName);
    {
	char *base = strdup(tdb->pathName);
	tdb->pathBase = strdup(basename(base));
	free(base);
    }
    tdb->dbcheck_func = opts->dbcheck_func;
    init_locks(tdb);
#ifdef AFS_PTHREAD_ENV
    opr_mutex_init(&beacon_globals.beacon_lock);
    opr_mutex_init(&vote_globals.vote_lock);
    opr_mutex_init(&addr_globals.addr_lock);
    opr_mutex_init(&version_globals.version_lock);
#endif
    Lock_Init(&tdb->cache_lock);
    *dbase = tdb;
    ubik_dbase = tdb;		/* for now, only one db per server; can fix later when we have names for the other dbases */

    /* initialize RX */

    /* the following call is idempotent so when/if it got called earlier,
     * by whatever called us, it doesn't really matter -- klm */
    code = rx_Init(opts->myPort);
    if (code < 0)
	return code;

    ubik_callPortal = opts->myPort;

    code = ukv_init(tdb, opts->default_kv);
    if (code)
	return code;

    udisk_Init(ubik_nBuffers);
    ulock_Init();

    code = uvote_Init();
    if (code)
	return code;
    code = urecovery_Initialize(tdb);
    if (code)
	return code;
    if (opts->info)
	code = ubeacon_InitServerListByInfo(opts->myHost, opts->info,
					    opts->clones, opts->configDir);
    else
	code = ubeacon_InitServerList(opts->myHost, opts->serverList,
				      opts->configDir);
    if (code)
	return code;

    /* try to get an additional security object */
    if (buildSecClassesProc == NULL) {
	numClasses = 3;
	ubik_sc = calloc(numClasses, sizeof(struct rx_securityClass *));
	ubik_sc[0] = rxnull_NewServerSecurityObject();
	if (ubik_SRXSecurityProc) {
	    code = (*ubik_SRXSecurityProc) (ubik_SRXSecurityRock,
					    &secClass,
					    &secIndex);
	    if (code == 0) {
		 ubik_sc[secIndex] = secClass;
	    }
	}
    } else {
        (*buildSecClassesProc) (securityRock, &ubik_sc, &numClasses);
    }
    /* for backwards compat this should keep working as it does now
       and not host bind */

    tservice =
	rx_NewService(0, VOTE_SERVICE_ID, "VOTE", ubik_sc, numClasses,
		      VOTE_ExecuteRequest);
    if (tservice == (struct rx_service *)0) {
	ViceLog(0, ("Could not create VOTE rx service!\n"));
	return -1;
    }
    rx_SetMinProcs(tservice, 2);
    rx_SetMaxProcs(tservice, 3);

    tservice =
	rx_NewService(0, DISK_SERVICE_ID, "DISK", ubik_sc, numClasses,
		      DISK_ExecuteRequest);
    if (tservice == (struct rx_service *)0) {
	ViceLog(0, ("Could not create DISK rx service!\n"));
	return -1;
    }
    rx_SetMinProcs(tservice, 2);
    rx_SetMaxProcs(tservice, 3);

    /* start an rx_ServerProc to handle incoming RPC's in particular the
     * UpdateInterfaceAddr RPC that occurs in ubeacon_InitServerList. This avoids
     * the "steplock" problem in ubik initialization. Defect 11037.
     */
#ifdef AFS_PTHREAD_ENV
    ubik_thread_create(&rxServer_tattr, &rxServerThread, (void *)rx_ServerProc);
#else
    LWP_CreateProcess(rx_ServerProc, rx_stackSize, RX_PROCESS_PRIORITY,
              NULL, "rx_ServerProc", &junk);
#endif

    /* send addrs to all other servers */
    code = ubeacon_updateUbikNetworkAddress(ubik_host);
    if (code)
	return code;

    /* now start up async processes */
#ifdef AFS_PTHREAD_ENV
    ubik_thread_create(&ubeacon_Interact_tattr, &ubeacon_InteractThread,
		(void *)ubeacon_Interact);
#else
    code = LWP_CreateProcess(ubeacon_Interact, 16384 /*8192 */ ,
			     LWP_MAX_PRIORITY - 1, (void *)0, "beacon",
			     &junk);
    if (code)
	return code;
#endif

#ifdef AFS_PTHREAD_ENV
    ubik_thread_create(&urecovery_Interact_tattr, &urecovery_InteractThread,
		(void *)urecovery_Interact);
#else
    code = LWP_CreateProcess(urecovery_Interact, 16384 /*8192 */ ,
			     LWP_MAX_PRIORITY - 1, (void *)0, "recovery",
			     &junk);
    if (code)
	return code;
#endif

#ifdef AFS_CTL_ENV
    uctl_Init(opts);
    ufreeze_Init(opts);
#endif

    return 0;
}

/*!
 * \see ubik_ServerInitByOpts()
 */
int
ubik_ServerInitByInfo(afs_uint32 myHost, short myPort,
		      struct afsconf_cell *info, char clones[],
		      const char *pathName, struct ubik_dbase **dbase)
{
    struct ubik_serverinit_opts opts;
    memset(&opts, 0, sizeof(opts));

    opts.myHost = myHost;
    opts.myPort = myPort;
    opts.info = info;
    opts.clones = clones;
    opts.pathName = pathName;

    return ubik_ServerInitByOpts(&opts, dbase);
}

/*!
 * \see ubik_ServerInitByOpts()
 */
int
ubik_ServerInit(afs_uint32 myHost, short myPort, afs_uint32 serverList[],
		const char *pathName, struct ubik_dbase **dbase)
{
    struct ubik_serverinit_opts opts;
    memset(&opts, 0, sizeof(opts));

    opts.myHost = myHost;
    opts.myPort = myPort;
    opts.serverList = serverList;
    opts.pathName = pathName;

    return ubik_ServerInitByOpts(&opts, dbase);
}

static int
BeginTransRaw(struct ubik_dbase *dbase, afs_int32 transMode,
	      struct ubik_trans **transPtr, int readAny)
{
    struct ubik_trans *trans;

    if (transMode != UBIK_READTRANS) {
	if (readAny || !dbase->raw_rw)
	    return UBADTYPE;
    }

    trans = calloc(1, sizeof(*trans));
    if (trans == NULL) {
	return UNOMEM;
    }

    trans->dbase = dbase;
    trans->type = transMode;
    trans->flags |= TRRAW;
    if (ubik_KVDbase(dbase)) {
	int code;
	trans->flags |= TRKEYVAL;
	trans->kv_dbh = okv_dbhandle_ref(dbase->kv_dbh);
	code = ukv_begin(trans, &trans->kv_tx);
	if (code != 0) {
	    ubik_AbortTrans(trans);
	    return code;
	}
    }

    *transPtr = trans;
    return 0;
}

/*!
 * \brief This routine begins a read or write transaction on the transaction
 * identified by transPtr, in the dbase named by dbase.
 *
 * An open mode of ubik_READTRANS identifies this as a read transaction,
 * while a mode of ubik_WRITETRANS identifies this as a write transaction.
 * transPtr is set to the returned transaction control block.
 * The readAny flag is set to 0 or 1 or 2 by the wrapper functions
 * ubik_BeginTrans() or ubik_BeginTransReadAny() or
 * ubik_BeginTransReadAnyWrite() below.
 *
 * \note We can only begin transaction when we have an up-to-date database.
 */
static int
BeginTrans(struct ubik_dbase *dbase, afs_int32 transMode,
	   struct ubik_trans **transPtr, int readAny)
{
    struct ubik_trans *jt;
    struct ubik_trans *tt;
    afs_int32 code;

    if (ubik_RawDbase(dbase)) {
	return BeginTransRaw(dbase, transMode, transPtr, readAny);
    }

    if (readAny > 1 && ubik_SyncWriterCacheProc == NULL) {
	/* it's not safe to use ubik_BeginTransReadAnyWrite without a
	 * cache-syncing function; fall back to ubik_BeginTransReadAny,
	 * which is safe but slower */
	ViceLog(0, ("ubik_BeginTransReadAnyWrite called, but "
	           "ubik_SyncWriterCacheProc not set; pretending "
	           "ubik_BeginTransReadAny was called instead\n"));
	readAny = 1;
    }

    if ((transMode != UBIK_READTRANS) && readAny)
	return UBADTYPE;
    DBHOLD(dbase);
    if (urecovery_AllBetter(dbase, readAny) == 0) {
	DBRELE(dbase);
	return UNOQUORUM;
    }
    /* otherwise we have a quorum, use it */

    /* make sure that at most one write transaction occurs at any one time.  This
     * has nothing to do with transaction locking; that's enforced by the lock package.  However,
     * we can't even handle two non-conflicting writes, since our log and recovery modules
     * don't know how to restore one without possibly picking up some data from the other. */
    if (transMode == UBIK_WRITETRANS) {
	/* if we're writing, sending, or receiving a database, wait */
	code = ubik_wait_db_flags(dbase, DBWRITING | DBSENDING | DBRECEIVING);
	osi_Assert(code == 0);

	if (!ubeacon_AmSyncSite()) {
	    DBRELE(dbase);
	    return UNOTSYNC;
	}
	if (!ubeacon_SyncSiteAdvertised()) {
	    /* i am the sync-site but the remotes are not aware yet */
	    DBRELE(dbase);
	    return UNOQUORUM;
	}
    }

    /* create the transaction */
    code = udisk_begin(dbase, transMode, &jt);	/* can't take address of register var */
    tt = jt;			/* move to a register */
    if (code || tt == NULL) {
	DBRELE(dbase);
	return code;
    }
    UBIK_VERSION_LOCK;
    if (readAny) {
	tt->flags |= TRREADANY;
	if (readAny > 1) {
	    tt->flags |= TRREADWRITE;
	}
    }
    /* label trans and dbase with new tid */
    tt->tid.epoch = version_globals.ubik_epochTime;
    /* bump by two, since tidCounter+1 means trans id'd by tidCounter has finished */
    tt->tid.counter = (version_globals.tidCounter += 2);

    if (transMode == UBIK_WRITETRANS) {
	/* for a write trans, we have to keep track of the write tid counter too */
	version_globals.writeTidCounter = tt->tid.counter;
    }

    UBIK_VERSION_UNLOCK;

    if (transMode == UBIK_WRITETRANS) {
	/* next try to start transaction on appropriate number of machines */
	code = ContactQuorum_NoArguments(DISK_Begin, tt, CCheckSyncAdvertised, "DISK_Begin");
	if (code) {
	    /* we must abort the operation */
	    udisk_abort(tt);
	    /* force aborts to the others */
	    ContactQuorum_NoArguments(DISK_Abort, tt, 0, "DISK_Abort");
	    udisk_end(tt);
	    DBRELE(dbase);
	    return code;
	}
    }

    *transPtr = tt;
    DBRELE(dbase);
    return 0;
}

/*!
 * \see BeginTrans()
 */
int
ubik_BeginTrans(struct ubik_dbase *dbase, afs_int32 transMode,
		struct ubik_trans **transPtr)
{
    return BeginTrans(dbase, transMode, transPtr, 0);
}

/*!
 * \see BeginTrans()
 */
int
ubik_BeginTransReadAny(struct ubik_dbase *dbase, afs_int32 transMode,
		       struct ubik_trans **transPtr)
{
    return BeginTrans(dbase, transMode, transPtr, 1);
}

/*!
 * \see BeginTrans()
 */
int
ubik_BeginTransReadAnyWrite(struct ubik_dbase *dbase, afs_int32 transMode,
                            struct ubik_trans **transPtr)
{
    return BeginTrans(dbase, transMode, transPtr, 2);
}

/*!
 * \brief This routine ends a read or write transaction by aborting it.
 */
int
ubik_AbortTrans(struct ubik_trans *transPtr)
{
    afs_int32 code;
    afs_int32 code2;
    struct ubik_dbase *dbase;

    if (ubik_RawTrans(transPtr)) {
	okv_abort(&transPtr->kv_tx);
	okv_dbhandle_rele(&transPtr->kv_dbh);
	free(transPtr);
	return 0;
    }

    dbase = transPtr->dbase;

    if (transPtr->flags & TRCACHELOCKED) {
	ReleaseReadLock(&dbase->cache_lock);
	transPtr->flags &= ~TRCACHELOCKED;
    }

    ObtainWriteLock(&dbase->cache_lock);

    DBHOLD(dbase);
    memset(&dbase->cachedVersion, 0, sizeof(struct ubik_version));

    ReleaseWriteLock(&dbase->cache_lock);

    /* see if we're still up-to-date */
    if (!urecovery_AllBetter(dbase, transPtr->flags & TRREADANY)) {
	udisk_abort(transPtr);
	udisk_end(transPtr);
	DBRELE(dbase);
	return UNOQUORUM;
    }

    if (transPtr->type == UBIK_READTRANS) {
	code = udisk_abort(transPtr);
	udisk_end(transPtr);
	DBRELE(dbase);
	return code;
    }

    /* below here, we know we're doing a write transaction */
    if (!ubeacon_AmSyncSite()) {
	udisk_abort(transPtr);
	udisk_end(transPtr);
	DBRELE(dbase);
	return UNOTSYNC;
    }

    /* now it is safe to try remote abort */
    code = ContactQuorum_NoArguments(DISK_Abort, transPtr, 0, "DISK_Abort");
    code2 = udisk_abort(transPtr);
    udisk_end(transPtr);
    DBRELE(dbase);
    return (code ? code : code2);
}

static void
WritebackApplicationCache(struct ubik_dbase *dbase)
{
    int code = 0;
    if (ubik_SyncWriterCacheProc) {
	code = ubik_SyncWriterCacheProc();
    }
    if (code) {
	/* we failed to sync the local cache, so just invalidate the cache;
	 * we'll try to read the cache in again on the next read */
	memset(&dbase->cachedVersion, 0, sizeof(dbase->cachedVersion));
    } else {
	memcpy(&dbase->cachedVersion, &dbase->version,
	       sizeof(dbase->cachedVersion));
    }
}

static int
EndTransRaw(struct ubik_trans *transPtr)
{
    struct ubik_version version;
    int code;

    memset(&version, 0, sizeof(version));

    if (transPtr->type == UBIK_READTRANS) {
	return ubik_AbortTrans(transPtr);
    }

    code = ubik_RawGetVersion(transPtr, &version);
    if (code != 0) {
	goto error;
    }

    if (version.epoch == 0 || version.counter == 0) {
	/* Require a valid version before we can commit. */
	code = UNOQUORUM;
	goto error;
    }

    /*
     * Note that for a KV trans, we don't ukv_commit with a specific version,
     * since we don't set the version for a flatfile raw commit either. We
     * depend on callers setting an explicit version before committing.
     */
    okv_commit(&transPtr->kv_tx);
    okv_dbhandle_rele(&transPtr->kv_dbh);
    free(transPtr);
    return 0;

 error:
    ubik_AbortTrans(transPtr);
    return code;
}

/*!
 * \brief This routine ends a read or write transaction on the open transaction identified by transPtr.
 * \return an error code.
 */
int
ubik_EndTrans(struct ubik_trans *transPtr)
{
    afs_int32 code;
    struct timeval tv;
    afs_int32 realStart;
    struct ubik_server *ts;
    afs_int32 now;
    int cachelocked = 0;
    struct ubik_dbase *dbase;

    if (ubik_RawTrans(transPtr)) {
	return EndTransRaw(transPtr);
    }

    if (transPtr->type == UBIK_WRITETRANS) {
	code = ubik_Flush(transPtr);
	if (code) {
	    ubik_AbortTrans(transPtr);
	    return (code);
	}
    }

    dbase = transPtr->dbase;

    if (transPtr->flags & TRCACHELOCKED) {
	ReleaseReadLock(&dbase->cache_lock);
	transPtr->flags &= ~TRCACHELOCKED;
    }

    if (transPtr->type != UBIK_READTRANS) {
	/* must hold cache_lock before DBHOLD'ing */
	ObtainWriteLock(&dbase->cache_lock);
	cachelocked = 1;
    }

    DBHOLD(dbase);

    /* give up if no longer current */
    if (!urecovery_AllBetter(dbase, transPtr->flags & TRREADANY)) {
	udisk_abort(transPtr);
	udisk_end(transPtr);
	DBRELE(dbase);
	code = UNOQUORUM;
	goto error;
    }

    if (transPtr->type == UBIK_READTRANS) {	/* reads are easy */
	code = udisk_commit(transPtr);
	if (code == 0)
	    goto success;	/* update cachedVersion correctly */
	udisk_end(transPtr);
	DBRELE(dbase);
	goto error;
    }

    if (!ubeacon_AmSyncSite()) {	/* no longer sync site */
	udisk_abort(transPtr);
	udisk_end(transPtr);
	DBRELE(dbase);
	code = UNOTSYNC;
	goto error;
    }

    /* now it is safe to do commit */
    code = udisk_commit(transPtr);
    if (code == 0) {
	/* db data has been committed locally; update the local cache so
	 * readers can get at it */
	WritebackApplicationCache(dbase);

	ReleaseWriteLock(&dbase->cache_lock);

	code = ContactQuorum_NoArguments(DISK_Commit, transPtr, CStampVersion, "DISK_Commit");

    } else {
	memset(&dbase->cachedVersion, 0, sizeof(struct ubik_version));
	ReleaseWriteLock(&dbase->cache_lock);
    }
    cachelocked = 0;
    if (code) {
	/* failed to commit, so must return failure.  Try to clear locks first, just for fun
	 * Note that we don't know if this transaction will eventually commit at this point.
	 * If it made it to a site that will be present in the next quorum, we win, otherwise
	 * we lose.  If we contact a majority of sites, then we won't be here: contacting
	 * a majority guarantees commit, since it guarantees that one dude will be a
	 * member of the next quorum. */
	ContactQuorum_NoArguments(DISK_ReleaseLocks, transPtr, 0, "DISK_ReleaseLocks");
	udisk_end(transPtr);
	DBRELE(dbase);
	goto error;
    }
    /* before we can start sending unlock messages, we must wait until all servers
     * that are possibly still functioning on the other side of a network partition
     * have timed out.  Check the server structures, compute how long to wait, then
     * start the unlocks */
    realStart = FT_ApproxTime();
    while (1) {
	/* wait for all servers to time out */
	code = 0;
	now = FT_ApproxTime();
	/* check if we're still sync site, the guy should either come up
	 * to us, or timeout.  Put safety check in anyway */
	if (now - realStart > 10 * BIGTIME) {
	    ubik_stats.escapes++;
	    ViceLog(0, ("ubik escaping from commit wait\n"));
	    break;
	}
	for (ts = ubik_servers; ts; ts = ts->next) {
	    UBIK_BEACON_LOCK;
	    if (!ts->beaconSinceDown && now <= ts->lastBeaconSent + BIGTIME) {
		UBIK_BEACON_UNLOCK;

		/* this guy could have some damaged data, wait for him */
		code = 1;
		tv.tv_sec = 1;	/* try again after a while (ha ha) */
		tv.tv_usec = 0;

#ifdef AFS_PTHREAD_ENV
		/* we could release the dbase outside of the loop, but we do
		 * it here, in the loop, to avoid an unnecessary RELE/HOLD
		 * if all sites are up */
		DBRELE(dbase);
		select(0, 0, 0, 0, &tv);
		DBHOLD(dbase);
#else
		IOMGR_Select(0, 0, 0, 0, &tv);	/* poll, should we wait on something? */
#endif

		break;
	    }
	    UBIK_BEACON_UNLOCK;
	}
	if (code == 0)
	    break;		/* no down ones still pseudo-active */
    }

    /* finally, unlock all the dudes.  We can return success independent of the number of servers
     * that really unlock the dbase; the others will do it if/when they elect a new sync site.
     * The transaction is committed anyway, since we succeeded in contacting a quorum
     * at the start (when invoking the DiskCommit function).
     */
    ContactQuorum_NoArguments(DISK_ReleaseLocks, transPtr, 0, "DISK_ReleaseLocks");

  success:
    udisk_end(transPtr);
    /* don't update cachedVersion here; it should have been updated way back
     * in ubik_CheckCache, and earlier in this function for writes */
    DBRELE(dbase);
    if (cachelocked) {
	ReleaseWriteLock(&dbase->cache_lock);
    }
    return 0;

  error:
    if (!cachelocked) {
	ObtainWriteLock(&dbase->cache_lock);
    }
    memset(&dbase->cachedVersion, 0, sizeof(struct ubik_version));
    ReleaseWriteLock(&dbase->cache_lock);
    return code;
}

static int
seek_fread(void *buf, size_t nbytes, FILE *fh, long offset)
{
    size_t bytes_read;

    opr_Assert(fh != NULL);
    if (fseek(fh, offset, SEEK_SET) < 0) {
	return UIOERROR;
    }

    bytes_read = fread(buf, 1, nbytes, fh);
    if (bytes_read == nbytes) {
	return 0;
    }

    if (ferror(fh)) {
	return UIOERROR;
    }

    /*
     * For a short read, blank out the rest of 'buf' that we failed to read in
     * and return success, to match the historical semantics of functions like
     * ubik_Read().
     */
    opr_Assert(bytes_read < nbytes);
    memset((char*)buf + bytes_read, 0, nbytes - bytes_read);

    return 0;
}

static int
seek_fwrite(void *buf, size_t nbytes, FILE *fh, long offset)
{
    opr_Assert(fh != NULL);
    if (fseek(fh, offset, SEEK_SET) < 0) {
	return UIOERROR;
    }
    if (fwrite(buf, 1, nbytes, fh) != nbytes) {
	return UIOERROR;
    }
    return 0;
}

static int
rawtrans_io(int do_write, struct ubik_trans *transPtr, void *buffer,
	    afs_int32 length)
{
    int code;
    if (do_write) {
	code = seek_fwrite(buffer, length, transPtr->dbase->raw_fh,
			   transPtr->seekPos + HDRSIZE);
    } else {
	code = seek_fread(buffer, length, transPtr->dbase->raw_fh,
			  transPtr->seekPos + HDRSIZE);
    }
    if (code == 0) {
	transPtr->seekPos += length;
    }
    return code;
}

/*!
 * \brief This routine reads length bytes into buffer from the current position in the database.
 *
 * The file pointer is updated appropriately (by adding the number of bytes actually transferred), and the length actually transferred is stored in the long integer pointed to by length.  A short read returns zero for an error code.
 *
 * \note *length is an INOUT parameter: at the start it represents the size of the buffer, and when done, it contains the number of bytes actually transferred.
 */
int
ubik_Read(struct ubik_trans *transPtr, void *buffer,
	  afs_int32 length)
{
    afs_int32 code;

    if (ubik_RawTrans(transPtr)) {
	return rawtrans_io(0, transPtr, buffer, length);
    }

    /* reads are easy to do: handle locally */
    DBHOLD(transPtr->dbase);
    if (!urecovery_AllBetter(transPtr->dbase, transPtr->flags & TRREADANY)) {
	DBRELE(transPtr->dbase);
	return UNOQUORUM;
    }

    code =
	udisk_read(transPtr, transPtr->seekFile, buffer, transPtr->seekPos,
		   length);
    if (code == 0) {
	transPtr->seekPos += length;
    }
    DBRELE(transPtr->dbase);
    return code;
}

/*!
 * \brief This routine will flush the io data in the iovec structures.
 *
 * It first flushes to the local disk and then uses ContactQuorum to write it
 * to the other servers.
 */
int
ubik_Flush(struct ubik_trans *transPtr)
{
    afs_int32 code, error = 0;

    if (transPtr->type != UBIK_WRITETRANS)
	return UBADTYPE;

    if (ubik_RawTrans(transPtr) || ubik_KVTrans(transPtr)) {
	return 0;
    }

    DBHOLD(transPtr->dbase);
    if (!transPtr->iovec_info.len || !transPtr->iovec_info.val) {
	DBRELE(transPtr->dbase);
	return 0;
    }

    if (!urecovery_AllBetter(transPtr->dbase, transPtr->flags & TRREADANY))
	ERROR_EXIT(UNOQUORUM);
    if (!ubeacon_AmSyncSite())	/* only sync site can write */
	ERROR_EXIT(UNOTSYNC);

    /* Update the rest of the servers in the quorum */
    code =
	ContactQuorum_DISK_WriteV(transPtr, 0, &transPtr->iovec_info,
				  &transPtr->iovec_data);
    if (code) {
	udisk_abort(transPtr);
	/* force aborts to the others */
	ContactQuorum_NoArguments(DISK_Abort, transPtr, 0, "DISK_Abort");
	transPtr->iovec_info.len = 0;
	transPtr->iovec_data.len = 0;
	ERROR_EXIT(code);
    }

    /* Wrote the buffers out, so start at scratch again */
    transPtr->iovec_info.len = 0;
    transPtr->iovec_data.len = 0;

  error_exit:
    DBRELE(transPtr->dbase);
    return error;
}

int
ubik_Write(struct ubik_trans *transPtr, void *vbuffer,
	   afs_int32 length)
{
    struct ubik_iovec *iovec;
    afs_int32 code, error = 0;
    afs_int32 pos, len, size;
    char * buffer = (char *)vbuffer;

    if (ubik_RawTrans(transPtr)) {
	return rawtrans_io(1, transPtr, buffer, length);
    }

    if (transPtr->type != UBIK_WRITETRANS)
	return UBADTYPE;
    if (!length)
	return 0;

    if (length > IOVEC_MAXBUF) {
	for (pos = 0, len = length; len > 0; len -= size, pos += size) {
	    size = ((len < IOVEC_MAXBUF) ? len : IOVEC_MAXBUF);
	    code = ubik_Write(transPtr, buffer+pos, size);
	    if (code)
		return (code);
	}
	return 0;
    }

    DBHOLD(transPtr->dbase);
    if (!transPtr->iovec_info.val) {
	transPtr->iovec_info.len = 0;
	transPtr->iovec_info.val =
	    malloc(IOVEC_MAXWRT * sizeof(struct ubik_iovec));
	transPtr->iovec_data.len = 0;
	transPtr->iovec_data.val = malloc(IOVEC_MAXBUF);
	if (!transPtr->iovec_info.val || !transPtr->iovec_data.val) {
	    if (transPtr->iovec_info.val)
		free(transPtr->iovec_info.val);
	    transPtr->iovec_info.val = 0;
	    if (transPtr->iovec_data.val)
		free(transPtr->iovec_data.val);
	    transPtr->iovec_data.val = 0;
	    DBRELE(transPtr->dbase);
	    return UNOMEM;
	}
    }

    /* If this write won't fit in the structure, then flush it out and start anew */
    if ((transPtr->iovec_info.len >= IOVEC_MAXWRT)
	|| ((length + transPtr->iovec_data.len) > IOVEC_MAXBUF)) {
	/* Can't hold the DB lock over ubik_Flush */
	DBRELE(transPtr->dbase);
	code = ubik_Flush(transPtr);
	if (code)
	    return (code);
	DBHOLD(transPtr->dbase);
    }

    if (!urecovery_AllBetter(transPtr->dbase, transPtr->flags & TRREADANY))
	ERROR_EXIT(UNOQUORUM);
    if (!ubeacon_AmSyncSite())	/* only sync site can write */
	ERROR_EXIT(UNOTSYNC);

    /* Write to the local disk */
    code =
	udisk_write(transPtr, transPtr->seekFile, buffer, transPtr->seekPos,
		    length);
    if (code) {
	udisk_abort(transPtr);
	transPtr->iovec_info.len = 0;
	transPtr->iovec_data.len = 0;
	DBRELE(transPtr->dbase);
	return (code);
    }

    /* Collect writes for the other ubik servers (to be done in bulk) */
    iovec = transPtr->iovec_info.val;
    iovec[transPtr->iovec_info.len].file = transPtr->seekFile;
    iovec[transPtr->iovec_info.len].position = transPtr->seekPos;
    iovec[transPtr->iovec_info.len].length = length;

    memcpy((char*)transPtr->iovec_data.val + transPtr->iovec_data.len,
	   buffer, length);

    transPtr->iovec_info.len++;
    transPtr->iovec_data.len += length;
    transPtr->seekPos += length;

  error_exit:
    DBRELE(transPtr->dbase);
    return error;
}

/*!
 * \brief This sets the file pointer associated with the current transaction
 * to the appropriate file and byte position.
 *
 * Unlike Unix files, a transaction is labelled by both a file number \p fileid
 * and a byte position relative to the specified file \p position.
 */
int
ubik_Seek(struct ubik_trans *transPtr, afs_int32 fileid,
	  afs_int32 position)
{
    afs_int32 code;

    if (ubik_RawTrans(transPtr)) {
	transPtr->seekFile = fileid;
	transPtr->seekPos = position;
	return 0;
    }

    DBHOLD(transPtr->dbase);
    if (!urecovery_AllBetter(transPtr->dbase, transPtr->flags & TRREADANY)) {
	code = UNOQUORUM;
    } else {
	transPtr->seekFile = fileid;
	transPtr->seekPos = position;
	code = 0;
    }
    DBRELE(transPtr->dbase);
    return code;
}

/*!
 * \brief set a lock; all locks are released on transaction end (commit/abort)
 */
int
ubik_SetLock(struct ubik_trans *atrans, afs_int32 apos, afs_int32 alen,
	     int atype)
{
    afs_int32 code = 0, error = 0;

    if (atype == LOCKWRITE) {
	if (atrans->type == UBIK_READTRANS)
	    return UBADTYPE;
	code = ubik_Flush(atrans);
	if (code)
	    return (code);
    }

    if (ubik_RawTrans(atrans)) {
	return 0;
    }

    DBHOLD(atrans->dbase);
    if (atype == LOCKREAD) {
	code = ulock_getLock(atrans, atype, 1);
	if (code)
	    ERROR_EXIT(code);
    } else {
	/* first, check that quorum is still good, and that dbase is up-to-date */
	if (!urecovery_AllBetter(atrans->dbase, atrans->flags & TRREADANY))
	    ERROR_EXIT(UNOQUORUM);
	if (!ubeacon_AmSyncSite())
	    ERROR_EXIT(UNOTSYNC);

	/* now do the operation locally, and propagate it out */
	code = ulock_getLock(atrans, atype, 1);
	if (code == 0) {
	    code = ContactQuorum_DISK_Lock(atrans, 0, 0, 1 /*unused */ ,
				 	   1 /*unused */ , LOCKWRITE);
	}
	if (code) {
	    /* we must abort the operation */
	    udisk_abort(atrans);
	    /* force aborts to the others */
	    ContactQuorum_NoArguments(DISK_Abort, atrans, 0, "DISK_Abort");
	    ERROR_EXIT(code);
	}
    }

  error_exit:
    DBRELE(atrans->dbase);
    return error;
}

/*!
 * \brief Facility to simplify database caching.
 * \return zero if last trans was done on the local server and was successful.
 * \return -1 means bad (NULL) argument.
 *
 * If return value is non-zero and the caller is a server caching part of the
 * Ubik database, it should invalidate that cache.
 */
static int
ubik_CacheUpdate(struct ubik_trans *atrans)
{
    if (!(atrans && atrans->dbase))
	return -1;
    return vcmp(atrans->dbase->cachedVersion, atrans->dbase->version) != 0;
}

/**
 * check and possibly update cache of ubik db.
 *
 * If the version of the cached db data is out of date, this calls (*check) to
 * update the cache. If (*check) returns success, we update the version of the
 * cached db data.
 *
 * Checking the version of the cached db data is done under a read lock;
 * updating the cache (and thus calling (*check)) is done under a write lock
 * so is guaranteed not to interfere with another thread's (*check). On
 * successful return, a read lock on the cached db data is obtained, which
 * will be released by ubik_EndTrans or ubik_AbortTrans.
 *
 * @param[in] atrans ubik transaction
 * @param[in] check  function to call to check/update cache
 * @param[in] rock   rock to pass to *check
 *
 * @return operation status
 *   @retval 0       success
 *   @retval nonzero error; cachedVersion not updated
 *
 * @post On success, application cache is read-locked, and cache data is
 *       up-to-date
 */
int
ubik_CheckCache(struct ubik_trans *atrans, ubik_updatecache_func cbf, void *rock)
{
    int ret = 0;

    if (!(atrans && atrans->dbase))
	return -1;

    if (ubik_RawTrans(atrans)) {
	return (*cbf)(atrans, rock);
    }

    ObtainReadLock(&atrans->dbase->cache_lock);

    while (ubik_CacheUpdate(atrans) != 0) {

	ReleaseReadLock(&atrans->dbase->cache_lock);
	ObtainSharedLock(&atrans->dbase->cache_lock);

	if (ubik_CacheUpdate(atrans) != 0) {

	    BoostSharedLock(&atrans->dbase->cache_lock);

	    ret = (*cbf) (atrans, rock);
	    if (ret == 0) {
		memcpy(&atrans->dbase->cachedVersion, &atrans->dbase->version,
		       sizeof(atrans->dbase->cachedVersion));
	    }
	}

	/* It would be nice if we could convert from a shared lock to a read
	 * lock... instead, just release the shared and acquire the read */
	ReleaseSharedLock(&atrans->dbase->cache_lock);

	if (ret) {
	    /* if we have an error, don't retry, and don't hold any locks */
	    return ret;
	}

	ObtainReadLock(&atrans->dbase->cache_lock);
    }

    atrans->flags |= TRCACHELOCKED;

    return 0;
}

/*!
 * "Who said anything about panicking?" snapped Arthur.
 * "This is still just the culture shock. You wait till I've settled down
 * into the situation and found my bearings. \em Then I'll start panicking!"
 * --Authur Dent
 *
 * \returns There is no return from panic.
 */
void
panic(char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    ViceLog(0, ("Ubik PANIC:\n"));
    vViceLog(0, (format, ap));
    va_end(ap);

    abort();
    AFS_UNREACHED(ViceLog(0, ("BACK FROM ABORT\n")));
    AFS_UNREACHED(exit(1));
}

/*!
 * This function takes an IP addresses as its parameter. It returns the
 * the primary IP address that is on the host passed in, or 0 if not found.
 */
afs_uint32
ubikGetPrimaryInterfaceAddr(afs_uint32 addr)
{
    struct ubik_server *ts;
    int j;

    UBIK_ADDR_LOCK;
    for (ts = ubik_servers; ts; ts = ts->next)
	for (j = 0; j < UBIK_MAX_INTERFACE_ADDR; j++)
	    if (ts->addr[j] == addr) {
		UBIK_ADDR_UNLOCK;
		return ts->addr[0];	/* net byte order */
	    }
    UBIK_ADDR_UNLOCK;
    return 0;			/* if not in server database, return error */
}

int
ubik_CheckAuth(struct rx_call *acall)
{
    if (checkSecurityProc)
	return (*checkSecurityProc) (securityRock, acall);
    else if (ubik_CheckRXSecurityProc) {
	return (*ubik_CheckRXSecurityProc) (ubik_CheckRXSecurityRock, acall);
    } else
	return 0;
}

void
ubik_SetServerSecurityProcs(void (*buildproc) (void *,
					       struct rx_securityClass ***,
					       afs_int32 *),
			    int (*checkproc) (void *, struct rx_call *),
			    void *rock)
{
    buildSecClassesProc = buildproc;
    checkSecurityProc = checkproc;
    securityRock = rock;
}

/**
 * Make a copy of an on-disk ubik database.
 *
 * @param[in] src_path	Path to source ubik db.
 * @param[in] dest_path	Path to copy db to create.
 *
 * @returns ubik error codes
 */
int
ubik_CopyDB(char *src_path, char *dest_path)
{
    int code;
    int iskv = 0;

    code = udb_dbinfo(src_path, NULL, &iskv, NULL);
    if (code != 0) {
	return code;
    }

    if (iskv) {
	return ukv_copydb(src_path, dest_path);
    } else {
	return uphys_copydb(src_path, dest_path);
    }
}

/**
 * Initialize "raw" access to a ubik database.
 *
 * "Raw" access means we access the given database file(s) directly, without
 * interacting with the ubik distributed system. The intention is for this to
 * be used by utilities for offline access to ubik database files, while still
 * using standard ubik calls like ubik_BeginTrans, ubik_Read, etc.
 *
 * Note that many things are not initialized about the returned ubik_dbase
 * struct, and so not all ubik_* calls will work with the given database.
 *
 * Transactions can be started and ended for this db the normal way (with
 * ubik_BeginTrans and ubik_AbortTrans/ubik_EndTrans), but note that i/o is
 * _not_ transactional. That is, data is written immediately to disk with
 * ubik_Write, and ubik_AbortTrans will not rollback anything.
 *
 * @param[in] path  Path to the database .DB0 file/dir.
 * @param[in] ropts Optional; specifies various options. See ubik_rawinit_opts
 *                  for details.
 * @param[out] dbase    The raw database handle.
 *
 * @return ubik/errno error codes
 */
int
ubik_RawInit(char *path, struct ubik_rawinit_opts *ropts,
	     struct ubik_dbase **dbase)
{
    static struct ubik_rawinit_opts ropt_defaults;

    struct ubik_dbase *tdb;
    int code;

    if (ropts == NULL) {
	ropts = &ropt_defaults;
    }

    *dbase = NULL;

    tdb = calloc(1, sizeof(*tdb));
    tdb->is_raw = 1;
    tdb->raw_rw = ropts->r_rw;

    init_locks(tdb);

    if (ropts->r_create_kv || ropts->r_create_flat) {
	if (!tdb->raw_rw) {
	    code = UBADTYPE;
	    goto done;
	}
    }

    if (ropts->r_create_kv) {
	code = ukv_create(path, NULL, &tdb->kv_dbh);
	if (code != 0) {
	    goto done;
	}

    } else if (ropts->r_create_flat) {
	tdb->raw_fh = fopen(path, "w+bx");
	if (tdb->raw_fh == NULL) {
	    code = errno;
	    goto done;
	}

    } else {
	int isdir = 0;

	code = udb_dbinfo(path, NULL, &isdir, NULL);
	if (code != 0) {
	    goto done;
	}

	if (isdir) {
	    code = ukv_open(path, &tdb->kv_dbh, NULL);
	    if (code != 0) {
		goto done;
	    }

	} else {
	    if (tdb->raw_rw) {
		tdb->raw_fh = fopen(path, "r+b");
	    } else {
		tdb->raw_fh = fopen(path, "rb");
	    }
	    if (tdb->raw_fh == NULL) {
		code = errno;
		goto done;
	    }
	}
    }

    *dbase = tdb;
    tdb = NULL;
    code = 0;

 done:
    ubik_RawClose(&tdb);
    return code;
}

/**
 * Close a "raw" ubik dbase handle.
 *
 * @param[inout] a_dbase    Dbase handle to close (or NULL to do nothing). Set
 *			    to NULL on return.
 *
 * @pre *a_dbase (if given) is a raw handle, opened with ubik_RawInit
 * @pre All transactions for *a_dbase (if given) have ended
 */
void
ubik_RawClose(struct ubik_dbase **a_dbase)
{
    struct ubik_dbase *dbase = *a_dbase;
    if (dbase == NULL) {
	return;
    }
    *a_dbase = NULL;

    opr_Assert(ubik_RawDbase(dbase));
    if (dbase->raw_fh != NULL) {
	fclose(dbase->raw_fh);
	dbase->raw_fh = NULL;
    }
    okv_close(&dbase->kv_dbh);

#ifdef AFS_PTHREAD_ENV
    opr_mutex_destroy(&dbase->versionLock);
    opr_cv_destroy(&dbase->flags_cond);
#else
    Lock_Destroy(&dbase->versionLock);
#endif

    free(dbase);
}

int
ubik_RawDbase(struct ubik_dbase *dbase)
{
    if (dbase->is_raw) {
	return 1;
    }
    return 0;
}

int
ubik_RawTrans(struct ubik_trans *transPtr)
{
    if ((transPtr->flags & TRRAW) != 0) {
	return 1;
    }
    return 0;
}

int
ubik_RawHandle(struct ubik_trans *trans, FILE **a_fh,
	       struct okv_trans **a_kvtx)
{
    if (!ubik_RawTrans(trans)) {
	return UBADTYPE;
    }
    if (a_fh != NULL) {
	*a_fh = NULL;
    }
    if (a_kvtx != NULL) {
	*a_kvtx = NULL;
    }

    if (trans->dbase->raw_fh != NULL) {
	if (a_fh == NULL) {
	    return UBADTYPE;
	}
	*a_fh = trans->dbase->raw_fh;
	return 0;
    }

    if (trans->kv_tx != NULL) {
	if (a_kvtx == NULL) {
	    return UBADTYPE;
	}
	*a_kvtx = trans->kv_tx;
	return 0;
    }

    return 0;
}

int
ubik_RawGetHeader(struct ubik_trans *trans, struct ubik_hdr *a_hdr)
{
    struct ubik_hdr hdr;
    int code;

    memset(&hdr, 0, sizeof(hdr));

    if (!ubik_RawTrans(trans) || ubik_KVTrans(trans)) {
	return UBADTYPE;
    }

    code = seek_fread(&hdr, sizeof(hdr), trans->dbase->raw_fh, 0);
    if (code != 0) {
	return code;
    }

    a_hdr->magic = ntohl(hdr.magic);
    a_hdr->size = ntohs(hdr.size);
    a_hdr->version.epoch = ntohl(hdr.version.epoch);
    a_hdr->version.counter = ntohl(hdr.version.counter);

    return 0;
}

int
ubik_RawGetVersion(struct ubik_trans *trans, struct ubik_version *version)
{
    int code;
    struct ubik_hdr hdr;

    memset(&hdr, 0, sizeof(hdr));

    if (!ubik_RawTrans(trans)) {
	return UBADTYPE;
    }

    if (ubik_KVTrans(trans)) {
	return ukv_getlabel(trans->kv_tx, version);
    }

    code = ubik_RawGetHeader(trans, &hdr);
    if (code != 0) {
	return code;
    }

    *version = hdr.version;

    return 0;
}

int
ubik_RawSetVersion(struct ubik_trans *trans, struct ubik_version *version)
{
    int code;
    struct ubik_hdr hdr;

    memset(&hdr, 0, sizeof(hdr));

    if (!ubik_RawTrans(trans) || trans->type != UBIK_WRITETRANS) {
	return UBADTYPE;
    }

    if (ubik_KVTrans(trans)) {
	return ukv_setlabel(trans->kv_tx, version);
    }

    hdr.version.epoch = htonl(version->epoch);
    hdr.version.counter = htonl(version->counter);
    hdr.magic = htonl(UBIK_MAGIC);
    hdr.size = htons(HDRSIZE);

    code = seek_fwrite(&hdr, sizeof(hdr), trans->dbase->raw_fh, 0);
    if (code != 0) {
	return code;
    }

    return 0;
}

/**
 * Install a 'write hook'.
 *
 * If a write hook is installed, then it is called any time ubik writes to the
 * active database on disk (via local or remote transactions, but notably not
 * during recovery operations). The hook is given the buffer that will be
 * written, along with the offset it will be written to, and the ubik dbase and
 * file number. The hook is called right before the data is actually written to
 * disk.
 *
 * Currently only one write hook can be installed, and it must be installed
 * during server startup.
 *
 * @param[in] func  Write hook to install. If NULL, uninstall any installed
 *		    hook.
 * @return ubik error codes
 */
int
ubik_InstallWriteHook(ubik_writehook_func func)
{
    struct ubik_dbase *dbase = ubik_dbase;
    if (dbase->write_hook != NULL && func != NULL) {
	return UINTERNAL;
    }
    dbase->write_hook = func;
    return 0;
}

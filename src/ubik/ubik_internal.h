/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#ifndef OPENAFS_UBIK_INTERNAL_H
#define OPENAFS_UBIK_INTERNAL_H

#include <afs/opr.h>
#ifdef AFS_PTHREAD_ENV
# include <opr/lock.h>
#else
# include <opr/lockstub.h>
#endif
#include <rx/rx_bulk.h>

#include "ubik_np.h"
#include "ubik_int.h"

/*! Sanity check: This macro represents an arbitrary date in the past
 * (Tue Jun 20 15:36:43 2017). The database epoch must be greater or
 * equal to this value. */
#define	UBIK_MILESTONE	    1497987403

struct ubik_stat {
    int kv;
    afs_int32 size;
    afs_uint64 n_items;
};

/*!
 * \brief representation of a ubik database.
 *
 * Contains info on low-level disk access routines
 * for use by disk transaction module.
 */
struct ubik_dbase {
    char *pathName;		/*!< root name for dbase */
    char *pathBase;             /*!< basename of 'pathName' */
    struct ubik_trans *activeTrans;	/*!< active transaction list */
    struct ubik_version version;	/*!< version number. protected by
					 *   DBHOLD and UBIK_VERSION_LOCK */
    struct okv_dbhandle *kv_dbh;	/*!< KV database (if db is KV) */
#ifdef AFS_PTHREAD_ENV
    pthread_mutex_t versionLock;	/*!< lock on version number */
#else
    struct Lock versionLock;	/*!< lock on version number */
#endif
    afs_int32 dbFlags;		/*!< flags */
    /* physio procedures */
    ubik_writehook_func write_hook; /*!< app hook for writing data */
    short readers;		/*!< number of current read transactions */
    struct ubik_version cachedVersion;	/*!< version of caller's cached data */
    struct Lock cache_lock; /*!< protects cached application data */
#ifdef AFS_PTHREAD_ENV
    pthread_cond_t flags_cond;      /*!< condition variable to manage changes to flags */
#endif
    ubik_dbcheck_func dbcheck_func; /*!< app hook to check if a new dbase is sane */

    int is_raw;	    /*!< is this a raw db handle? */
    int raw_rw;	    /*!< for raw dbs, can we write to the db? */
    FILE *raw_fh;   /*!< for raw dbs, the file handle to the db file */
};

/*!
 * \brief representation of a ubik transaction
 */
struct ubik_trans {
    struct ubik_dbase *dbase;	/*!< corresponding database */
    struct ubik_trans *next;	/*!< in the list */
    afs_int32 locktype;		/*!< transaction lock */
    struct ubik_tid tid;	/*!< transaction id of this trans (if write trans.) */
    struct okv_dbhandle *kv_dbh; /*!< KV database (if any) */
    struct okv_trans *kv_tx;	/*!< KV transaction (if any) */
    afs_int32 seekFile;		/*!< seek ptr: file number */
    afs_int32 seekPos;		/*!< seek ptr: offset therein */
    short flags;		/*!< trans flag bits */
    char type;			/*!< type of trans */
    iovec_wrt iovec_info;
    iovec_buf iovec_data;
    struct rx_bulk *bulk_call;	/*!< handle for sending multiple DISK_*
				 *   calls to other servers */
};

/*! \name some ubik parameters */
#define	UBIK_PAGESIZE	    1024	/*!< fits in current r packet */
#define	UBIK_LOGPAGESIZE    10	/*!< base 2 log thereof */
#define	NBUFFERS	    20	/*!< number of 1K buffers */
#define	HDRSIZE		    64	/*!< bytes of header per dbfile */
/*\}*/

/*! \name ubik_dbase flags */
#define	DBWRITING	    1	/*!< are any write trans. in progress */
#define DBSENDING           2   /*!< sending db to someone */
#define DBRECEIVING         4   /*!< receiving db from someone */
/*\}*/

/*!\name ubik trans flags */
#define TRDONE		0x01    /*!< commit or abort done */
#define TRABORT		0x02    /*!< if #TRDONE, tells if aborted */
#define TRREADANY	0x04    /*!< read any data available in trans */
#define TRCACHELOCKED	0x20    /*!< this trans has locked dbase->cache_lock
				 *   (meaning, this trans has called
				 *   ubik_CheckCache at some point */
#define TRREADWRITE	0x40    /*!< read even if there's a conflicting
				 *   ubik- level write lock */
#define TRRAW		0x80    /*!< is this a tx for a raw db? */
#define	TRKEYVAL       0x100	/*!< this trans uses the KV store instead of
				 *   flat-file access */
#define TRREMOTE       0x200	/*!< tx is being accessed by remote DISK_*
				 *   calls (and so, may be accessed across
				 *   threads) */
/*\}*/

/*! \name ubik system database numbers */
#define	LOGFILE		    (-1)
/*\}*/

/*! \name define log opcodes */
#define	LOGNEW		    100	/*!< start transaction */
#define	LOGEND		    101	/*!< commit (good) end transaction */
#define	LOGABORT	    102	/*!< abort (fail) transaction */
#define	LOGDATA		    103	/*!< data */
#define	LOGTRUNCATE	    104	/*!< truncate operation (no longer used) */
/*\}*/

/*!
 * \name timer constants
 * time constant for replication algorithms: the R time period is 20 seconds.  Both
 * #SMALLTIME and #BIGTIME must be larger than #RPCTIMEOUT+max(#RPCTIMEOUT, #POLLTIME),
 * so that timeouts do not prevent us from getting through to our servers in time.
 *
 * We use multi-R to time out multiple down hosts concurrently.
 * The only other restrictions:  #BIGTIME > #SMALLTIME and
 * #BIGTIME-#SMALLTIME > #MAXSKEW (the clock skew).
 */
#define MAXSKEW	10
#define POLLTIME 15
#define RPCTIMEOUT 20
#define BIGTIME 75
#define SMALLTIME 60
/*\}*/

/*!
 * \brief the per-server state, used by the sync site to keep track of its charges
 */
struct ubik_server {
    struct ubik_server *next;	/*!< next ptr */
    afs_uint32 addr[UBIK_MAX_INTERFACE_ADDR];	/*!< network order, addr[0] is primary */
    afs_int32 lastVoteTime;	/*!< last time yes vote received */
    afs_int32 lastBeaconSent;	/*!< last time beacon attempted */
    struct ubik_version version;	/*!< version, only used during recovery */
    struct rx_connection *vote_rxcid;	/*!< cid to use to contact dude for votes */
    struct rx_connection *disk_rxcid;	/*!< cid to use to contact dude for disk reqs */
    char lastVote;		/*!< true if last vote was yes */
    char up;			/*!< is it up? */
    char beaconSinceDown;	/*!< did beacon get through since last crash? */
    char currentDB;		/*!< is dbase up-to-date */
    char magic;			/*!< the one whose vote counts twice */
    char isClone;		/*!< is only a clone, doesn't vote */
};

/*! \name hold and release functions on a database */
#ifdef AFS_PTHREAD_ENV
# define	DBHOLD(a)	opr_mutex_enter(&((a)->versionLock))
# define	DBRELE(a)	opr_mutex_exit(&((a)->versionLock))
#else /* !AFS_PTHREAD_ENV */
# define	DBHOLD(a)	ObtainWriteLock(&((a)->versionLock))
# define	DBRELE(a)	ReleaseWriteLock(&((a)->versionLock))
#endif /* !AFS_PTHREAD_ENV */
/*\}*/

/* globals */

/*!name list of all servers in the system */
extern struct ubik_server *ubik_servers;
extern char amIClone;
/*\}*/

/*! \name network port info */
extern short ubik_callPortal;
/*\}*/

extern afs_int32 ubik_quorum;	/* min hosts in quorum */
extern struct ubik_dbase *ubik_dbase;	/* the database handled by this server */
extern afs_uint32 ubik_host[UBIK_MAX_INTERFACE_ADDR];	/* this host addr, in net order */
extern int ubik_amSyncSite;	/* sleep on this waiting to be sync site */
extern struct ubik_stats {	/* random stats */
    afs_int32 escapes;
} ubik_stats;
extern afs_int32 urecovery_state;	/* sync site recovery process state */
extern struct ubik_trans *ubik_currentTrans;	/* current trans */
extern afs_int32 ubik_debugFlag;	/* ubik debug flag */
extern int ubikPrimaryAddrOnly;	/* use only primary address */

/*
 * Lock ordering
 *
 * Any of the locks may be acquired singly; when acquiring multiple locks, they
 * should be acquired in the listed order:
 * 	application cache lock	(dbase->cache_lock)
 * 	database lock		DBHOLD/DBRELE
 * 	beacon lock		UBIK_BEACON_LOCK/UNLOCK
 * 	vote lock		UBIK_VOTE_LOCK/UNLOCK
 * 	version lock		UBIK_VERSION_LOCK/UNLOCK
 * 	server address lock	UBIK_ADDR_LOCK/UNLOCK
 */

/*!
 * \brief Global beacon data.  All values are protected by beacon_lock
 * This lock also protects some values in the ubik_server structures:
 * 	lastVoteTime
 * 	lastBeaconSent
 * 	lastVote
 * 	up
 * 	beaconSinceDown
 */
struct beacon_data {
#ifdef AFS_PTHREAD_ENV
    pthread_mutex_t beacon_lock;
#endif
    int ubik_amSyncSite;		/*!< flag telling if I'm sync site */
    afs_int32 syncSiteUntil;		/*!< valid only if amSyncSite */
    int ubik_syncSiteAdvertised;	/*!< flag telling if remotes are aware we have quorum */
};

#define UBIK_BEACON_LOCK opr_mutex_enter(&beacon_globals.beacon_lock)
#define UBIK_BEACON_UNLOCK opr_mutex_exit(&beacon_globals.beacon_lock)

/*!
 * \brief Global vote data.  All values are protected by vote_lock
 */
struct vote_data {
#ifdef AFS_PTHREAD_ENV
    pthread_mutex_t vote_lock;
#endif
    struct ubik_version ubik_dbVersion;	/* sync site's dbase version */
    struct ubik_tid ubik_dbTid;		/* sync site's tid, or 0 if none */
    /* Used by all sites in nominating new sync sites */
    afs_int32 ubik_lastYesTime;		/* time we sent the last yes vote */
    afs_uint32 lastYesHost;		/* host to which we sent yes vote */
    /* Next is time sync site began this vote: guarantees sync site until this + SMALLTIME */
    afs_int32 lastYesClaim;
    int lastYesState;			/* did last site we voted for claim to be sync site? */
    /* Used to guarantee that nomination process doesn't loop */
    afs_int32 lowestTime;
    afs_uint32 lowestHost;
    afs_int32 syncTime;
    afs_int32 syncHost;
};

#define UBIK_VOTE_LOCK opr_mutex_enter(&vote_globals.vote_lock)
#define UBIK_VOTE_UNLOCK opr_mutex_exit(&vote_globals.vote_lock)

/*!
 * \brief Server address data.  All values are protected by addr_lock
 *
 * This lock also protects:
 *     ubik_server: addr[], vote_rxcid, disk_rxcid
 *
 */
struct addr_data {
#ifdef AFS_PTHREAD_ENV
    pthread_mutex_t addr_lock;
#endif
    afs_int32 ubikSecIndex;
    struct rx_securityClass *ubikSecClass;
};

#define UBIK_ADDR_LOCK opr_mutex_enter(&addr_globals.addr_lock)
#define UBIK_ADDR_UNLOCK opr_mutex_exit(&addr_globals.addr_lock)

/*!
 * \brief The version lock protects the structure members, as well as
 * partially protecting ubik_dbase->version. Reading ubik_dbase->version can be
 * done while holding either UBIK_VERSION_LOCK or DBHOLD. Writing
 * ubik_dbase->version requires holding both locks.
 */
struct version_data {
#ifdef AFS_PTHREAD_ENV
    pthread_mutex_t version_lock;
#endif
    afs_int32 ubik_epochTime;	/* time when this site started */
    afs_int32 tidCounter;	/* last RW or RO trans tid counter */
    afs_int32 writeTidCounter;	/* last write trans tid counter */
    int db_writing;		/* Is there a write tx running?
				 * This is the same as
				 * (ubik_dbase->dbFlags & DBWRITING)
				 * but is protected by UBIK_VERSION_LOCK
				 * instead of by DBHOLD. */
};
extern struct version_data version_globals;

#define UBIK_VERSION_LOCK opr_mutex_enter(&version_globals.version_lock)
#define UBIK_VERSION_UNLOCK opr_mutex_exit(&version_globals.version_lock)

/* phys.c */
extern int uphys_read(struct ubik_dbase *adbase, afs_int32 afile,
		      void *abuffer, afs_int32 apos,
		      afs_int32 alength);
extern int uphys_write(struct ubik_dbase *adbase, afs_int32 afile,
		       void *abuffer, afs_int32 apos,
		       afs_int32 alength);
extern int uphys_truncate(struct ubik_dbase *adbase, afs_int32 afile,
			  afs_int32 asize);
extern int uphys_getnfiles(struct ubik_dbase *adbase);
extern int uphys_getlabel(struct ubik_dbase *adbase, afs_int32 afile,
			  struct ubik_version *aversion);
extern int uphys_setlabel(struct ubik_dbase *adbase, afs_int32 afile,
			  struct ubik_version *aversion);
extern int uphys_sync(struct ubik_dbase *adbase, afs_int32 afile);
extern void uphys_invalidate(struct ubik_dbase *adbase,
			     afs_int32 afid);
extern int uphys_buf_append(struct ubik_dbase *adbase, afs_int32 afid,
			    void *buf, afs_int32 alength);

int uphys_stat_path(char *path, struct ubik_stat *astat);
int uphys_getlabel_path(char *path, struct ubik_version *aversion);
int uphys_setlabel_path(char *path, struct ubik_version *aversion);
int uphys_copydb(char *src_path, char *dest_path);
int uphys_recvdb(struct rx_call *rxcall, char *path,
		 struct ubik_version *version, afs_int64 length);
int uphys_senddb(char *path, struct rx_call *rxcall,
		 struct ubik_version *version, afs_int64 length);

/*! \name recovery.c */
extern int urecovery_ResetState(void);
extern int urecovery_LostServer(struct ubik_server *server);
extern int urecovery_AllBetter(struct ubik_dbase *adbase,
			       int areadAny);
extern int urecovery_AbortAll(struct ubik_dbase *adbase);
extern int urecovery_CheckTid(struct ubik_tid *atid, int abortalways);
extern int urecovery_Initialize(struct ubik_dbase *adbase);
extern void *urecovery_Interact(void *);
extern int DoProbe(struct ubik_server *server);

extern void ubik_set_db_flags(struct ubik_dbase *dbase, int flags);
extern void ubik_clear_db_flags(struct ubik_dbase *dbase, int flags);
extern int ubik_wait_db_flags(struct ubik_dbase *dbase, int flags);

struct urecovery_recvdb_type {
    char *descr;
    int client;
    int old_rpc;
};
extern struct urecovery_recvdb_type urecovery_recvdb_getfile_old;
extern struct urecovery_recvdb_type urecovery_recvdb_ssendfile_old;
extern struct urecovery_recvdb_type urecovery_recvdb_getfile2;
extern struct urecovery_recvdb_type urecovery_recvdb_ssendfile2;

struct urecovery_recvdb_info {
    /* remote server IP */
    afs_uint32 otherHost;

    /* For client-side receives only, the rxconn to issue the RPC on. */
    struct rx_connection *rxconn;

    /* For server-side receives only, the rxcall for the RPC. */
    struct rx_call *rxcall;

    /* For SDISK_SendFile only, the length and version arguments from the
     * SDISK_SendFile RPC must be supplied here. */
    afs_int64 flat_length;
    struct ubik_version *flat_version;
};

int urecovery_receive_db(struct ubik_dbase *dbase,
			 struct urecovery_recvdb_type *rtype,
			 struct urecovery_recvdb_info *rinfo,
			 struct ubik_version *version)
			 AFS_NONNULL((1,2,3));

struct urecovery_senddb_type {
    char *descr;
    int client;
    int old_rpc;
};
extern struct urecovery_senddb_type urecovery_senddb_sendfile_old;
extern struct urecovery_senddb_type urecovery_senddb_sgetfile_old;
extern struct urecovery_senddb_type urecovery_senddb_sendfile2;
extern struct urecovery_senddb_type urecovery_senddb_sgetfile2;

struct urecovery_senddb_info {
    /* remote server IP */
    afs_uint32 otherHost;

    /* For client-side sends only, the rxconn to issue the RPC on. */
    struct rx_connection *rxconn;

    /* For server-side sends only, the rxcall for the RPC. */
    struct rx_call *rxcall;

    /*
     * If this is set, the caller must have already set the DBSENDING flag on
     * the database. Otherwise, urecovery_send_db sets DBSENDING on entry, and
     * clears it before returning.
     */
    int nosetflags;
};

int urecovery_send_db(struct ubik_dbase *dbase,
		      struct urecovery_senddb_type *stype,
		      struct urecovery_senddb_info *sinfo,
		      struct ubik_version *version)
		      AFS_NONNULL((1,2,3));

int urecovery_distribute_db(struct ubik_dbase *dbase, int *a_nsent);
/*\}*/

/*! \name ubik.c */
extern int ubik_cq_disk_setversion(struct ubik_trans *atrans, int flags,
				   struct ubik_version *oldversion,
				   struct ubik_version *newversion);

extern void panic(char *format, ...)
    AFS_ATTRIBUTE_FORMAT(__printf__, 1, 2);

extern afs_uint32 ubikGetPrimaryInterfaceAddr(afs_uint32 addr);

extern int ubik_CheckAuth(struct rx_call *);

/*\}*/

/*! \name beacon.c */
struct afsconf_cell;
extern void ubeacon_InitSecurityClass(void);
extern void ubeacon_ReinitServer(struct ubik_server *ts);
extern void ubeacon_Debug(struct ubik_debug *aparm);
extern int ubeacon_AmSyncSite(void);
extern int ubeacon_SyncSiteAdvertised(void);
extern int ubeacon_InitServerListByInfo(afs_uint32 ame,
					struct afsconf_cell *info,
					char clones[], char *configDir);
extern int ubeacon_InitServerList(afs_uint32 ame, afs_uint32 aservers[],
				  char *configDir);
extern void *ubeacon_Interact(void *);
extern int ubeacon_updateUbikNetworkAddress(afs_uint32 ubik_host[UBIK_MAX_INTERFACE_ADDR]);
extern struct beacon_data beacon_globals;
extern struct addr_data addr_globals;

/*\}*/

/*! \name disk.c */
extern int udisk_Init(int nBUffers);
extern void udisk_Debug(struct ubik_debug *aparm);
extern int udisk_Invalidate(struct ubik_dbase *adbase, afs_int32 afid);
extern int udisk_read(struct ubik_trans *atrans, afs_int32 afile,
		      void *abuffer, afs_int32 apos, afs_int32 alen);
extern int udisk_write(struct ubik_trans *atrans, afs_int32 afile,
		       void *abuffer, afs_int32 apos, afs_int32 alen);
extern int udisk_begin(struct ubik_dbase *adbase, int atype, int flags,
		       struct ubik_trans **atrans);
extern int udisk_commit(struct ubik_trans *atrans);
extern int udisk_abort(struct ubik_trans *atrans);
extern void udisk_end(struct ubik_trans *atrans);
/*\}*/

/*! \name lock.c */
extern void ulock_Init(void);
extern int  ulock_getLock(struct ubik_trans *atrans, int atype, int await);
extern void ulock_relLock(struct ubik_trans *atrans);
extern void ulock_Debug(struct ubik_debug *aparm);
/*\}*/

/*! \name vote.c */
extern int uvote_ShouldIRun(void);
extern afs_int32 uvote_GetSyncSite(void);
extern int uvote_Init(void);
extern struct vote_data vote_globals;
extern void uvote_set_dbVersion(struct ubik_version);
extern int uvote_eq_dbVersion(struct ubik_version);
extern int uvote_HaveSyncAndVersion(struct ubik_version);
/*\}*/

/* udb.c */

void udb_v32to64(struct ubik_version *from, struct ubik_version64 *to);
void udb_tid32to64(struct ubik_tid *from, struct ubik_tid64 *to);
int udb_v64to32(char *descr, struct ubik_version64 *from,
		struct ubik_version *to);
int udb_tid64to32(char *descr, struct ubik_tid64 *from,
		  struct ubik_tid *to);
int udb_vcmp64(struct ubik_version64 *vers_a,
	       struct ubik_version64 *vers_b);

int udb_path(struct ubik_dbase *dbase, char *suffix, char **apath);
int udb_dbinfo(char *path, int *a_exists, int *a_iskv, int *a_islink);
int udb_stat(char *path, struct ubik_stat *astat);
int udb_delpath(char *path);
int udb_del_suffixes(struct ubik_dbase *dbase, char *suffix_new,
		     char *suffix_spare);
int udb_check_contents(struct ubik_dbase *dbase, char *path);
int udb_install_simple(struct ubik_dbase *dbase, char *suffix_new,
		       struct ubik_version *vers_new);
int udb_install(struct ubik_dbase *dbase, char *suffix_new,
		char *suffix_old, struct ubik_version *new_vers);
int udb_getlabel_path(char *path, struct ubik_version *version);
int udb_getlabel_db(struct ubik_dbase *dbase, struct ubik_version *version);
int udb_setlabel_path(char *path, struct ubik_version *version);
int udb_setlabel_trans(struct ubik_trans *trans, struct ubik_version *version);
int udb_setlabel_db(struct ubik_dbase *dbase, struct ubik_version *version);
int udb_recvdb_stream(struct rx_call *rxcall, char *path,
		      struct ubik_version *version);
int udb_senddb_stream(char *path, struct rx_call *rxcall,
		      struct ubik_version *version);

/* freeze_server.c */

void ufreeze_Init(struct ubik_serverinit_opts *opts);

/* ukv.c */

int ukv_next(struct okv_trans *tx, struct rx_opaque *key,
	     struct rx_opaque *value, int *a_eof);
int ukv_getlabel(struct okv_trans *tx, struct ubik_version *version);
int ukv_getlabel_db(struct ubik_dbase *dbase, struct ubik_version *version);
int ukv_setlabel(struct okv_trans *tx, struct ubik_version *version);
int ukv_setlabel_db(struct ubik_dbase *dbase, struct ubik_version *version);
int ukv_setlabel_path(char *path, struct ubik_version *version);
int ukv_begin(struct ubik_trans *atrans, struct okv_trans **a_tx);
int ukv_put(struct ubik_trans *atrans, struct rx_opaque *key,
	    struct rx_opaque *value, int replace);
int ukv_delete(struct ubik_trans *atrans, struct rx_opaque *key, int *a_noent);
int ukv_commit(struct okv_trans **a_tx, struct ubik_version *version);
int ukv_stat(char *path, struct ubik_stat *astat);
int ukv_create(char *kvdir, char *okv_engine, struct okv_dbhandle **a_dbh);
int ukv_open(char *kvdir, struct okv_dbhandle **a_dbh,
	     struct ubik_version *version);
int ukv_init(struct ubik_dbase *dbase, int create_db);
int ukv_copydb(char *src_path, char *dest_path);
int ukv_db_readlink(struct ubik_dbase *dbase, char *path_db, char **a_path);
int ukv_db_prepinstall(struct ubik_dbase *dbase, char *path_orig);
void ukv_cleanup_unused(struct ubik_dbase *dbase);
int ukv_recvdb(struct rx_call *rxcall, char *path,
	       struct ubik_version *version);
int ukv_senddb(char *path, struct rx_call *rxcall,
	       struct ubik_version *version);

#endif /* OPENAFS_UBIK_INTERNAL_H */

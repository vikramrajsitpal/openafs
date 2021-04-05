/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#ifndef UBIK_H
#define UBIK_H

#include <stdarg.h>

#include <ubik_int.h>

/*! \name ubik_trans types */
#define	UBIK_READTRANS	    0
#define	UBIK_WRITETRANS	    1
/*\}*/

/*! \name ubik_lock types */
#define	LOCKREAD	    1
#define	LOCKWRITE	    2
#define	LOCKWAIT	    3
/*\}*/

/*! \name ubik client flags */
#define UPUBIKONLY 	    1	/*!< only check servers presumed functional */
#define UBIK_CALL_NEW 	    2	/*!< use the semantics of ubik_Call_New */
/*\}*/

/*! \name RX services types */
#define	VOTE_SERVICE_ID	    50
#define	DISK_SERVICE_ID	    51
#define	USER_SERVICE_ID	    52	/*!< Since most applications use same port! */
/*\}*/

#define	UBIK_MAGIC	0x354545

/*! \name global ubik parameters */
#define	MAXSERVERS	    20	/*!< max number of servers */
/*\}*/

/*! version comparison macro */
#define vcmp(a,b) ((a).epoch == (b).epoch? ((a).counter - (b).counter) : ((a).epoch - (b).epoch))

#ifdef AFS_PTHREAD_ENV
#include <pthread.h>
#else
#include <lwp.h>
#endif
#include <lock.h>		/* just to make sure we've got this */

struct ubik_client;
struct ubik_dbase;
struct ubik_trans;

/*! \name ubik_client state bits */
#define	CFLastFailed	    1	/*!< last call failed to this guy (to detect down hosts) */
/*\}*/

/*!
 * \brief per-client structure for ubik
 */
struct ubik_client {
    short initializationState;	/*!< ubik client init state */
    short states[MAXSERVERS];	/*!< state bits */
    struct rx_connection *conns[MAXSERVERS];
    afs_int32 syncSite;
#ifdef AFS_PTHREAD_ENV
    pthread_mutex_t cm;
#endif
};

#ifdef AFS_PTHREAD_ENV
#define LOCK_UBIK_CLIENT(client) opr_mutex_enter(&client->cm)
#define UNLOCK_UBIK_CLIENT(client) opr_mutex_exit(&client->cm)
#else
#define LOCK_UBIK_CLIENT(client)
#define UNLOCK_UBIK_CLIENT(client)
#endif

#define	ubik_GetRPCConn(astr,aindex)	((aindex) >= MAXSERVERS? 0 : (astr)->conns[aindex])
#define	ubik_GetRPCHost(astr,aindex)	((aindex) >= MAXSERVERS? 0 : (astr)->hosts[aindex])

/*!
 * \brief ubik header file structure
 */
struct ubik_hdr {
    afs_int32 magic;		/*!< magic number */
    short pad1;			/*!< some 0-initd padding */
    short size;			/*!< header allocation size */
    struct ubik_version version;	/*!< the version for this file */
};

/**
 * ubik_CheckCache callback function.
 *
 * @param[in] atrans  ubik transaction
 * @param[in] rock    rock passed to ubik_CheckCache
 *
 * @return operation status
 *   @retval 0        cache was read properly
 */
typedef int (*ubik_updatecache_func) (struct ubik_trans *atrans, void *rock);

/*! \name procedures for automatically authenticating ubik connections */
extern int (*ubik_CRXSecurityProc) (void *, struct rx_securityClass **,
				    afs_int32 *);
extern void *ubik_CRXSecurityRock;
extern int (*ubik_SRXSecurityProc) (void *, struct rx_securityClass **,
				    afs_int32 *);
extern void *ubik_SRXSecurityRock;
extern int (*ubik_CheckRXSecurityProc) (void *, struct rx_call *);
extern void *ubik_CheckRXSecurityRock;

extern void ubik_SetClientSecurityProcs(int (*scproc)(void *,
						      struct rx_securityClass **,
						      afs_int32 *),
					int (*checkproc) (void *),
					void *rock);
extern void ubik_SetServerSecurityProcs
		(void (*buildproc) (void *,
                                    struct rx_securityClass ***,
                                    afs_int32 *),
                 int (*checkproc) (void *, struct rx_call *),
                 void *rock);

/*\}*/

/*
 * For applications that make use of ubik_BeginTransReadAnyWrite, writing
 * processes must not update the application-level cache as they write,
 * or else readers can read the new cache before the data is committed to
 * the db. So, when a commit occurs, the cache must be updated right then.
 * If set, this function will be called during commits of write transactions,
 * to update the application-level cache after a write. This will be called
 * immediately after the local disk commit succeeds, and it will be called
 * with a lock held that prevents other threads from reading from the cache
 * or the db in general.
 *
 * Note that this function MUST be set in order to make use of
 * ubik_BeginTransReadAnyWrite.
 */
extern int (*ubik_SyncWriterCacheProc) (void);

/*! \name urecovery state bits for sync site */
#define	UBIK_RECSYNCSITE	1	/* am sync site */
#define	UBIK_RECFOUNDDB		2	/* found acceptable dbase from quorum */
#define	UBIK_RECHAVEDB		4	/* fetched best dbase */
#define	UBIK_RECLABELDB		8	/* relabelled dbase */
#define	UBIK_RECSENTDB		0x10	/* sent best db to *everyone* */
#define	UBIK_RECSBETTER		UBIK_RECLABELDB	/* last state */
/*\}*/

extern afs_int32 ubik_nBuffers;

/*!
 * \name Public function prototypes
 */

/*! \name ubik.c */
struct afsconf_cell;
extern int ubik_ServerInitByInfo(afs_uint32 myHost, short myPort,
				 struct afsconf_cell *info, char clones[],
				 const char *pathName,
				 struct ubik_dbase **dbase);
extern int ubik_ServerInit(afs_uint32 myHost, short myPort,
			   afs_uint32 serverList[],
			   const char *pathName, struct ubik_dbase **dbase);
extern int ubik_BeginTrans(struct ubik_dbase *dbase,
			   afs_int32 transMode, struct ubik_trans **transPtr);
extern int ubik_BeginTransReadAny(struct ubik_dbase *dbase,
				  afs_int32 transMode,
				  struct ubik_trans **transPtr);
extern int ubik_BeginTransReadAnyWrite(struct ubik_dbase *dbase,
                                       afs_int32 transMode,
                                       struct ubik_trans **transPtr);
extern int ubik_AbortTrans(struct ubik_trans *transPtr);

extern int ubik_EndTrans(struct ubik_trans *transPtr);
extern int ubik_Read(struct ubik_trans *transPtr, void *buffer,
		     afs_int32 length);
extern int ubik_Flush(struct ubik_trans *transPtr);
extern int ubik_Write(struct ubik_trans *transPtr, void *buffer,
		      afs_int32 length);
extern int ubik_Seek(struct ubik_trans *transPtr, afs_int32 fileid,
		     afs_int32 position);
extern int ubik_Tell(struct ubik_trans *transPtr, afs_int32 * fileid,
		     afs_int32 * position);
extern int ubik_SetLock(struct ubik_trans *atrans, afs_int32 apos,
			afs_int32 alen, int atype);
extern int ubik_CheckCache(struct ubik_trans *atrans,
                           ubik_updatecache_func check,
                           void *rock);
/*\}*/

/*! \name ubikclient.c */

extern int ubik_ParseClientList(int argc, char **argv, afs_uint32 * aothers);
extern unsigned int afs_random(void);
extern int ubik_ClientInit(struct rx_connection **serverconns,
			   struct ubik_client **aclient);
extern afs_int32 ubik_ClientDestroy(struct ubik_client *aclient);
extern struct rx_connection *ubik_RefreshConn(struct rx_connection *tc);

struct ubik_callrock_info {
    struct rx_connection *conn;
};
typedef afs_int32 (*ubik_callrock_func)(struct ubik_callrock_info *info, void *rock);
extern afs_int32 ubik_CallRock(struct ubik_client *aclient, afs_int32 aflags,
			       ubik_callrock_func proc, void *rock)
			       AFS_NONNULL((3));

#ifdef UBIK_LEGACY_CALLITER
extern afs_int32 ubik_CallIter(int (*aproc) (), struct ubik_client *aclient,
			       afs_int32 aflags, int *apos, long p1, long p2,
			       long p3, long p4, long p5, long p6, long p7,
			       long p8, long p9, long p10, long p11, long p12,
			       long p13, long p14, long p15, long p16);
extern afs_int32 ubik_Call_New(int (*aproc) (), struct ubik_client
			       *aclient, afs_int32 aflags, long p1, long p2,
			       long p3, long p4, long p5, long p6, long p7,
			       long p8, long p9, long p10, long p11, long p12,
			       long p13, long p14, long p15, long p16);
#endif
/*\}*/

/* \name ubikcmd.c */
extern int ubik_ParseServerList(int argc, char **argv, afs_uint32 *ahost,
				afs_uint32 *aothers);
/*\}*/

/* \name uinit.c */

struct rx_securityClass;
struct afsconf_dir;
typedef int (*ugen_secproc_func)(struct rx_securityClass *, afs_int32);
extern int ugen_ClientInitCell(struct afsconf_dir *dir,
			       struct afsconf_cell *info,
			       int secFlags,
			       struct ubik_client **uclientp,
			       int maxservers, const char *serviceid,
			       int deadtime);
extern int ugen_ClientInitServer(const char *confDir, char *cellName,
				 int secFlags, struct ubik_client **uclientp,
				 int maxservers, char *serviceid,
				 int deadtime, afs_uint32 server,
			         afs_uint32 port);
extern int ugen_ClientInitFlags(const char *confDir, char *cellName,
				int secFlags, struct ubik_client **uclientp,
				ugen_secproc_func secproc,
				int maxservers, char *serviceid,
				int deadtime);
extern afs_int32 ugen_ClientInit(int noAuthFlag, const char *confDir,
				 char *cellName, afs_int32 sauth,
				 struct ubik_client **uclientp,
				 ugen_secproc_func secproc,
				 char *funcName,
				 afs_int32 gen_rxkad_level,
				 afs_int32 maxservers, char *serviceid,
				 afs_int32 deadtime, afs_uint32 server,
				 afs_uint32 port, afs_int32 usrvid);

#endif /* UBIK_H */

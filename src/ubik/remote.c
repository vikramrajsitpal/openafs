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

#include <assert.h>

#include <lock.h>
#include <rx/xdr.h>
#include <rx/rx.h>
#include <afs/afsutil.h>

#include "ubik_internal.h"

static void printServerInfo(void);

/*! \file
 * routines for handling requests remotely-submitted by the sync site.  These are
 * only write transactions (we don't propagate read trans), and there is at most one
 * write transaction extant at any one time.
 */

struct ubik_trans *ubik_currentTrans = 0;



/* the rest of these guys handle remote execution of write
 * transactions: this is the code executed on the other servers when a
 * sync site is executing a write transaction.
 */
afs_int32
SDISK_Begin(struct rx_call *rxcall, struct ubik_tid *atid)
{
    afs_int32 code;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }
    DBHOLD(ubik_dbase);
    if (urecovery_AllBetter(ubik_dbase, 0) == 0) {
	code = UNOQUORUM;
	goto out;
    }
    /* wait until we're not sending the database to someone */
    if (ubik_wait_db_flags(ubik_dbase, DBSENDING)) {
	ViceLog(0, ("Ubik: Unexpected database flags in SDISK_BeginFlags (flags: 0x%x)",
		   ubik_dbase->dbFlags));
	code = UNOQUORUM;
	goto out;
    }
    if (urecovery_AllBetter(ubik_dbase, 0) == 0) {
	code = UNOQUORUM;
	goto out;
    }
    urecovery_CheckTid(atid, 1);
    code = udisk_begin(ubik_dbase, UBIK_WRITETRANS, &ubik_currentTrans);
    if (!code && ubik_currentTrans) {
	/* label this trans with the right trans id */
	ubik_currentTrans->tid.epoch = atid->epoch;
	ubik_currentTrans->tid.counter = atid->counter;
    }
  out:
    DBRELE(ubik_dbase);
    return code;
}


afs_int32
SDISK_Commit(struct rx_call *rxcall, struct ubik_tid *atid)
{
    afs_int32 code;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }
    ObtainWriteLock(&ubik_dbase->cache_lock);
    DBHOLD(ubik_dbase);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }
    /*
     * sanity check to make sure only write trans appear here
     */
    if (ubik_currentTrans->type != UBIK_WRITETRANS) {
	code = UBADTYPE;
	goto done;
    }

    urecovery_CheckTid(atid, 0);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }

    code = udisk_commit(ubik_currentTrans);
    if (code == 0) {
	/* sync site should now match */
	uvote_set_dbVersion(ubik_dbase->version);
    }
done:
    DBRELE(ubik_dbase);
    ReleaseWriteLock(&ubik_dbase->cache_lock);
    return code;
}

afs_int32
SDISK_ReleaseLocks(struct rx_call *rxcall, struct ubik_tid *atid)
{
    afs_int32 code;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }

    DBHOLD(ubik_dbase);

    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }
    /* sanity check to make sure only write trans appear here */
    if (ubik_currentTrans->type != UBIK_WRITETRANS) {
	code = UBADTYPE;
	goto done;
    }

    urecovery_CheckTid(atid, 0);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }

    /* If the thread is not waiting for lock - ok to end it */
    if (ubik_currentTrans->locktype != LOCKWAIT) {
	udisk_end(ubik_currentTrans);
    }
    ubik_currentTrans = (struct ubik_trans *)0;
done:
    DBRELE(ubik_dbase);
    return code;
}

afs_int32
SDISK_Abort(struct rx_call *rxcall, struct ubik_tid *atid)
{
    afs_int32 code;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }
    DBHOLD(ubik_dbase);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }
    /* sanity check to make sure only write trans appear here  */
    if (ubik_currentTrans->type != UBIK_WRITETRANS) {
	code = UBADTYPE;
	goto done;
    }

    urecovery_CheckTid(atid, 0);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }

    code = udisk_abort(ubik_currentTrans);
    /* If the thread is not waiting for lock - ok to end it */
    if (ubik_currentTrans->locktype != LOCKWAIT) {
	udisk_end(ubik_currentTrans);
    }
    ubik_currentTrans = (struct ubik_trans *)0;
done:
    DBRELE(ubik_dbase);
    return code;
}

/* apos and alen are not used */
afs_int32
SDISK_Lock(struct rx_call *rxcall, struct ubik_tid *atid,
	   afs_int32 afile, afs_int32 apos, afs_int32 alen, afs_int32 atype)
{
    afs_int32 code;
    struct ubik_trans *ubik_thisTrans;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }
    DBHOLD(ubik_dbase);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }
    /* sanity check to make sure only write trans appear here */
    if (ubik_currentTrans->type != UBIK_WRITETRANS) {
	code = UBADTYPE;
	goto done;
    }
    if (alen != 1) {
	code = UBADLOCK;
	goto done;
    }
    urecovery_CheckTid(atid, 0);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }

    ubik_thisTrans = ubik_currentTrans;
    code = ulock_getLock(ubik_currentTrans, atype, 1);

    /* While waiting, the transaction may have been ended/
     * aborted from under us (urecovery_CheckTid). In that
     * case, end the transaction here.
     */
    if (!code && (ubik_currentTrans != ubik_thisTrans)) {
	udisk_end(ubik_thisTrans);
	code = USYNC;
    }
done:
    DBRELE(ubik_dbase);
    return code;
}

/*!
 * \brief Write a vector of data
 */
afs_int32
SDISK_WriteV(struct rx_call *rxcall, struct ubik_tid *atid,
	     iovec_wrt *io_vector, iovec_buf *io_buffer)
{
    afs_int32 code, i, offset;
    struct ubik_iovec *iovec;
    char *iobuf;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }
    DBHOLD(ubik_dbase);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }
    /* sanity check to make sure only write trans appear here */
    if (ubik_currentTrans->type != UBIK_WRITETRANS) {
	code = UBADTYPE;
	goto done;
    }

    urecovery_CheckTid(atid, 0);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }

    iovec = (struct ubik_iovec *)io_vector->val;
    iobuf = (char *)io_buffer->val;
    for (i = 0, offset = 0; i < io_vector->len; i++) {
	/* Sanity check for going off end of buffer */
	if ((offset + iovec[i].length) > io_buffer->len) {
	    code = UINTERNAL;
	} else {
	    code =
		udisk_write(ubik_currentTrans, iovec[i].file, &iobuf[offset],
			    iovec[i].position, iovec[i].length);
	}
	if (code)
	    break;

	offset += iovec[i].length;
    }
done:
    DBRELE(ubik_dbase);
    return code;
}

afs_int32
SDISK_Write(struct rx_call *rxcall, struct ubik_tid *atid,
	    afs_int32 afile, afs_int32 apos, bulkdata *adata)
{
    afs_int32 code;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }
    DBHOLD(ubik_dbase);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }
    /* sanity check to make sure only write trans appear here */
    if (ubik_currentTrans->type != UBIK_WRITETRANS) {
	code = UBADTYPE;
	goto done;
    }

    urecovery_CheckTid(atid, 0);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }
    code = udisk_write(ubik_currentTrans, afile, adata->val, apos, adata->len);
done:
    DBRELE(ubik_dbase);
    return code;
}

afs_int32
SDISK_GetVersion(struct rx_call *rxcall,
		 struct ubik_version *aversion)
{
    afs_int32 code;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }

    /*
     * If we are the sync site, recovery shouldn't be running on any
     * other site. We shouldn't be getting this RPC as long as we are
     * the sync site.  To prevent any unforseen activity, we should
     * reject this RPC until we have recognized that we are not the
     * sync site anymore, and/or if we have any pending WRITE
     * transactions that have to complete. This way we can be assured
     * that this RPC would not block any pending transactions that
     * should either fail or pass. If we have recognized the fact that
     * we are not the sync site any more, all write transactions would
     * fail with UNOQUORUM anyway.
     */
    DBHOLD(ubik_dbase);
    if (ubeacon_AmSyncSite()) {
	DBRELE(ubik_dbase);
	return UDEADLOCK;
    }

    code = udb_getlabel_db(ubik_dbase, aversion);
    DBRELE(ubik_dbase);
    if (code) {
	/* tell other side there's no dbase */
	aversion->epoch = 0;
	aversion->counter = 0;
    }
    return 0;
}

static int
uremote_sgetfile(struct rx_call *rxcall, struct urecovery_senddb_type *stype,
		 struct ubik_version *version)
{
    struct urecovery_senddb_info sinfo;
    afs_uint32 host;
    int code;

    memset(&sinfo, 0, sizeof(sinfo));
    sinfo.rxcall = rxcall;

    host = rx_HostOf(rx_PeerOf(rx_ConnectionOf(rxcall)));
    sinfo.otherHost = ubikGetPrimaryInterfaceAddr(host);
    if (sinfo.otherHost == 0) {
	sinfo.otherHost = host;
    }

    DBHOLD(ubik_dbase);
    code = urecovery_send_db(ubik_dbase, stype, &sinfo, version);
    DBRELE(ubik_dbase);

    return code;
}

afs_int32
SDISK_GetFile(struct rx_call *rxcall, afs_int32 file,
	      struct ubik_version *version)
{
    afs_int32 code;
    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }

    if (file != 0) {
	return UNOENT;
    }

    return uremote_sgetfile(rxcall, &urecovery_senddb_sgetfile_old, version);
}

static int
uremote_ssendfile(struct rx_call *rxcall, struct urecovery_recvdb_type *rtype,
		  afs_int32 flat_length, struct ubik_version *flat_vers)
{
    int code;
    struct rx_peer *tpeer;
    struct rx_connection *tconn;
    afs_uint32 syncHost = 0;
    afs_uint32 otherHost = 0;
    struct urecovery_recvdb_info rinfo;
    struct ubik_version version;
    char hoststr[16];

    memset(&version, 0, sizeof(version));

    /*
     * first, we do a sanity check to see if the guy sending us the database is
     * the guy we think is the sync site.  It turns out that we might not have
     * decided yet that someone's the sync site, but they could have enough
     * votes from others to be sync site anyway, and could send us the database
     * in advance of getting our votes.  This is fine, what we're really trying
     * to check is that some authenticated bogon isn't sending a random database
     * into another configuration.  This could happen on a bad configuration
     * screwup.  Thus, we only object if we're sure we know who the sync site
     * is, and it ain't the guy talking to us.
     */
    syncHost = uvote_GetSyncSite();
    tconn = rx_ConnectionOf(rxcall);
    tpeer = rx_PeerOf(tconn);
    otherHost = ubikGetPrimaryInterfaceAddr(rx_HostOf(tpeer));
    if (syncHost && syncHost != otherHost) {
	/* we *know* this is the wrong guy */
        char sync_hoststr[16];
	ViceLog(0,
	    ("Ubik: Refusing synchronization with server %s since it is not the sync-site (%s).\n",
	     afs_inet_ntoa_r(otherHost, hoststr),
	     afs_inet_ntoa_r(syncHost, sync_hoststr)));
	return USYNC;
    }

    memset(&rinfo, 0, sizeof(rinfo));
    rinfo.rxcall = rxcall;
    rinfo.otherHost = otherHost;
    rinfo.flat_version = flat_vers;
    rinfo.flat_length = flat_length;

    DBHOLD(ubik_dbase);
    code = urecovery_receive_db(ubik_dbase, rtype, &rinfo, &version);
    if (code == 0) {
	uvote_set_dbVersion(version);
    }
    DBRELE(ubik_dbase);
    return code;
}

afs_int32
SDISK_SendFile(struct rx_call *rxcall, afs_int32 file,
	       afs_int32 length, struct ubik_version *avers)
{
    afs_int32 code;

    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }

    if (file != 0) {
	return UNOENT;
    }

    return uremote_ssendfile(rxcall, &urecovery_recvdb_ssendfile_old, length,
			     avers);
}

afs_int32
SDISK_Probe(struct rx_call *rxcall)
{
    return 0;
}

/*!
 * \brief Update remote machines addresses in my server list
 *
 * Send back my addresses to caller of this RPC
 * \return zero on success, else 1.
 */
afs_int32
SDISK_UpdateInterfaceAddr(struct rx_call *rxcall,
			  UbikInterfaceAddr *inAddr,
			  UbikInterfaceAddr *outAddr)
{
    struct ubik_server *ts, *tmp;
    afs_uint32 remoteAddr;	/* in net byte order */
    int i, j, found = 0, probableMatch = 0;
    char hoststr[16];

    UBIK_ADDR_LOCK;
    /* copy the output parameters */
    for (i = 0; i < UBIK_MAX_INTERFACE_ADDR; i++)
	outAddr->hostAddr[i] = ntohl(ubik_host[i]);

    remoteAddr = htonl(inAddr->hostAddr[0]);
    for (ts = ubik_servers; ts; ts = ts->next)
	if (ts->addr[0] == remoteAddr) {	/* both in net byte order */
	    probableMatch = 1;
	    break;
	}

    if (probableMatch) {
	/* verify that all addresses in the incoming RPC are
	 ** not part of other server entries in my CellServDB
	 */
	for (i = 0; !found && (i < UBIK_MAX_INTERFACE_ADDR)
	     && inAddr->hostAddr[i]; i++) {
	    remoteAddr = htonl(inAddr->hostAddr[i]);
	    for (tmp = ubik_servers; (!found && tmp); tmp = tmp->next) {
		if (ts == tmp)	/* this is my server */
		    continue;
		for (j = 0; (j < UBIK_MAX_INTERFACE_ADDR) && tmp->addr[j];
		     j++)
		    if (remoteAddr == tmp->addr[j]) {
			found = 1;
			break;
		    }
	    }
	}
    }

    /* if (probableMatch) */
    /* inconsistent addresses in CellServDB */
    if (!probableMatch || found) {
	ViceLog(0, ("Inconsistent Cell Info from server:\n"));
	for (i = 0; i < UBIK_MAX_INTERFACE_ADDR && inAddr->hostAddr[i]; i++)
	    ViceLog(0, ("... %s\n", afs_inet_ntoa_r(htonl(inAddr->hostAddr[i]), hoststr)));
	fflush(stdout);
	fflush(stderr);
	printServerInfo();
	UBIK_ADDR_UNLOCK;
	return UBADHOST;
    }

    /* update our data structures */
    for (i = 1; i < UBIK_MAX_INTERFACE_ADDR; i++)
	ts->addr[i] = htonl(inAddr->hostAddr[i]);

    ViceLog(0, ("ubik: A Remote Server has addresses:\n"));
    for (i = 0; i < UBIK_MAX_INTERFACE_ADDR && ts->addr[i]; i++)
	ViceLog(0, ("... %s\n", afs_inet_ntoa_r(ts->addr[i], hoststr)));

    UBIK_ADDR_UNLOCK;

    /*
     * The most likely cause of a DISK_UpdateInterfaceAddr RPC
     * is because the server was restarted.  Reset its state
     * so that no DISK_Begin RPCs will be issued until the
     * known database version is current.
     */
    UBIK_BEACON_LOCK;
    ts->beaconSinceDown = 0;
    ts->currentDB = 0;
    urecovery_LostServer(ts);
    UBIK_BEACON_UNLOCK;
    return 0;
}

static void
printServerInfo(void)
{
    struct ubik_server *ts;
    int i, j = 1;
    char hoststr[16];

    ViceLog(0, ("Local CellServDB:\n"));
    for (ts = ubik_servers; ts; ts = ts->next, j++) {
	ViceLog(0, ("  Server %d:\n", j));
	for (i = 0; (i < UBIK_MAX_INTERFACE_ADDR) && ts->addr[i]; i++)
	    ViceLog(0, ("  ... %s\n", afs_inet_ntoa_r(ts->addr[i], hoststr)));
    }
}

afs_int32
SDISK_SetVersion(struct rx_call *rxcall, struct ubik_tid *atid,
		 struct ubik_version *oldversionp,
		 struct ubik_version *newversionp)
{
    afs_int32 code = 0;

    if ((code = ubik_CheckAuth(rxcall))) {
	return (code);
    }
    DBHOLD(ubik_dbase);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }
    /* sanity check to make sure only write trans appear here */
    if (ubik_currentTrans->type != UBIK_WRITETRANS) {
	code = UBADTYPE;
	goto done;
    }

    /* Should not get this for the sync site */
    if (ubeacon_AmSyncSite()) {
	code = UDEADLOCK;
	goto done;
    }

    urecovery_CheckTid(atid, 0);
    if (!ubik_currentTrans) {
	code = USYNC;
	goto done;
    }

    /* Set the label if our version matches the sync-site's. Also set the label
     * if our on-disk version matches the old version, and our view of the
     * sync-site's version matches the new version. This suggests that
     * ubik_dbVersion was updated while the sync-site was setting the new
     * version, and it already told us via VOTE_Beacon. */
    if (uvote_eq_dbVersion(*oldversionp)
	|| (uvote_eq_dbVersion(*newversionp)
	    && vcmp(ubik_dbase->version, *oldversionp) == 0)) {
	UBIK_VERSION_LOCK;
	code = udb_setlabel_trans(ubik_currentTrans, newversionp);
	if (!code) {
	    ubik_dbase->version = *newversionp;
	    uvote_set_dbVersion(*newversionp);
	}
	UBIK_VERSION_UNLOCK;
    } else {
	code = USYNC;
    }
done:
    DBRELE(ubik_dbase);
    return code;
}

afs_int32
SDISK_GetFile2(struct rx_call *rxcall)
{
    afs_int32 code;
    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }

    return uremote_sgetfile(rxcall, &urecovery_senddb_sgetfile2, NULL);
}

afs_int32
SDISK_SendFile2(struct rx_call *rxcall)
{
    afs_int32 code;
    if ((code = ubik_CheckAuth(rxcall))) {
	return code;
    }

    return uremote_ssendfile(rxcall, &urecovery_recvdb_ssendfile2, 0, NULL);
}

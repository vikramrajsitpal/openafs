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

#include <rx/rx.h>
#include <afs/afsutil.h>
#include <afs/cellconfig.h>


#include "ubik_internal.h"

/*! \file
 * This module is responsible for determining when the system has
 * recovered to the point that it can handle new transactions.  It
 * replays logs, polls to determine the current dbase after a crash,
 * and distributes the new database to the others.
 *
 * The sync site associates a version number with each database.  It
 * broadcasts the version associated with its current dbase in every
 * one of its beacon messages.  When the sync site send a dbase to a
 * server, it also sends the db's version.  A non-sync site server can
 * tell if it has the right dbase version by simply comparing the
 * version from the beacon message \p uvote_dbVersion with the version
 * associated with the database \p ubik_dbase->version.  The sync site
 * itself simply has one counter to keep track of all of this (again
 * \p ubik_dbase->version).
 *
 * sync site: routine called when the sync site loses its quorum; this
 * procedure is called "up" from the beacon package.  It resyncs the
 * dbase and nudges the recovery daemon to try to propagate out the
 * changes.  It also resets the recovery daemon's state, since
 * recovery must potentially find a new dbase to propagate out.  This
 * routine should not do anything with variables used by non-sync site
 * servers.
 */

/*!
 * if this flag is set, then ubik will use only the primary address
 * (the address specified in the CellServDB) to contact other
 * ubik servers. Ubik recovery will not try opening connections
 * to the alternate interface addresses.
 */
int ubikPrimaryAddrOnly;

int
urecovery_ResetState(void)
{
    urecovery_state = 0;
    return 0;
}

/*!
 * \brief sync site
 *
 * routine called when a non-sync site server goes down; restarts recovery
 * process to send missing server the new db when it comes back up for
 * non-sync site servers.
 *
 * \note This routine should not do anything with variables used by non-sync site servers.
 */
int
urecovery_LostServer(struct ubik_server *ts)
{
    ubeacon_ReinitServer(ts);
    return 0;
}

/*!
 * return true iff we have a current database (called by both sync
 * sites and non-sync sites) How do we determine this?  If we're the
 * sync site, we wait until recovery has finished fetching and
 * re-labelling its dbase (it may still be trying to propagate it out
 * to everyone else; that's THEIR problem).  If we're not the sync
 * site, then we must have a dbase labelled with the right version,
 * and we must have a currently-good sync site.
 */
int
urecovery_AllBetter(struct ubik_dbase *adbase, int areadAny)
{
    afs_int32 rcode;

    ViceLog(25, ("allbetter checking\n"));
    rcode = 0;


    if (areadAny) {
	if (ubik_dbase->version.epoch > 1)
	    rcode = 1;		/* Happy with any good version of database */
    }

    /* Check if we're sync site and we've got the right data */
    else if (ubeacon_AmSyncSite() && (urecovery_state & UBIK_RECHAVEDB)) {
	rcode = 1;
    }

    /* next, check if we're aux site, and we've ever been sent the
     * right data (note that if a dbase update fails, we won't think
     * that the sync site is still the sync site, 'cause it won't talk
     * to us until a timeout period has gone by.  When we recover, we
     * leave this clear until we get a new dbase */
    else if (uvote_HaveSyncAndVersion(ubik_dbase->version)) {
	rcode = 1;
    }

    ViceLog(25, ("allbetter: returning %d\n", rcode));
    return rcode;
}

/*!
 * \brief abort all transactions on this database
 */
int
urecovery_AbortAll(struct ubik_dbase *adbase)
{
    struct ubik_trans *tt;
    int reads = 0, writes = 0;

    for (tt = adbase->activeTrans; tt; tt = tt->next) {
	if (tt->type == UBIK_WRITETRANS)
	    writes++;
	else
	    reads++;
	udisk_abort(tt);
    }
    ViceLog(0, ("urecovery_AbortAll: just aborted %d read and %d write transactions\n",
		    reads, writes));
    return 0;
}

/*!
 * \brief this routine aborts the current remote transaction, if any, if the tid is wrong
 */
int
urecovery_CheckTid(struct ubik_tid *atid, int abortalways)
{
    if (ubik_currentTrans) {
	/* there is remote write trans, see if we match, see if this
	 * is a new transaction */
	if (atid->epoch != ubik_currentTrans->tid.epoch
	    || atid->counter > ubik_currentTrans->tid.counter || abortalways) {
	    /* don't match, abort it */
	    int endit = 0;
	    /* If the thread is not waiting for lock - ok to end it */
	    if (ubik_currentTrans->locktype != LOCKWAIT) {
		endit = 1;
	    }

	    ViceLog(0, ("urecovery_CheckTid: Aborting/ending bad remote "
			"transaction. (tx %d.%d, atid %d.%d, abortalways %d, "
			"endit %d)\n",
			ubik_currentTrans->tid.epoch,
			ubik_currentTrans->tid.counter,
			atid->epoch, atid->counter,
			abortalways, endit));
	    if (endit) {
		udisk_end(ubik_currentTrans);
	    }
	    ubik_currentTrans = (struct ubik_trans *)0;
	}
    }
    return 0;
}

/*!
 * \brief replay logs
 *
 * log format is defined here, and implicitly in disk.c
 *
 * 4 byte opcode, followed by parameters, each 4 bytes long.  All integers
 * are in logged in network standard byte order, in case we want to move logs
 * from machine-to-machine someday.
 *
 * Begin transaction: opcode \n
 * Commit transaction: opcode, version (8 bytes) \n
 * Truncate file (no longer used): opcode, file number, length \n
 * Abort transaction: opcode \n
 * Write data: opcode, file, position, length, <length> data bytes \n
 *
 * A very simple routine, it just replays the log.  Note that this is a new-value only log, which
 * implies that no uncommitted data is written to the dbase: one writes data to the log, including
 * the commit record, then we allow data to be written through to the dbase.  In our particular
 * implementation, once a transaction is done, we write out the pages to the database, so that
 * our buffer package doesn't have to know about stable and uncommitted data in the memory buffers:
 * any changed data while there is an uncommitted write transaction can be zapped during an
 * abort and the remaining dbase on the disk is exactly the right dbase, without having to read
 * the log.
 */
static int
ReplayLog(struct ubik_dbase *adbase)
{
    afs_int32 opcode;
    afs_int32 code, tpos;
    int logIsGood;
    afs_int32 len, thisSize, tfile, filePos;
    afs_int32 buffer[4];
    afs_int32 syncFile = -1;
    afs_int32 data[1024];

    if (ubik_KVDbase(adbase)) {
	return 0;
    }

    /* read the lock twice, once to see whether we have a transaction to deal
     * with that committed, (theoretically, we should support more than one
     * trans in the log at once, but not yet), and once replaying the
     * transactions.  */
    tpos = 0;
    logIsGood = 0;
    /* for now, assume that all ops in log pertain to one transaction; see if there's a commit */
    while (1) {
	code = uphys_read(adbase, LOGFILE, &opcode, tpos, sizeof(afs_int32));
	if (code != sizeof(afs_int32))
	    break;
	opcode = ntohl(opcode);
	if (opcode == LOGNEW) {
	    /* handle begin trans */
	    tpos += sizeof(afs_int32);
	} else if (opcode == LOGABORT)
	    break;
	else if (opcode == LOGEND) {
	    logIsGood = 1;
	    break;
	} else if (opcode == LOGDATA) {
	    tpos += 4;
	    code = uphys_read(adbase, LOGFILE, buffer, tpos,
			      3 * sizeof(afs_int32));
	    if (code != 3 * sizeof(afs_int32))
		break;
	    /* otherwise, skip over the data bytes, too */
	    tpos += ntohl(buffer[2]) + 3 * sizeof(afs_int32);
	} else {
	    ViceLog(0, ("corrupt log opcode (%d) at position %d\n", opcode,
		       tpos));
	    break;		/* corrupt log! */
	}
    }
    if (logIsGood) {
	/* actually do the replay; log should go all the way through the commit record, since
	 * we just read it above. */
	tpos = 0;
	logIsGood = 0;
	syncFile = -1;
	while (1) {
	    code = uphys_read(adbase, LOGFILE, &opcode, tpos,
			      sizeof(afs_int32));
	    if (code != sizeof(afs_int32))
		break;
	    opcode = ntohl(opcode);
	    if (opcode == LOGNEW) {
		/* handle begin trans */
		tpos += sizeof(afs_int32);
	    } else if (opcode == LOGABORT)
		panic("log abort\n");
	    else if (opcode == LOGEND) {
		struct ubik_version version;
		tpos += 4;
		code = uphys_read(adbase, LOGFILE, buffer, tpos,
				  2 * sizeof(afs_int32));
		if (code != 2 * sizeof(afs_int32))
		    return UBADLOG;
		version.epoch = ntohl(buffer[0]);
		version.counter = ntohl(buffer[1]);
		code = uphys_setlabel(adbase, 0, &version);
		if (code)
		    return code;
		ViceLog(0, ("Successfully replayed log for interrupted "
		           "transaction; db version is now %ld.%ld\n",
		           (long) version.epoch, (long) version.counter));
		logIsGood = 1;
		break;		/* all done now */
	    } else if (opcode == LOGDATA) {
		tpos += 4;
		code = uphys_read(adbase, LOGFILE, buffer, tpos,
				  3 * sizeof(afs_int32));
		if (code != 3 * sizeof(afs_int32))
		    break;
		tpos += 3 * sizeof(afs_int32);
		/* otherwise, skip over the data bytes, too */
		len = ntohl(buffer[2]);	/* total number of bytes to copy */
		filePos = ntohl(buffer[1]);
		tfile = ntohl(buffer[0]);
		/* try to minimize file syncs */
		if (syncFile != tfile) {
		    if (syncFile >= 0)
			code = uphys_sync(adbase, syncFile);
		    else
			code = 0;
		    syncFile = tfile;
		    if (code)
			return code;
		}
		while (len > 0) {
		    thisSize = (len > sizeof(data) ? sizeof(data) : len);
		    /* copy sizeof(data) buffer bytes at a time */
		    code = uphys_read(adbase, LOGFILE, data, tpos, thisSize);
		    if (code != thisSize)
			return UBADLOG;
		    code = uphys_write(adbase, tfile, data, filePos, thisSize);
		    if (code != thisSize)
			return UBADLOG;
		    filePos += thisSize;
		    tpos += thisSize;
		    len -= thisSize;
		}
	    } else {
		ViceLog(0, ("corrupt log opcode (%d) at position %d\n",
			   opcode, tpos));
		break;		/* corrupt log! */
	    }
	}
	if (logIsGood) {
	    if (syncFile >= 0)
		code = uphys_sync(adbase, syncFile);
	    if (code)
		return code;
	} else {
	    ViceLog(0, ("Log read error on pass 2\n"));
	    return UBADLOG;
	}
    }

    /* now truncate the log, we're done with it */
    code = uphys_truncate(adbase, LOGFILE, 0);
    return code;
}

/*! \brief
 * Called at initialization to figure out version of the dbase we really have.
 *
 * This routine is called after replaying the log; it reads the restored labels.
 */
static int
InitializeDB(struct ubik_dbase *adbase)
{
    afs_int32 code;

    code = udb_getlabel_db(adbase, &adbase->version);
    if (code) {
	/* try setting the label to a new value */
	UBIK_VERSION_LOCK;
	adbase->version.epoch = 1;	/* value for newly-initialized db */
	adbase->version.counter = 1;
	code = udb_setlabel_db(adbase, &adbase->version);
	if (code) {
	    /* failed, try to set it back */
	    adbase->version.epoch = 0;
	    adbase->version.counter = 0;
	    udb_setlabel_db(adbase, &adbase->version);
	}
	UBIK_VERSION_UNLOCK;
    }
    return 0;
}

/*!
 * \brief initialize the local ubik_dbase
 *
 * We replay the logs and then read the resulting file to figure out what version we've really got.
 */
int
urecovery_Initialize(struct ubik_dbase *adbase)
{
    afs_int32 code;

    DBHOLD(adbase);
    code = ReplayLog(adbase);
    if (code)
	goto done;
    code = InitializeDB(adbase);
done:
    DBRELE(adbase);
    return code;
}

static int
do_StartDISK_GetFile(struct rx_call *rxcall,
		     afs_int32 *a_length)
{
    afs_int32 length;
    afs_int32 nbytes;
    int code;

    code = StartDISK_GetFile(rxcall, 0);
    if (code != 0) {
	code = UIOERROR;
	goto done;
    }

    nbytes = rx_Read(rxcall, (char *)&length, sizeof(length));
    if (nbytes != sizeof(length)) {
	code = UIOERROR;
	goto done;
    }

    *a_length = ntohl(length);

    code = 0;

 done:
    return code;
}

static int
recvdb_oldstyle(struct urecovery_recvdb_type *rtype,
		struct urecovery_recvdb_info *rinfo, struct rx_call *rxcall,
		char *path, struct ubik_version *a_version)
{
    int code;
    int client = rtype->client;
    struct ubik_version version;

    memset(&version, 0, sizeof(version));

    if (client) {
	afs_int32 length = 0;

	code = do_StartDISK_GetFile(rxcall, &length);
	if (code != 0) {
	    goto done;
	}

	code = uphys_recvdb(rxcall, path, NULL, length);
	if (code != 0) {
	    goto done;
	}

	code = EndDISK_GetFile(rxcall, &version);
	if (code != 0) {
	    goto done;
	}

	code = uphys_setlabel_path(path, &version);
	if (code != 0) {
	    goto done;
	}

    } else {
	opr_Assert(rinfo->flat_version != NULL);
	version = *rinfo->flat_version;

	code = uphys_recvdb(rxcall, path, &version, rinfo->flat_length);
	if (code != 0) {
	    goto done;
	}
    }

    *a_version = version;

 done:
    return code;
}

struct urecovery_recvdb_type urecovery_recvdb_getfile_old = {
    .descr = "DISK_GetFile",
    .client = 1,
    .old_rpc = 1,
};
struct urecovery_recvdb_type urecovery_recvdb_ssendfile_old = {
    .descr = "SDISK_SendFile",
    .client = 0,
    .old_rpc = 1,
};
struct urecovery_recvdb_type urecovery_recvdb_getfile2 = {
    .descr = "DISK_GetFile2",
    .client = 1,
    .old_rpc = 0,
};
struct urecovery_recvdb_type urecovery_recvdb_ssendfile2 = {
    .descr = "SDISK_SendFile2",
    .client = 0,
    .old_rpc = 0,
};

/**
 * Receive a ubik database from another server.
 *
 * @pre DBHOLD held
 *
 * @param[in] dbase Database to receive (ubik_dbase).
 * @param[in] rtype The type of receive to perform (client vs server, which rpc
 *		    variant, etc).
 * @param[in] rinfo Various info about receiving the db.
 * @param[out] a_version    Optional. If non-NULL, set to the version of the
 *			    database we received, on success.
 *
 * @returns ubik error codes
 */
afs_int32
urecovery_receive_db(struct ubik_dbase *dbase,
		     struct urecovery_recvdb_type *rtype,
		     struct urecovery_recvdb_info *rinfo,
		     struct ubik_version *a_version)
{
    afs_int32 code;
    char hoststr[16];
    struct ubik_version version;
    int client = rtype->client;
    int old_rpc = rtype->old_rpc;
    char *descr = rtype->descr;
    struct rx_call *rxcall = NULL;
    char *path_tmp = NULL;

    memset(&version, 0, sizeof(version));

    /*
     * Wait until we're not sending the database to someone. Since every
     * transaction will be aborted (via urecovery_AbortAll below), we do not
     * have to wait for DBWRITING to go away.
     */
    if (ubik_wait_db_flags(ubik_dbase, DBSENDING)) {
	ViceLog(0, ("ubik: Error, saw unexpected database flags 0x%x before "
		    "receiving db from %s (via %s)\n",
		    dbase->dbFlags,
		    afs_inet_ntoa_r(rinfo->otherHost, hoststr),
		    descr));
	return UINTERNAL;
    }
    ubik_set_db_flags(dbase, DBRECEIVING);

    ViceLog(0, ("ubik: Receiving db from %s (via %s)\n",
	       afs_inet_ntoa_r(rinfo->otherHost, hoststr),
	       descr));

    /* Abort any active trans that may scribble over the database. */
    urecovery_AbortAll(dbase);

    /*
     * At this point, we know that we do not have any active read or write
     * transactions (they were just aborted), and we know that we are not
     * sending or receiving a database. We drop DBHOLD here, so new read
     * transactions can come in while the database is being transferred. That
     * might seem a bit odd because we just aborted any existing read
     * transactions; in the future we could probably avoid aborting read
     * transactions (and just abort writes), but for now we just abort
     * everything.
     */
    DBRELE(dbase);

    /* Remove any .TMP/.OLD databases that might be leftover from a previous
     * interrupted receive or whatever. */
    code = udb_del_suffixes(dbase, ".TMP", ".OLD");
    if (code != 0) {
	goto done;
    }

    if (client) {
	opr_Assert(rinfo->rxcall == NULL);
	opr_Assert(rinfo->rxconn != NULL);
	rxcall = rx_NewCall(rinfo->rxconn);

    } else {
	rxcall = rinfo->rxcall;
    }
    opr_Assert(rxcall != NULL);

    code = udb_path(dbase, ".TMP", &path_tmp);
    if (code != 0) {
	goto done;
    }

    /* Receive the database data from the wire into a .TMP database, and label
     * it with the received version. */

    if (old_rpc) {
	code = recvdb_oldstyle(rtype, rinfo, rxcall, path_tmp, &version);
	if (code != 0) {
	    goto done;
	}

    } else {
	if (client) {
	    code = StartDISK_GetFile2(rxcall);
	    if (code != 0) {
		goto done;
	    }
	}

	code = udb_recvdb_stream(rxcall, path_tmp, &version);
	if (code != 0) {
	    goto done;
	}

	if (client) {
	    code = EndDISK_GetFile2(rxcall);
	    if (code != 0) {
		goto done;
	    }
	}
    }

    if (client) {
	code = rx_EndCall(rxcall, code);
	rxcall = NULL;
	if (code != 0) {
	    goto done;
	}
    }

    /* Pivot our .TMP database (already labelled with 'version') into place, to
     * start using it. */
    code = udb_install(dbase, ".TMP", NULL, &version);
    if (code != 0) {
	goto done;
    }

    if (a_version != NULL) {
	*a_version = version;
    }

 done:
    free(path_tmp);
    if (client && rxcall != NULL) {
	code = rx_EndCall(rxcall, code);
    }
    if (code != 0) {
	ViceLog(0, ("ubik: Failed to receive db from %s (via %s), "
		    "error=%d\n",
		    afs_inet_ntoa_r(rinfo->otherHost, hoststr),
		    descr,
		    code));

    } else {
	ViceLog(0, ("ubik: Finished receiving db from %s (via %s), "
		    "version=%d.%d\n",
		    afs_inet_ntoa_r(rinfo->otherHost, hoststr),
		    descr,
		    version.epoch, version.counter));
    }

    DBHOLD(dbase);

    ubik_clear_db_flags(ubik_dbase, DBRECEIVING);

    return code;
}

static afs_int32
fetch_db(struct ubik_dbase *dbase, struct ubik_server *ts)
{
    struct urecovery_recvdb_info rinfo;
    afs_int32 code;
    char hoststr[16];

    memset(&rinfo, 0, sizeof(rinfo));

    UBIK_ADDR_LOCK;
    rinfo.otherHost = ts->addr[0];
    rinfo.rxconn = ts->disk_rxcid;
    rx_GetConnection(rinfo.rxconn);
    UBIK_ADDR_UNLOCK;

    code = urecovery_receive_db(dbase, &urecovery_recvdb_getfile2, &rinfo,
				NULL);
    if (code == RXGEN_OPCODE) {
	static int warned;

	/*
	 * If we got an RXGEN_OPCODE error, the remote server probably doesn't
	 * understand the DISK_GetFile2 RPC; try again with the old-style
	 * DISK_GetFile RPC.
	 */
	if (!warned) {
	    warned = 1;
	    afs_inet_ntoa_r(rinfo.otherHost, hoststr);
	    ViceLog(0, ("ubik: Warning: %s doesn't seem to support the "
			"DISK_GetFile2 RPC. Retrying with DISK_GetFile, but %s "
			"should perhaps be upgraded. "
			"(This message is only logged once.)\n",
			hoststr, hoststr));
	}

	code = urecovery_receive_db(dbase, &urecovery_recvdb_getfile_old,
				    &rinfo, NULL);
    }

    rx_PutConnection(rinfo.rxconn);

    return code;
}

static int
senddb_oldstyle(struct urecovery_senddb_type *stype, char *path,
		struct rx_call *rxcall, struct ubik_version *version)
{
    int code;
    int client = stype->client;
    struct ubik_stat ustat;
    afs_int32 length;

    memset(&ustat, 0, sizeof(ustat));

    code = uphys_stat_path(path, &ustat);
    if (code != 0) {
	goto done;
    }

    length = ustat.size;

    if (client) {
	code = StartDISK_SendFile(rxcall, 0, length, version);
	if (code != 0) {
	    goto done;
	}

    } else {
	afs_int32 nbytes;
	afs_int32 tlen = htonl(length);

	nbytes = rx_Write(rxcall, (char*)&tlen, sizeof(tlen));
	if (nbytes != sizeof(tlen)) {
	    ViceLog(0, ("Rx-write length error, nbytes=%d/%d, call error=%d\n",
		    nbytes, (int)sizeof(tlen), rx_Error(rxcall)));
	    code = UIOERROR;
	    goto done;
	}
    }

    code = uphys_senddb(path, rxcall, version, length);
    if (code != 0) {
	goto done;
    }

    if (client) {
	code = EndDISK_SendFile(rxcall);
	if (code != 0) {
	    goto done;
	}
    }

 done:
    return code;
}

struct urecovery_senddb_type urecovery_senddb_sendfile_old = {
    .descr = "DISK_SendFile",
    .client = 1,
    .old_rpc = 1,
};
struct urecovery_senddb_type urecovery_senddb_sgetfile_old = {
    .descr = "SDISK_GetFile",
    .client = 0,
    .old_rpc = 1,
};
struct urecovery_senddb_type urecovery_senddb_sendfile2 = {
    .descr = "DISK_SendFile2",
    .client = 1,
    .old_rpc = 0,
};
struct urecovery_senddb_type urecovery_senddb_sgetfile2 = {
    .descr = "SDISK_GetFile2",
    .client = 0,
    .old_rpc = 0,
};

/**
 * Send the ubik database to another server.
 *
 * @pre DBHOLD held
 * @pre DBSENDING must be set, if 'nosetflags' is nonzero
 *
 * @param[in] dbase Database to send (ubik_dbase).
 * @param[in] stype The type of send to perform (client vs server, which rpc
 *		    variant, etc).
 * @param[in] sinfo Various info needed for sending the db.
 * @param[out] version	Optional. If non-NULL, set to the version of the
 *			database that was sent, on success.
 *
 * @returns ubik error codes
 */
afs_int32
urecovery_send_db(struct ubik_dbase *dbase,
		  struct urecovery_senddb_type *stype,
		  struct urecovery_senddb_info *sinfo,
		  struct ubik_version *a_version)
{
    afs_int32 code;
    char hoststr[16];
    struct ubik_version version;
    char *descr = stype->descr;
    int client = stype->client;
    int old_rpc = stype->old_rpc;
    int nosetflags = sinfo->nosetflags;
    struct rx_call *rxcall = NULL;
    int start_logged = 0;
    char *path = NULL;

    memset(&version, 0, sizeof(version));

    if (nosetflags) {
	/* Our caller must have already set DBSENDING. */
	opr_Assert((dbase->dbFlags & DBSENDING) != 0);
    } else {
	/* wait until we're not sending, receiving, or changing the database */
	code = ubik_wait_db_flags(dbase, DBWRITING | DBSENDING | DBRECEIVING);
	osi_Assert(code == 0);
	ubik_set_db_flags(dbase, DBSENDING);
    }

    if (ubik_KVDbase(dbase) && old_rpc) {
	ViceLog(0, ("ubik: Cannot send KV db to %s via %s\n",
		afs_inet_ntoa_r(sinfo->otherHost, hoststr), descr));
	code = UBADTYPE;
	goto done_locked;
    }

    code = udb_getlabel_db(dbase, &version);
    if (code != 0) {
	goto done_locked;
    }

    /*
     * At this point, we know that nobody is writing to the database, and we're
     * not sending or receiving the db. So, we can drop DBHOLD here to allow
     * read transactions.
     */
    DBRELE(dbase);

    code = udb_path(dbase, NULL, &path);
    if (code != 0) {
	goto done;
    }

    ViceLog(0, ("ubik: Sending db to %s (via %s), version=%d.%d\n",
		afs_inet_ntoa_r(sinfo->otherHost, hoststr), descr,
		version.epoch, version.counter));
    start_logged = 1;

    if (client) {
	opr_Assert(sinfo->rxcall == NULL);
	opr_Assert(sinfo->rxconn != NULL);
	rxcall = rx_NewCall(sinfo->rxconn);
    } else {
	rxcall = sinfo->rxcall;
    }
    opr_Assert(rxcall != NULL);

    if (old_rpc) {
	code = senddb_oldstyle(stype, path, rxcall, &version);
	if (code != 0) {
	    goto done;
	}

    } else {
	if (client) {
	    code = StartDISK_SendFile2(rxcall);
	    if (code != 0) {
		goto done;
	    }
	}

	code = udb_senddb_stream(path, rxcall, &version);
	if (code != 0) {
	    goto done;
	}

	if (client) {
	    code = EndDISK_SendFile2(rxcall);
	    if (code != 0) {
		goto done;
	    }
	}
    }

    if (a_version != NULL) {
	*a_version = version;
    }

 done:
    if (client && rxcall != NULL) {
	code = rx_EndCall(rxcall, code);
    }

    if (start_logged) {
	if (code != 0) {
	    ViceLog(0, ("ubik: Failed to send db to %s (via %s), "
			"error=%d\n",
			afs_inet_ntoa_r(sinfo->otherHost, hoststr),
			descr, code));

	} else {
	    ViceLog(0, ("ubik: Finished sending db to %s (via %s), "
			"version=%d.%d\n",
			afs_inet_ntoa_r(sinfo->otherHost, hoststr),
			descr,
			version.epoch, version.counter));
	}
    }

    free(path);

    DBHOLD(dbase);

 done_locked:
    if (!nosetflags) {
	ubik_clear_db_flags(dbase, DBSENDING);
    }

    return code;
}

static afs_int32
dist_dbase_to(struct ubik_dbase *dbase, struct ubik_server *ts,
	      afs_uint32 otherHost)
{
    struct urecovery_senddb_info sinfo;
    struct ubik_version version;
    int code;

    memset(&version, 0, sizeof(version));
    memset(&sinfo, 0, sizeof(sinfo));

    sinfo.otherHost = otherHost;
    sinfo.nosetflags = 1;

    UBIK_ADDR_LOCK;
    sinfo.rxconn = ts->disk_rxcid;
    rx_GetConnection(sinfo.rxconn);
    UBIK_ADDR_UNLOCK;

    code = urecovery_send_db(dbase, &urecovery_senddb_sendfile2, &sinfo,
			     &version);
    if (code == RXGEN_OPCODE && !ubik_KVDbase(dbase)) {
	static int warned;
	char hoststr[16];

	/*
	 * If we got an RXGEN_OPCODE error, the remote server probably doesn't
	 * understand the DISK_SendFile2 RPC. If we have a non-KV database, try
	 * again with the old-style DISK_SendFile RPC. If we have a KV
	 * database, we can't use the old-style RPC, so there's nothing to
	 * retry.
	 */
	if (!warned) {
	    warned = 1;
	    afs_inet_ntoa_r(otherHost, hoststr);
	    ViceLog(0, ("ubik: Warning: %s doesn't seem to support the "
			"DISK_SendFile2 RPC. Retrying with DISK_SendFile, but "
			"%s should perhaps be upgraded. "
			"(This message is only logged once.)\n",
			hoststr, hoststr));
	}

	code = urecovery_send_db(dbase, &urecovery_senddb_sendfile_old, &sinfo,
				 &version);
    }

    if (code == 0) {
	/* we set a new file */
	ts->version = version;
	ts->currentDB = 1;
    }

    rx_PutConnection(sinfo.rxconn);

    return code;
}

int
urecovery_distribute_db(struct ubik_dbase *dbase, int *a_nsent)
{
    struct ubik_server *ts;
    struct in_addr inAddr;
    char hoststr[16];
    afs_int32 code;
    int n_sent = 0;
    int dbok;

    memset(&inAddr, 0, sizeof(inAddr));

    dbok = 1;		/* start off assuming they all worked */

    for (ts = ubik_servers; ts; ts = ts->next) {
	UBIK_ADDR_LOCK;
	inAddr.s_addr = ts->addr[0];
	UBIK_ADDR_UNLOCK;
	UBIK_BEACON_LOCK;
	if (!ts->up) {
	    UBIK_BEACON_UNLOCK;
	    /* It would be nice to have this message at loglevel
	     * 0 as well, but it will log once every 4s for each
	     * down server while in this recovery state.  This
	     * should only be changed to loglevel 0 if it is
	     * also rate-limited.
	     */
	    ViceLog(5, ("recovery cannot send version to %s\n",
			afs_inet_ntoa_r(inAddr.s_addr, hoststr)));
	    dbok = 0;
	    continue;
	}
	UBIK_BEACON_UNLOCK;

	if (vcmp(ts->version, dbase->version) != 0) {
	    /* This guy has an old version of the db; send our db to
	     * them. */
	    code = dist_dbase_to(dbase, ts, inAddr.s_addr);
	    if (code != 0) {
		dbok = 0;
	    } else {
		n_sent++;
	    }
	} else {
	    /* mark file up to date */
	    ts->currentDB = 1;
	}
    }

    if (a_nsent != NULL) {
	*a_nsent = n_sent;
    }
    if (!dbok) {
	return -1;
    }
    return 0;
}

/*!
 * \brief Main interaction loop for the recovery manager
 *
 * The recovery light-weight process only runs when you're the
 * synchronization site.  It performs the following tasks, if and only
 * if the prerequisite tasks have been performed successfully (it
 * keeps track of which ones have been performed in its bit map,
 * \p urecovery_state).
 *
 * First, it is responsible for probing that all servers are up.  This
 * is the only operation that must be performed even if this is not
 * yet the sync site, since otherwise this site may not notice that
 * enough other machines are running to even elect this guy to be the
 * sync site.
 *
 * After that, the recovery process does nothing until the beacon and
 * voting modules manage to get this site elected sync site.
 *
 * After becoming sync site, recovery first attempts to find the best
 * database available in the network (it must do this in order to
 * ensure finding the latest committed data).  After finding the right
 * database, it must fetch this dbase to the sync site.
 *
 * After fetching the dbase, it relabels it with a new version number,
 * to ensure that everyone recognizes this dbase as the most recent
 * dbase.
 *
 * One the dbase has been relabelled, this machine can start handling
 * requests.  However, the recovery module still has one more task:
 * propagating the dbase out to everyone who is up in the network.
 */
void *
urecovery_Interact(void *dummy)
{
    afs_int32 code;
    struct ubik_server *bestServer = NULL;
    struct ubik_server *ts;
    int doingRPC, now;
    afs_int32 lastProbeTime;
    /* if we're the sync site, the best db version we've found yet */
    static struct ubik_version bestDBVersion;
    struct timeval tv;
    int first;

    opr_threadname_set("recovery");

    /* otherwise, begin interaction */
    urecovery_state = 0;
    lastProbeTime = 0;
    for (first = 1; ; first = 0) {
	if (!first) {
	    /* Run through this loop every 4 seconds (but don't wait 4 seconds
	     * the first time around). */
	    tv.tv_sec = 4;
	    tv.tv_usec = 0;
#ifdef AFS_PTHREAD_ENV
	    select(0, 0, 0, 0, &tv);
#else
	    IOMGR_Select(0, 0, 0, 0, &tv);
#endif
	}

	ViceLog(5, ("recovery running in state %x\n", urecovery_state));

	/* Every 30 seconds, check all the down servers and mark them
	 * as up if they respond. When a server comes up or found to
	 * not be current, then re-find the the best database and
	 * propogate it.
	 */
	if ((now = FT_ApproxTime()) > 30 + lastProbeTime) {

	    for (ts = ubik_servers, doingRPC = 0; ts; ts = ts->next) {
		UBIK_BEACON_LOCK;
		if (!ts->up) {
		    UBIK_BEACON_UNLOCK;
		    doingRPC = 1;
		    code = DoProbe(ts);
		    if (code == 0) {
			UBIK_BEACON_LOCK;
			ts->up = 1;
			UBIK_BEACON_UNLOCK;
			DBHOLD(ubik_dbase);
			urecovery_state &= ~UBIK_RECFOUNDDB;
			DBRELE(ubik_dbase);
		    }
		} else {
		    UBIK_BEACON_UNLOCK;
		    DBHOLD(ubik_dbase);
		    if (!ts->currentDB)
			urecovery_state &= ~UBIK_RECFOUNDDB;
		    DBRELE(ubik_dbase);
		}
	    }

	    if (doingRPC)
		now = FT_ApproxTime();
	    lastProbeTime = now;
	}

	/* Mark whether we are the sync site */
	DBHOLD(ubik_dbase);
	if (!ubeacon_AmSyncSite()) {
	    urecovery_state &= ~UBIK_RECSYNCSITE;
	    DBRELE(ubik_dbase);
	    continue;		/* nothing to do */
	}
	urecovery_state |= UBIK_RECSYNCSITE;

	/* If a server has just come up or if we have not found the
	 * most current database, then go find the most current db.
	 */
	if (!(urecovery_state & UBIK_RECFOUNDDB)) {
            int okcalls = 0;
	    DBRELE(ubik_dbase);
	    bestServer = (struct ubik_server *)0;
	    bestDBVersion.epoch = 0;
	    bestDBVersion.counter = 0;
	    for (ts = ubik_servers; ts; ts = ts->next) {
		UBIK_BEACON_LOCK;
		if (!ts->up) {
		    UBIK_BEACON_UNLOCK;
		    continue;	/* don't bother with these guys */
		}
		UBIK_BEACON_UNLOCK;
		if (ts->isClone)
		    continue;
		UBIK_ADDR_LOCK;
		code = DISK_GetVersion(ts->disk_rxcid, &ts->version);
		UBIK_ADDR_UNLOCK;
		if (code == 0) {
                    okcalls++;
		    /* perhaps this is the best version */
		    if (vcmp(ts->version, bestDBVersion) > 0) {
			/* new best version */
			bestDBVersion = ts->version;
			bestServer = ts;
		    }
		}
	    }

	    DBHOLD(ubik_dbase);

            if (okcalls + 1 >= ubik_quorum) {
                /* If we've asked a majority of sites about their db version,
                 * then we can say with confidence that we've found the best db
                 * version. If we haven't contacted most sites (because
                 * GetVersion failed or because we already know the server is
                 * down), then we don't really know if we know about the best
                 * db version. So we can only proceed in here if 'okcalls'
                 * indicates we managed to contact a majority of sites. */

                /* take into consideration our version. Remember if we,
                 * the sync site, have the best version. Also note that
                 * we may need to send the best version out.
                 */
                if (vcmp(ubik_dbase->version, bestDBVersion) >= 0) {
                    bestDBVersion = ubik_dbase->version;
                    bestServer = (struct ubik_server *)0;
                    urecovery_state |= UBIK_RECHAVEDB;
                } else {
                    /* Clear the flag only when we know we have to retrieve
                     * the db. Because urecovery_AllBetter() looks at it.
                     */
                    urecovery_state &= ~UBIK_RECHAVEDB;
                }
                urecovery_state |= UBIK_RECFOUNDDB;
                urecovery_state &= ~UBIK_RECSENTDB;
            }
	}
	if (!(urecovery_state & UBIK_RECFOUNDDB)) {
	    DBRELE(ubik_dbase);
	    continue;		/* not ready */
	}

	/* If we, the sync site, do not have the best db version, then
	 * go and get it from the server that does.
	 */
	if ((urecovery_state & UBIK_RECHAVEDB) || !bestServer) {
	    urecovery_state |= UBIK_RECHAVEDB;
	} else {
	    /* we don't have the best version; we should fetch it. */
	    code = fetch_db(ubik_dbase, bestServer);
	    if (code == 0) {
		urecovery_state |= UBIK_RECHAVEDB;
	    }
	}
	if (!(urecovery_state & UBIK_RECHAVEDB)) {
	    DBRELE(ubik_dbase);
	    continue;		/* not ready */
	}

	/* If the database was newly initialized, then when we establish quorum, write
	 * a new label. This allows urecovery_AllBetter() to allow access for reads.
	 * Setting it to 2 also allows another site to come along with a newer
	 * database and overwrite this one.
	 */
	if (ubik_dbase->version.epoch == 1) {
	    urecovery_AbortAll(ubik_dbase);
	    UBIK_VERSION_LOCK;
	    ubik_dbase->version.epoch = 2;
	    ubik_dbase->version.counter = 1;
	    code = udb_setlabel_db(ubik_dbase, &ubik_dbase->version);
	    UBIK_VERSION_UNLOCK;
	    udisk_Invalidate(ubik_dbase, 0);	/* data may have changed */
	}

	/* Check the other sites and send the database to them if they
	 * do not have the current db.
	 */
	if (!(urecovery_state & UBIK_RECSENTDB)) {
	    /* now propagate out new version to everyone else */

	    /*
	     * Check if a write transaction is in progress. We can't send the
	     * db when a write is in progress here because the db would be
	     * obsolete as soon as it goes there. Also, ops after the begin
	     * trans would reach the recepient and wouldn't find a transaction
	     * pending there.  Frankly, I don't think it's possible to get past
	     * the write-lock above if there is a write transaction in progress,
	     * but then, it won't hurt to check, will it?
	     */
	    if (ubik_wait_db_flags(ubik_dbase, DBWRITING | DBSENDING)) {
		ViceLog(0, ("Ubik: Unexpected database flags before DISK_SendFile "
			   "(flags: 0x%x)\n", ubik_dbase->dbFlags));
		DBRELE(ubik_dbase);
		continue;
	    }
	    ubik_set_db_flags(ubik_dbase, DBSENDING);

	    code = urecovery_distribute_db(ubik_dbase, NULL);

	    ubik_clear_db_flags(ubik_dbase, DBSENDING);

	    if (code == 0)
		urecovery_state |= UBIK_RECSENTDB;
	}
	DBRELE(ubik_dbase);
    }
    AFS_UNREACHED(return(NULL));
}

/*!
 * \brief send a Probe to all the network address of this server
 *
 * \return 0 if success, else return 1
 */
int
DoProbe(struct ubik_server *server)
{
    struct rx_connection *conns[UBIK_MAX_INTERFACE_ADDR];
    struct rx_connection *connSuccess = 0;
    int i, nconns, success_i = -1;
    afs_uint32 addr;
    char buffer[32];
    char hoststr[16];

    UBIK_ADDR_LOCK;
    for (i = 0; (i < UBIK_MAX_INTERFACE_ADDR) && (addr = server->addr[i]);
	 i++) {
	conns[i] =
	    rx_NewConnection(addr, ubik_callPortal, DISK_SERVICE_ID,
			     addr_globals.ubikSecClass, addr_globals.ubikSecIndex);

	/* user requirement to use only the primary interface */
	if (ubikPrimaryAddrOnly) {
	    i = 1;
	    break;
	}
    }
    UBIK_ADDR_UNLOCK;
    nconns = i;
    opr_Assert(nconns);			/* at least one interface address for this server */

    multi_Rx(conns, nconns) {
	multi_DISK_Probe();
	if (!multi_error) {	/* first success */
	    success_i = multi_i;

	    multi_Abort;
	}
    } multi_End;

    if (success_i >= 0) {
	UBIK_ADDR_LOCK;
	addr = server->addr[success_i];	/* successful interface addr */

	if (server->disk_rxcid)	/* destroy existing conn */
	    rx_DestroyConnection(server->disk_rxcid);
	if (server->vote_rxcid)
	    rx_DestroyConnection(server->vote_rxcid);

	/* make new connections */
	server->disk_rxcid = conns[success_i];
	server->vote_rxcid = rx_NewConnection(addr, ubik_callPortal,
	                                      VOTE_SERVICE_ID, addr_globals.ubikSecClass,
	                                      addr_globals.ubikSecIndex);

	connSuccess = conns[success_i];
	strcpy(buffer, afs_inet_ntoa_r(server->addr[0], hoststr));

	ViceLog(0, ("ubik:server %s is back up: will be contacted through %s\n",
	     buffer, afs_inet_ntoa_r(addr, hoststr)));
	UBIK_ADDR_UNLOCK;
    }

    /* Destroy all connections except the one on which we succeeded */
    for (i = 0; i < nconns; i++)
	if (conns[i] != connSuccess)
	    rx_DestroyConnection(conns[i]);

    if (!connSuccess)
	ViceLog(5, ("ubik:server %s still down\n",
		    afs_inet_ntoa_r(server->addr[0], hoststr)));

    if (connSuccess)
	return 0;		/* success */
    else
	return 1;		/* failure */
}

/**
 * Add new flag(s) to the database.
 *
 * @param[in]  dbase  database
 * @param[in]  flags  new flag(s)
 *
 * @pre        DBHOLD
 */
void
ubik_set_db_flags(struct ubik_dbase *dbase, int flags)
{
    osi_Assert((dbase->dbFlags & flags) == 0);
    dbase->dbFlags |= flags;
#ifdef AFS_PTHREAD_ENV
    opr_cv_broadcast(&dbase->flags_cond);
#else
    LWP_NoYieldSignal(&dbase->dbFlags);
#endif
}

/**
 * Clear flag(s) from the database.
 *
 * @param[in]  dbase  database
 * @param[in]  flags  flag(s) to be cleared
 *
 * @pre        DBHOLD
 */
void
ubik_clear_db_flags(struct ubik_dbase *dbase, int flags)
{
    osi_Assert((dbase->dbFlags & flags) == flags);
    dbase->dbFlags &= ~flags;
#ifdef AFS_PTHREAD_ENV
    opr_cv_broadcast(&dbase->flags_cond);
#else
    LWP_NoYieldSignal(&dbase->dbFlags);
#endif
}

/**
 * Wait until flag(s) are cleared.
 *
 * Note that if DBSENDING is set, and it's not specified in wait_flags, then we
 * bail out with an error. The same goes for DBRECEIVING.
 *
 * @param[in]  dbase      database
 * @param[in]  wait_flags flag(s) we will wait for
 *
 * @pre        DBHOLD
 *
 * @return status
 *   @retval  0  expected flag(s) cleared
 *   @retval -1  unexpected flag(s) found
 */
int
ubik_wait_db_flags(struct ubik_dbase *dbase, int wait_flags)
{
    /* For DBSENDING/DBRECEIVING, we cannot return success while those flags
     * are set. Either the caller wants to wait for those flags to go away (by
     * specifying them in wait_flags), or it is an error if they are specified
     * while we are waiting. */
    int bail_flags = DBSENDING | DBRECEIVING;
    bail_flags &= ~wait_flags;

    if (bail_flags && (dbase->dbFlags & bail_flags)) {
	return -1;
    }
    while ((dbase->dbFlags & wait_flags)) {
	if (bail_flags && (dbase->dbFlags & bail_flags)) {
	    return -1;
	}
	ViceLog(125, ("ubik: waiting for the following database flags to go "
		    "away: 0x%x\n", dbase->dbFlags));
#ifdef AFS_PTHREAD_ENV
	opr_cv_wait(&dbase->flags_cond, &dbase->versionLock);
#else
	DBRELE(dbase);
	LWP_WaitProcess(&dbase->dbFlags);
	DBHOLD(dbase);
#endif
	ViceLog(125, ("ubik: database flags changed; current flags: 0x%x\n",
		    dbase->dbFlags));
    }
    return 0;
}

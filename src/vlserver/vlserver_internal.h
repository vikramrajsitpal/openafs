/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#ifndef _VLSERVER_INTERNAL_H
#define _VLSERVER_INTERNAL_H

struct vl_cache {
    int vldbversion;
    int maxnservers;

    struct vlheader cheader;
    afs_uint32 hostaddress[MAXSERVERID+1];
    struct extentaddr *ex_addr[VL_MAX_ADDREXTBLKS];
};

/**
 * context for a transaction of a single VL operation.
 */
struct vl_ctx {
    struct ubik_trans *trans;
    struct vl_cache *cache;
    int builddb;
};

/* vlprocs.c */
extern int Init_VLdbase(struct vl_ctx *ctx, int locktype, int this_op);
extern int vl_EndTrans(struct vl_ctx *ctx);

/* vlutils.c */
extern afs_int32 vlwrite_cheader(struct vl_ctx *ctx,
				 struct vlheader *cheader, void *buffer,
				 afs_int32 length);
extern afs_int32 vlwrite_exblock(struct vl_ctx *ctx, afs_int32 base,
				 struct extentaddr *exblock,
				 afs_int32 exblock_addr, void *buffer,
				 afs_int32 length);
extern afs_int32 vlentrywrite(struct vl_ctx *ctx, afs_int32 offset,
			      struct nvlentry *nep);
extern int write_vital_vlheader(struct vl_ctx *ctx);
extern afs_int32 readExtents(struct vl_ctx *ctx);
extern afs_int32 CheckInit(struct vl_ctx *ctx, int builddb, int locktype);
extern afs_int32 AllocBlock(struct vl_ctx *ctx,
			    struct nvlentry *tentry);
extern afs_int32 FindExtentBlock(struct vl_ctx *ctx, afsUUID *uuidp,
				 afs_int32 createit, afs_int32 hostslot,
				 struct extentaddr **expp, afs_int32 *basep);
extern afs_int32 FindByID(struct vl_ctx *ctx, afs_uint32 volid,
		          afs_int32 voltype, struct nvlentry *tentry,
			  afs_int32 *error);
extern afs_int32 FindByName(struct vl_ctx *ctx, char *volname,
			    struct nvlentry *tentry, afs_int32 *error);
extern int EntryIDExists(struct vl_ctx *ctx, const afs_uint32 *ids,
			 afs_int32 ids_len, afs_int32 *error);
extern afs_uint32 NextUnusedID(struct vl_ctx *ctx, afs_uint32 maxvolid,
			       afs_uint32 bump, afs_int32 *error);
extern int HashNDump(struct vl_ctx *ctx, int hashindex);
extern int HashIdDump(struct vl_ctx *ctx, int hashindex);
extern int ThreadVLentry(struct vl_ctx *ctx, afs_int32 blockindex,
                         struct nvlentry *tentry);
extern int UnthreadVLentry(struct vl_ctx *ctx, afs_int32 blockindex,
			 struct nvlentry *aentry);
extern int HashVolid(struct vl_ctx *ctx, afs_int32 voltype,
		     afs_int32 blockindex, struct nvlentry *tentry);
extern int UnhashVolid(struct vl_ctx *ctx, afs_int32 voltype,
		       afs_int32 blockindex, struct nvlentry *aentry);
extern int HashVolname(struct vl_ctx *ctx, afs_int32 blockindex,
		       struct nvlentry *aentry);
extern int UnhashVolname(struct vl_ctx *ctx, afs_int32 blockindex,
			 struct nvlentry *aentry);
extern afs_int32 NextEntry(struct vl_ctx *ctx, afs_int32 blockindex,
			   struct nvlentry *tentry, afs_int32 *remaining);
extern int FreeBlock(struct vl_ctx *ctx, afs_int32 blockindex);
extern int vlsynccache(void);
extern int vl_checkdb(struct ubik_trans *trans);
#endif

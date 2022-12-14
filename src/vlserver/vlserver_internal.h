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

#include "vlserver.h"

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

    /* Skip writing the cheader on disk. */
    int cheader_nowrite;

    /* When hashing a vlentry, assume it doesn't already exist (skip checking
     * for collisions). */
    int hash_nocollide;
};

/* vlprocs.c */
extern int Init_VLdbase(struct vl_ctx *ctx, int locktype, int this_op);
extern int vl_EndTrans(struct vl_ctx *ctx);

/* vlutils.c */

/* vldb4-kv key for the cheader */
#define VL4KV_KEY_CHEADERKV 0x04686472 /* 04 + "hdr" */

/* vldb4-kv key tag for ex blocks */
#define VL4KV_KEY_EXBLOCK   0x04455862 /* 04 + "EXb" */

/* vldb4-kv key tag for volume ids */
#define VL4KV_KEY_VOLID	    0x04564944 /* 04 + "VID" */

/* vldb4-kv key tab for volume names */
#define VL4KV_KEY_VOLNAME   0x046E616D /* 04 + "nam" */

/* vldb4-kv key for an ex block with the given 'base' */
struct vl4kv_exkey {
    afs_uint32 tag;
    afs_int32 base;
};

/* vldb4-kv key for volume id 'volid' */
struct vl4kv_volidkey {
    afs_uint32 tag;
    afs_uint32 volid;
};

/*
 * vldb4-kv key for volume name 'name'. Note that we don't store the entire
 * struct in the db; if 'name' is only 5 bytes long, we set our key to only be
 * 'sizeof(tag) + 5' bytes.
 */
struct vl4kv_volnamekey {
    afs_uint32 tag;
    char name[VL_MAXNAMELEN];
};

/*
 * The cheader equivalent for vldb4-kv. This is the same as struct vlheader,
 * but it doesn't have the VolnameHash/VolidHash fields (since those aren't
 * used in vldb4-kv).
 */
struct vlheader_kv {
    struct vital_vlheader vital_header;
    afs_uint32 IpMappedAddr[MAXSERVERID + 1];
    afs_int32 SIT;
};

extern afs_int32 vlread_cheader(struct vl_ctx *ctx, struct vlheader *cheader);
extern afs_int32 vlread_exblock(struct vl_ctx *ctx, afs_int32 base,
				afs_int32 offset, struct extentaddr *exblock);
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
extern afs_int32 vlgrow_eofPtr(struct vlheader *cheader, afs_int32 bump,
			       afs_int32 *a_blockindex);
extern afs_int32 readExtents(struct vl_ctx *ctx);
extern int vlexcpy(struct extentaddr **dst_ex, struct extentaddr **src_ex);
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

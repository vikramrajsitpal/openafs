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

#include <afs/opr.h>
#include <lock.h>
#include <rx/xdr.h>
#include <ubik_np.h>

#include "vlserver.h"
#include "vlserver_internal.h"

static int index_OK(struct vl_ctx *ctx, afs_int32 blockindex);

#define ERROR_EXIT(code) do { \
    error = (code); \
    goto error_exit; \
} while (0)

/* vl_cache to use for read transactions */
static struct vl_cache rd_vlcache;

/* vl_cache to use for the current active write transaction. */
static struct vl_cache wr_vlcache;

/*
 * This file implements two variants of the VLDB4 database format:
 *
 * - vldb4: the traditional flat-file db (the kind used in e.g. OpenAFS 1.8)
 * - vldb4-kv: a newer variant using the ubik key-value store ("KV").
 *
 * The flat-file variant uses ubik_Read and ubik_Write to store data in a flat
 * file, a stream of bytes. In vldb4, we store volume entries in our own hash
 * table that is stored in the root cheader, and entries are linked together by
 * addresses in the vlentry structs themselves. MH data is stored at the
 * address contained in the SIT field in the root cheader.
 *
 * In vldb4-kv, we never use ubik_Read/ubik_Write. Instead, we store everything
 * in the database as key/value pairs. To distinguish between different types
 * of data, each of our keys is prefixed with a 4-byte NBO "tag":
 *
 * - Our root 'struct vlheader_kv' (aka cheader_kv) is stored via the key
 * VL4KV_KEY_CHEADERKV. This contains all of the data in the normal vlheader
 * (aka cheader), except for the hash tables.
 *
 * - Our vlentries are stored by the key vl4kv_volidkey with the 'tag' set to
 * VL4KV_KEY_VOLID, and 'volid' set to the RW volid for the volume. The value
 * of these is the 'struct nvlentry' for that volume.
 *
 * - Additional keys for each vlentry are also stored for each non-RW volid for
 * the volume, in order to lookup the volume by e.g. RO id. The key for these
 * is a vl4kv_volidkey with the 'tag' set to VL4KV_KEY_VOLID, and 'volid' set
 * to the volume id. The value for these is the RW id for the volume (just the
 * 4-byte int in NBO); the actual nvlentry for it can then be looked up by that
 * RW id.
 *
 * - An additional key for each volume is also stored for the volume name, so
 * the volume can be looked up by name. The key is vl4kv_volnamekey with 'tag'
 * set to VL4KV_KEY_VOLNAME, and 'name' filled in with the volume name. The
 * size of the key depends on the length of the given name; we don't (usually)
 * store the entire struct vl4kv_volnamekey. That is, if the given name is only
 * 5 bytes long, our key is only of size 'sizeof(tag) + 5' bytes (so, 9 bytes).
 *
 * - MH data is stored by the key vl4kv_exkey, with 'tag' set to
 * VL4KV_KEY_EXBLOCK, and 'base' set to the MH block base number.
 */

/*
 * Various pieces of code use a 'blockindex' (that is, an offset in our flat
 * file) to say where to store data to or read data from. For KV, we of course
 * don't use actual file offsets, but we need to have _some_ value to pass
 * around to keep the code paths similar. Just pick an arbitrary number here;
 * it just needs to be nonzero, since some functions return an offset of 0 to
 * represent failure.
 */
#define VL4KV_FAKE_BLOCKINDEX (1)

static_inline int
vlctx_kv(struct vl_ctx *ctx)
{
    return ubik_KVTrans(ctx->trans);
}

/* Convert a traditional cheader to the "kv" cheader. */
static_inline void
cheader2kv(struct vlheader *cheader, struct vlheader_kv *cheader_kv)
{
    opr_StaticAssert(sizeof(cheader->IpMappedAddr) == sizeof(cheader_kv->IpMappedAddr));

    memset(cheader_kv, 0, sizeof(*cheader_kv));

    cheader_kv->vital_header = cheader->vital_header;
    cheader_kv->vital_header.headersize = htonl(sizeof(*cheader_kv));
    memcpy(cheader_kv->IpMappedAddr, cheader->IpMappedAddr,
	   sizeof(cheader->IpMappedAddr));
    cheader_kv->SIT = cheader->SIT;
}

/* Convert a "kv" cheader to a traditional cheader. */
static_inline void
kv2cheader(struct vlheader_kv *cheader_kv, struct vlheader *cheader)
{
    opr_StaticAssert(sizeof(cheader->IpMappedAddr) == sizeof(cheader_kv->IpMappedAddr));

    memset(cheader, 0, sizeof(*cheader));

    cheader->vital_header = cheader_kv->vital_header;
    memcpy(cheader->IpMappedAddr, cheader_kv->IpMappedAddr,
	   sizeof(cheader->IpMappedAddr));
    cheader->SIT = cheader_kv->SIT;
}

/* Set 'buf' to point at 'val' with length 'len'. */
static_inline void
opaque_set(struct rx_opaque *buf, void *val, size_t len)
{
    buf->len = len;
    buf->val = val;
}

/* Copy a fixed-size object out of 'buf' into 'dest', of size 'destsize'. */
static_inline void
opaque_copy(struct rx_opaque *buf, void *dest, size_t destsize)
{
    opr_Assert(buf->val != NULL);
    opr_Assert(buf->len >= destsize);
    memcpy(dest, buf->val, destsize);
}

static void
init_exkey(struct rx_opaque *keybuf, struct vl4kv_exkey *exkey,
	   afs_int32 base)
{
    memset(exkey, 0, sizeof(*exkey));
    exkey->tag = htonl(VL4KV_KEY_EXBLOCK);
    exkey->base = htonl(base);

    opaque_set(keybuf, exkey, sizeof(*exkey));
}

static void
init_volidkey(struct rx_opaque *keybuf, struct vl4kv_volidkey *ikey,
	      afs_uint32 volid)
{
    memset(ikey, 0, sizeof(*ikey));
    ikey->tag = htonl(VL4KV_KEY_VOLID);
    ikey->volid = htonl(volid);

    opaque_set(keybuf, ikey, sizeof(*ikey));
}

static void
init_volnamekey(struct rx_opaque *keybuf, struct vl4kv_volnamekey *nkey,
		char *volname)
{
    size_t len = strlen(volname);

    opr_Assert(len <= sizeof(nkey->name));

    memset(nkey, 0, sizeof(*nkey));
    nkey->tag = htonl(VL4KV_KEY_VOLNAME);
    memcpy(nkey->name, volname, len);

    opaque_set(keybuf, nkey, sizeof(nkey->tag) + len);
}

/* Hashing algorithm based on the volume id; HASHSIZE must be prime */
afs_int32
IDHash(afs_int32 volumeid)
{
    return ((abs(volumeid)) % HASHSIZE);
}


/* Hashing algorithm based on the volume name; name's size is implicit (64 chars) and if changed it should be reflected here. */
afs_int32
NameHash(char *volumename)
{
    unsigned int hash;
    int i;

    hash = 0;
    for (i = strlen(volumename), volumename += i - 1; i--; volumename--)
	hash = (hash * 63) + (*((unsigned char *)volumename) - 63);
    return (hash % HASHSIZE);
}


/* package up seek and write into one procedure for ease of use */
static afs_int32
vlwrite(struct vl_ctx *ctx, afs_int32 offset, void *buffer,
	afs_int32 length)
{
    afs_int32 errorcode;

    if ((errorcode = ubik_Seek(ctx->trans, 0, offset)))
	return errorcode;
    return (ubik_Write(ctx->trans, buffer, length));
}

/* Write a portion of the cheader to disk. Write 'length' bytes of 'buffer',
 * which must point to somewhere inside 'cheader'. */
afs_int32
vlwrite_cheader(struct vl_ctx *ctx, struct vlheader *cheader,
		void *buffer, afs_int32 length)
{
    struct vlheader_kv cheader_kv;
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;
    afs_uint32 ckey;
    afs_int32 offset = DOFFSET(0, cheader, buffer);

    opr_Assert(offset >= 0 && offset <= sizeof(*cheader));
    opr_Assert(offset + length <= sizeof(*cheader));

    if (!vlctx_kv(ctx)) {
	return vlwrite(ctx, offset, buffer, length);
    }

    /* For KV, we just replace the entire (abbreviated) cheader. */
    cheader2kv(cheader, &cheader_kv);

    ckey = htonl(VL4KV_KEY_CHEADERKV);
    opaque_set(&keybuf, &ckey, sizeof(ckey));
    opaque_set(&valbuf, &cheader_kv, sizeof(cheader_kv));

    return ubik_KVReplace(ctx->trans, &keybuf, &valbuf);
}

/*
 * Write a portion of an extent block to disk. Write 'length' bytes of
 * 'buffer', which must point somewhere inside 'exblock'. The given extent
 * block is base 'base', and exists at database file offset 'exblock_addr'.
 */
afs_int32
vlwrite_exblock(struct vl_ctx *ctx, afs_int32 base,
		struct extentaddr *exblock, afs_int32 exblock_addr,
		void *buffer, afs_int32 length)
{
    struct vl4kv_exkey exkey;
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;
    afs_int32 offset = DOFFSET(0, exblock, buffer);

    opr_Assert(offset >= 0 && offset <= VL_ADDREXTBLK_SIZE);
    opr_Assert(offset + length <= VL_ADDREXTBLK_SIZE);

    if (!vlctx_kv(ctx)) {
	return vlwrite(ctx, exblock_addr + offset, buffer, length);
    }

    /* For KV, we store each MH block as its own item. So just write out the
     * entire block. */

    init_exkey(&keybuf, &exkey, base);
    opaque_set(&valbuf, exblock, VL_ADDREXTBLK_SIZE);

    return ubik_KVReplace(ctx->trans, &keybuf, &valbuf);
}

/* Package up seek and read into one procedure for ease of use */
static afs_int32
vlread(struct vl_ctx *ctx, afs_int32 offset, void *buffer,
       afs_int32 length)
{
    afs_int32 errorcode;

    if ((errorcode = ubik_Seek(ctx->trans, 0, offset)))
	return errorcode;
    return (ubik_Read(ctx->trans, buffer, length));
}

/* Read in the cheader from disk. */
static afs_int32
vlread_cheader(struct vl_ctx *ctx, struct vlheader *cheader)
{
    afs_uint32 ckey = htonl(VL4KV_KEY_CHEADERKV);
    struct vlheader_kv cheader_kv;
    struct rx_opaque keybuf;
    afs_int32 code;

    if (!vlctx_kv(ctx)) {
	return vlread(ctx, 0, cheader, sizeof(*cheader));
    }

    /*
     * We don't need the hash tables for KV, so in the KV store we have an
     * abbreviated cheader; it contains everything in the normal cheader
     * except for the hash tables. After we read in the "kv cheader", we
     * convert to the "normal" cheader below via kv2cheader.
     */

    opaque_set(&keybuf, &ckey, sizeof(ckey));
    code = ubik_KVGetCopy(ctx->trans, &keybuf, &cheader_kv,
			  sizeof(cheader_kv), NULL);
    if (code != 0) {
	return code;
    }

    kv2cheader(&cheader_kv, cheader);
    return 0;
}

/* Read in an extent block from disk, for base 'base' at database file offset
 * 'offset'. */
static afs_int32
vlread_exblock(struct vl_ctx *ctx, afs_int32 base, afs_int32 offset,
	       struct extentaddr *exblock)
{
    struct vl4kv_exkey exkey;
    struct rx_opaque keybuf;

    if (!vlctx_kv(ctx)) {
	return vlread(ctx, offset, exblock, VL_ADDREXTBLK_SIZE);
    }

    init_exkey(&keybuf, &exkey, base);

    return ubik_KVGetCopy(ctx->trans, &keybuf, exblock, VL_ADDREXTBLK_SIZE, NULL);
}

static void
nvlentry_htonl(struct nvlentry *src, struct nvlentry *dest)
{
    afs_int32 i;
    for (i = 0; i < MAXTYPES; i++)
	dest->volumeId[i] = htonl(src->volumeId[i]);
    dest->flags = htonl(src->flags);
    dest->LockAfsId = htonl(src->LockAfsId);
    dest->LockTimestamp = htonl(src->LockTimestamp);
    dest->cloneId = htonl(src->cloneId);
    for (i = 0; i < MAXTYPES; i++)
	dest->nextIdHash[i] = htonl(src->nextIdHash[i]);
    dest->nextNameHash = htonl(src->nextNameHash);
    memcpy(dest->name, src->name, VL_MAXNAMELEN);
    memcpy(dest->serverNumber, src->serverNumber, NMAXNSERVERS);
    memcpy(dest->serverPartition, src->serverPartition, NMAXNSERVERS);
    memcpy(dest->serverFlags, src->serverFlags, NMAXNSERVERS);
}

static void
nvlentry_ntohl(struct nvlentry *src, struct nvlentry *dest)
{
    /* ntohl() and htonl() are the same, so just call htonl() to do this. */
    nvlentry_htonl(src, dest);
}

/*
 * Make the given volid or volname key point to the given RW volid. If the key
 * already exists, verify that it is pointing to the correct RW volid. If it's
 * not, throw an error.
 */
static int
kv_hashvolkey(struct vl_ctx *ctx, struct rx_opaque *key, afs_uint32 rwid)
{
    int code;
    int noent = 0;
    afs_uint32 volid;
    struct rx_opaque valbuf;

    if (rwid == 0) {
	/* Sanity check. */
	VLog(0, ("Error: tried to hash RW volid 0.\n"));
	return VL_IO;
    }

    /*
     * For looking up volumes by name or non-RW volid, we store a mapping where
     * the key is the normal volid/volname key, but the value is just the RW
     * volid (in net-order). Whoever looks up the key can then use that RW
     * volid to find the actual volume entry.
     */

    /* First, lookup the name in the KV store, to see if an entry for it
     * already exists. */
    code = ubik_KVGetCopy(ctx->trans, key, &volid, sizeof(volid), &noent);
    if (code != 0) {
	return code;
    }

    if (noent) {
	/* An entry for this key doesn't exist; add it now. */
	rwid = htonl(rwid);
	opaque_set(&valbuf, &rwid, sizeof(rwid));
	return ubik_KVPut(ctx->trans, key, &valbuf);
    }

    /* This key already exists; see if it's pointing to the correct RW id. */
    volid = ntohl(volid);
    if (volid == rwid) {
	/* The existing entry is already pointing to the correct RW id;
	 * nothing more to do. */
	return 0;
    }

    /* The existing entry is pointing to some other RW id. That shouldn't
     * happen. */
    VLog(0, ("Error: Tried to hash RW id %u, but entry already exists "
	     "pointing to volid %u\n", rwid, volid));
    return VL_DBBAD;
}

static int
kv_HashVolid(struct vl_ctx *ctx, afs_int32 voltype, struct nvlentry *tentry)
{
    afs_uint32 volid;
    struct vl4kv_volidkey ikey;
    struct rx_opaque keybuf;

    if (voltype == RWVOL) {
	/* Don't separately hash the RW id; we store the entry itself under the
	 * RW id. */
	return 0;
    }

    volid = tentry->volumeId[voltype];
    if (volid == 0) {
	/* This vlentry doesn't have a volid for this type; nothing to do. */
	return 0;
    }

    init_volidkey(&keybuf, &ikey, volid);
    return kv_hashvolkey(ctx, &keybuf, tentry->volumeId[RWVOL]);
}

static int
kv_HashVolname(struct vl_ctx *ctx, struct nvlentry *aentry)
{
    struct vl4kv_volnamekey nkey;
    struct rx_opaque keybuf;

    opr_StaticAssert(sizeof(nkey.name) == sizeof(aentry->name));

    init_volnamekey(&keybuf, &nkey, aentry->name);
    return kv_hashvolkey(ctx, &keybuf, aentry->volumeId[RWVOL]);
}

/* vldb4-kv: Store a vlentry into the db. */
static afs_int32
kv_vlentryput(struct vl_ctx *ctx, struct nvlentry *tentry,
	      struct nvlentry *spare_entry)
{
    afs_uint32 rwid;
    afs_int32 voltype;
    afs_int32 code;
    struct vl4kv_volidkey ikey;
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;

    /* When writing out the vlentry, we also need to hash it by its volids and
     * volname, in case any of those items have changed. */

    for (voltype = ROVOL; voltype <= BACKVOL; voltype++) {
	code = kv_HashVolid(ctx, voltype, tentry);
	if (code != 0) {
	    return code;
	}
    }

    code = kv_HashVolname(ctx, tentry);
    if (code != 0) {
	return code;
    }

    /* Now we can store the vlentry itself; store it under the volid key for
     * the RW volid. */

    rwid = tentry->volumeId[RWVOL];
    if (rwid == 0) {
	return VL_IO;
    }

    init_volidkey(&keybuf, &ikey, rwid);

    nvlentry_htonl(tentry, spare_entry);
    opaque_set(&valbuf, spare_entry, sizeof(*spare_entry));

    return ubik_KVReplace(ctx->trans, &keybuf, &valbuf);
}

/* take entry and convert to network order and write to disk */
afs_int32
vlentrywrite(struct vl_ctx *ctx, afs_int32 offset, struct nvlentry *nep)
{
    struct vl_cache *cache = ctx->cache;
    struct vlentry oentry;
    struct nvlentry nentry;
    void *bufp;
    afs_int32 i;

    opr_StaticAssert(sizeof(oentry) == sizeof(nentry));

    if (vlctx_kv(ctx)) {
	return kv_vlentryput(ctx, nep, &nentry);
    }

    if (cache->maxnservers == 13) {
	nvlentry_htonl(nep, &nentry);
	bufp = &nentry;
    } else {
	memset(&oentry, 0, sizeof(struct vlentry));
	for (i = 0; i < MAXTYPES; i++)
	    oentry.volumeId[i] = htonl(nep->volumeId[i]);
	oentry.flags = htonl(nep->flags);
	oentry.LockAfsId = htonl(nep->LockAfsId);
	oentry.LockTimestamp = htonl(nep->LockTimestamp);
	oentry.cloneId = htonl(nep->cloneId);
	for (i = 0; i < MAXTYPES; i++)
	    oentry.nextIdHash[i] = htonl(nep->nextIdHash[i]);
	oentry.nextNameHash = htonl(nep->nextNameHash);
	memcpy(oentry.name, nep->name, VL_MAXNAMELEN);
	memcpy(oentry.serverNumber, nep->serverNumber, OMAXNSERVERS);
	memcpy(oentry.serverPartition, nep->serverPartition, OMAXNSERVERS);
	memcpy(oentry.serverFlags, nep->serverFlags, OMAXNSERVERS);
	bufp = &oentry;
    }
    return vlwrite(ctx, offset, bufp, sizeof(nentry));
}

/* read entry from disk and convert to host order */
static afs_int32
vlentryread(struct vl_ctx *ctx, afs_int32 offset, struct nvlentry *nbufp)
{
    struct vl_cache *cache = ctx->cache;
    struct vlentry *oep, tentry;
    struct nvlentry *nep;
    void *bufp = &tentry;
    afs_int32 i;

    opr_StaticAssert(sizeof(*oep) == sizeof(*nep));

    i = vlread(ctx, offset, bufp, sizeof(tentry));
    if (i)
	return i;
    if (cache->maxnservers == 13) {
	nep = bufp;
	nvlentry_ntohl(nep, nbufp);
    } else {
	oep = bufp;
	memset(nbufp, 0, sizeof(struct nvlentry));
	for (i = 0; i < MAXTYPES; i++)
	    nbufp->volumeId[i] = ntohl(oep->volumeId[i]);
	nbufp->flags = ntohl(oep->flags);
	nbufp->LockAfsId = ntohl(oep->LockAfsId);
	nbufp->LockTimestamp = ntohl(oep->LockTimestamp);
	nbufp->cloneId = ntohl(oep->cloneId);
	for (i = 0; i < MAXTYPES; i++)
	    nbufp->nextIdHash[i] = ntohl(oep->nextIdHash[i]);
	nbufp->nextNameHash = ntohl(oep->nextNameHash);
	memcpy(nbufp->name, oep->name, VL_MAXNAMELEN);
	memcpy(nbufp->serverNumber, oep->serverNumber, OMAXNSERVERS);
	memcpy(nbufp->serverPartition, oep->serverPartition, OMAXNSERVERS);
	memcpy(nbufp->serverFlags, oep->serverFlags, OMAXNSERVERS);
	/* initilize the last elements to BADSERVERID */
	for (i = OMAXNSERVERS; i < NMAXNSERVERS; i++) {
	    nbufp->serverNumber[i] = BADSERVERID;
	    nbufp->serverPartition[i] = BADSERVERID;
	    nbufp->serverFlags[i] = BADSERVERID;
	}
    }
    return 0;
}

/* Convenient write of small critical vldb header info to the database. */
int
write_vital_vlheader(struct vl_ctx *ctx)
{
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;
    afs_int32 code;

    code = vlwrite_cheader(ctx, cheader, &cheader->vital_header,
			   sizeof(vital_vlheader));
    if (code != 0) {
	return VL_IO;
    }
    return 0;
}

/* vldb4-kv: Fetch a vlentry from the db by the given key (either a volid or
 * volname key). */
static afs_int32
kv_vlentryget(struct vl_ctx *ctx, struct rx_opaque *key,
	      struct nvlentry *aentry)
{
    struct vl_cache *cache = ctx->cache;
    int noent = 0;
    afs_int32 code;
    afs_uint32 volid;
    struct rx_opaque valbuf;

    opr_Assert(vlctx_kv(ctx));
    opr_Assert(cache->maxnservers == 13);

    memset(&valbuf, 0, sizeof(valbuf));

    code = ubik_KVGet(ctx->trans, key, &valbuf, &noent);
    if (code != 0) {
	return code;
    }
    if (noent) {
	return VL_NOENT;
    }

    if (valbuf.len == sizeof(volid)) {
	/*
	 * The returned value isn't the volume entry; it's the RW volid for the
	 * volume. We now look up the volume by this volid to get the real
	 * entry.
	 */
	struct vl4kv_volidkey ikey;
	struct rx_opaque keybuf;

	opaque_copy(&valbuf, &volid, sizeof(volid));
	volid = ntohl(volid);

	init_volidkey(&keybuf, &ikey, volid);

	code = ubik_KVGet(ctx->trans, &keybuf, &valbuf, NULL);
	if (code != 0) {
	    return code;
	}
    }
    if (valbuf.len != sizeof(*aentry)) {
	VLog(0, ("Error: Invalid vlentry size in kv store: %d != %d.\n",
		 (int)valbuf.len, (int)sizeof(*aentry)));
	return VL_IO;
    }

    {
	struct nvlentry tentry;
	memset(&tentry, 0, sizeof(tentry));
	opaque_copy(&valbuf, &tentry, sizeof(tentry));
	nvlentry_ntohl(&tentry, aentry);
    }

    return 0;
}

int extent_mod = 0;

/* This routine reads in the extent blocks for multi-homed servers.
 * There used to be an initialization bug that would cause the contaddrs
 * pointers in the first extent block to be bad. Here we will check the
 * pointers and zero them in the in-memory copy if we find them bad. We
 * also try to write the extent blocks back out. If we can't, then we
 * will wait until the next write transaction to write them out
 * (extent_mod tells us the on-disk copy is bad).
 */
afs_int32
readExtents(struct vl_ctx *ctx)
{
    struct vl_cache *cache = ctx->cache;
    afs_uint32 extentAddr;
    afs_int32 error = 0, code;
    int i;

    struct vlheader *cheader = &cache->cheader;
    struct extentaddr **ex_addr = cache->ex_addr;

    extent_mod = 0;
    extentAddr = ntohl(cheader->SIT);
    if (!extentAddr)
	return 0;

    /* Read the first extension block */
    if (!ex_addr[0]) {
	ex_addr[0] = malloc(VL_ADDREXTBLK_SIZE);
	if (!ex_addr[0])
	    ERROR_EXIT(VL_NOMEM);
    }
    code = vlread_exblock(ctx, 0, extentAddr, ex_addr[0]);
    if (code) {
	free(ex_addr[0]);	/* Not the place to create it */
	ex_addr[0] = 0;
	ERROR_EXIT(VL_IO);
    }

    /* In case more that 64 mh servers are in use they're kept in these
     * continuation blocks
     */
    for (i = 1; i < VL_MAX_ADDREXTBLKS; i++) {
	if (!ex_addr[0]->ex_contaddrs[i])
	    continue;

	/* Before reading it in, check to see if the address is good */
	if ((ntohl(ex_addr[0]->ex_contaddrs[i]) <
	     ntohl(ex_addr[0]->ex_contaddrs[i - 1]) + VL_ADDREXTBLK_SIZE)
	    || (ntohl(ex_addr[0]->ex_contaddrs[i]) >
		ntohl(cheader->vital_header.eofPtr) - VL_ADDREXTBLK_SIZE)) {
	    extent_mod = 1;
	    ex_addr[0]->ex_contaddrs[i] = 0;
	    continue;
	}


	/* Read the continuation block */
	if (!ex_addr[i]) {
	    ex_addr[i] = malloc(VL_ADDREXTBLK_SIZE);
	    if (!ex_addr[i])
		ERROR_EXIT(VL_NOMEM);
	}
	code = vlread_exblock(ctx, i, ntohl(ex_addr[0]->ex_contaddrs[i]),
			      ex_addr[i]);
	if (code) {
	    free(ex_addr[i]);	/* Not the place to create it */
	    ex_addr[i] = 0;
	    ERROR_EXIT(VL_IO);
	}

	/* After reading it in, check to see if its a real continuation block */
	if (ntohl(ex_addr[i]->ex_hdrflags) != VLCONTBLOCK) {
	    extent_mod = 1;
	    ex_addr[0]->ex_contaddrs[i] = 0;
	    free(ex_addr[i]);	/* Not the place to create it */
	    ex_addr[i] = 0;
	    continue;
	}
    }

    if (extent_mod) {
	code = vlwrite_exblock(ctx, 0, ex_addr[0], extentAddr, ex_addr[0],
			       VL_ADDREXTBLK_SIZE);
	if (!code) {
	    VLog(0, ("Multihome server support modification\n"));
	}
	/* Keep extent_mod true in-case the transaction aborts */
	/* Don't return error so we don't abort transaction */
    }

  error_exit:
    return error;
}

/* Check that the database has been initialized.  Be careful to fail in a safe
   manner, to avoid bogusly reinitializing the db.  */
/**
 * reads in db cache from ubik.
 *
 * @param[in] ut ubik transaction
 * @param[in] rock  opaque pointer to our struct vl_ctx
 *
 * @return operation status
 *   @retval 0 success
 */
static afs_int32
UpdateCache(struct ubik_trans *trans, void *rock)
{
    struct vl_ctx *ctx = rock;
    int builddb = ctx->builddb;
    struct vl_cache *cache;
    afs_int32 error = 0, i, code, ubcode;
    int force_cache = 0;
    struct vlheader *cheader;
    afs_uint32 *hostaddress;

    if (ctx->cache == NULL) {
	ctx->cache = &rd_vlcache;
    }
    cache = ctx->cache;

    cheader = &cache->cheader;
    hostaddress = cache->hostaddress;

    /* if version changed (or first call), read the header */
    ubcode = vlread_cheader(ctx, cheader);
    cache->vldbversion = ntohl(cheader->vital_header.vldbversion);

    if (!ubcode && (cache->vldbversion != 0)) {
	memcpy(hostaddress, cheader->IpMappedAddr, sizeof(cheader->IpMappedAddr));
	for (i = 0; i < MAXSERVERID + 1; i++) {	/* cvt HostAddress to host order */
	    hostaddress[i] = ntohl(hostaddress[i]);
	}

	code = readExtents(ctx);
	if (code)
	    ERROR_EXIT(code);
    }

    /* now, if can't read, or header is wrong, write a new header */
    if (ubcode || cache->vldbversion == 0) {
	if (builddb) {
	    afs_uint32 version = VLDBVERSION_3;
	    if (ubik_KVTrans(trans)) {
		version = VLDBVERSION_4_KV;
	    }

	    VLog(0, ("Can't read VLDB header, re-initialising...\n"));

	    ctx->cache = cache = &wr_vlcache;
	    cheader = &cache->cheader;
	    hostaddress = cache->hostaddress;

	    /*
	     * Don't reset ctx->cache; we need to make sure this cache is the
	     * one actually used, since we're populating it from scratch.
	     * Without this, the caller will copy rd_vlcache into wr_vlcache,
	     * overwriting our changes here.
	     */
	    force_cache = 1;

	    /* try to write a good header */
	    /* The read cache will be sync'ed to this new header
	     * when the ubik transaction is ended by vlsynccache(). */
	    memset(cheader, 0, sizeof(*cheader));
	    cheader->vital_header.vldbversion = htonl(version);
	    cheader->vital_header.headersize = htonl(sizeof(*cheader));
	    /* DANGER: Must get this from a master place!! */
	    cheader->vital_header.MaxVolumeId = htonl(0x20000000);
	    cheader->vital_header.eofPtr = htonl(sizeof(*cheader));
	    for (i = 0; i < MAXSERVERID + 1; i++) {
		cheader->IpMappedAddr[i] = 0;
		hostaddress[i] = 0;
	    }
	    code = vlwrite_cheader(ctx, cheader, cheader, sizeof(*cheader));
	    if (code) {
		VLog(0, ("Can't write VLDB header (error = %d)\n", code));
		ERROR_EXIT(VL_IO);
	    }
	    cache->vldbversion = ntohl(cheader->vital_header.vldbversion);
	} else {
	    VLog(1, ("Unable to read VLDB header.\n"));
	    ERROR_EXIT(VL_EMPTY);
	}
    }

    if (ubik_KVTrans(trans)) {
	if (cache->vldbversion != VLDBVERSION_4_KV) {
	    VLog(0, ("Invalid VLDB version 0x%x (doesn't match 0x%x), quitting!\n",
		     cache->vldbversion, VLDBVERSION_4_KV));
	    ERROR_EXIT(VL_BADVERSION);
	}
    } else {
	if ((cache->vldbversion != VLDBVERSION_3)
	    && (cache->vldbversion != VLDBVERSION_2)
	    && (cache->vldbversion != VLDBVERSION_4)) {
	    VLog(0,
		("VLDB version %d doesn't match this software version(%d, %d or %d), quitting!\n",
		 cache->vldbversion, VLDBVERSION_4, VLDBVERSION_3, VLDBVERSION_2));
	    ERROR_EXIT(VL_BADVERSION);
	}
    }

    if (cache->vldbversion == VLDBVERSION_3 || cache->vldbversion == VLDBVERSION_4
	 || cache->vldbversion == VLDBVERSION_4_KV) {
	cache->maxnservers = 13;
    } else {
	cache->maxnservers = 8;
    }

    if (!force_cache) {
	/* Let our caller calculate the proper cache to use. */
	ctx->cache = NULL;
    }

  error_exit:
    /* all done */
    return error;
}

/* makes a deep copy of src_ex into dst_ex */
static int
vlexcpy(struct extentaddr **dst_ex, struct extentaddr **src_ex)
{
    int i;
    for (i = 0; i < VL_MAX_ADDREXTBLKS; i++) {
	if (src_ex[i]) {
	    if (!dst_ex[i]) {
		dst_ex[i] = malloc(VL_ADDREXTBLK_SIZE);
	    }
	    if (!dst_ex[i]) {
		return VL_NOMEM;
	    }
	    memcpy(dst_ex[i], src_ex[i], VL_ADDREXTBLK_SIZE);

	} else if (dst_ex[i]) {
	    /* we have no src, but we have a dst... meaning, this block
	     * has gone away */
	    free(dst_ex[i]);
	    dst_ex[i] = NULL;
	}
    }
    return 0;
}

static int
vlcache_copy(struct vl_cache *dest, struct vl_cache *src)
{
    struct extentaddr *save_exaddr[4] = {
	dest->ex_addr[0],
	dest->ex_addr[1],
	dest->ex_addr[2],
	dest->ex_addr[3],
    };
    /*
     * Everything in struct vl_cache can be a simple shallow copy, except for
     * the contents of ex_addr. So save a copy of ex_addr, then do a shallow
     * copy of everything, then restore ex_addr and do a deep copy of ex_addr
     * by going through vlexcpy.
     */
    *dest = *src;
    dest->ex_addr[0] = save_exaddr[0];
    dest->ex_addr[1] = save_exaddr[1];
    dest->ex_addr[2] = save_exaddr[2];
    dest->ex_addr[3] = save_exaddr[3];


    return vlexcpy(dest->ex_addr, src->ex_addr);
}

afs_int32
CheckInit(struct vl_ctx *ctx, int builddb, int locktype)
{
    int code;
    struct vl_cache *cache;

    ctx->cache = NULL;
    ctx->builddb = builddb;

    code = ubik_CheckCache(ctx->trans, UpdateCache, ctx);
    if (code != 0) {
	return code;
    }

    /*
     * If ctx->cache is not NULL, then UpdateCache has been run to populate the
     * cache, and it has already picked which cache we should use. Otherwise,
     * we need to pick the cache to use here.
     */
    cache = ctx->cache;
    if (cache == NULL) {
	if (locktype == LOCKREAD) {
	    /* For read transactions, use the read cache. */
	    cache = &rd_vlcache;
	} else {
	    /*
	     * For write transactions, make a copy of the read cache (into
	     * wr_vlcache). This will be copied back into the read cache when
	     * this transaction successfully commits (via vlsynccache).
	     */
	    code = vlcache_copy(&wr_vlcache, &rd_vlcache);
	    if (code != 0) {
		return code;
	    }
	    cache = &wr_vlcache;
	}
    }

    /* these next two cases shouldn't happen (UpdateCache should either
     * rebuild the db or return an error if these cases occur), but just to
     * be on the safe side... */
    if (cache->vldbversion == 0) {
	return VL_EMPTY;
    }
    if (vlctx_kv(ctx)) {
	if (cache->vldbversion != VLDBVERSION_4_KV) {
	    return VL_BADVERSION;
	}
    } else {
	if ((cache->vldbversion != VLDBVERSION_3)
	    && (cache->vldbversion != VLDBVERSION_2)
	    && (cache->vldbversion != VLDBVERSION_4)) {
	    return VL_BADVERSION;
	}
    }

    ctx->cache = cache;
    return 0;
}

/**
 * Grow the eofPtr in the header by 'bump' bytes.
 *
 * @param[inout] cheader    VL header
 * @param[in] bump	    How many bytes to add to eofPtr
 * @param[out] a_blockindex On success, set to the original eofPtr before we
 *			    bumped it
 * @return VL error codes
 */
static afs_int32
grow_eofPtr(struct vlheader *cheader, afs_int32 bump, afs_int32 *a_blockindex)
{
    afs_int32 blockindex = ntohl(cheader->vital_header.eofPtr);

    if (blockindex < 0 || blockindex >= MAX_AFS_INT32 - bump) {
	VLog(0, ("Error: Tried to grow the VLDB beyond the 2GiB limit. Either "
		 "find a way to trim down your VLDB, or upgrade to a release "
		 "and database format that supports a larger VLDB.\n"));
	return VL_IO;
    }

    *a_blockindex = blockindex;
    cheader->vital_header.eofPtr = htonl(blockindex + bump);
    return 0;
}

afs_int32
GetExtentBlock(struct vl_ctx *ctx, afs_int32 base)
{
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;
    struct extentaddr **ex_addr = cache->ex_addr;
    afs_int32 blockindex, code, error = 0;

    /* Base 0 must exist before any other can be created */
    if ((base != 0) && !ex_addr[0])
	ERROR_EXIT(VL_CREATEFAIL);	/* internal error */

    if (!ex_addr[0] || !ex_addr[0]->ex_contaddrs[base]) {
	/* Create a new extension block */
	if (!ex_addr[base]) {
	    ex_addr[base] = malloc(VL_ADDREXTBLK_SIZE);
	    if (!ex_addr[base])
		ERROR_EXIT(VL_NOMEM);
	}
	memset(ex_addr[base], 0, VL_ADDREXTBLK_SIZE);

	/* Write the full extension block at end of vldb */
	ex_addr[base]->ex_hdrflags = htonl(VLCONTBLOCK);
	code = grow_eofPtr(cheader, VL_ADDREXTBLK_SIZE, &blockindex);
	if (code)
	    ERROR_EXIT(VL_IO);

	code = vlwrite_exblock(ctx, base, ex_addr[base], blockindex,
			       ex_addr[base], VL_ADDREXTBLK_SIZE);
	if (code)
	    ERROR_EXIT(VL_IO);

	code = write_vital_vlheader(ctx);
	if (code)
	    ERROR_EXIT(VL_IO);

	/* Write the address of the base extension block in the vldb header */
	if (base == 0) {
	    cheader->SIT = htonl(blockindex);
	    code = vlwrite_cheader(ctx, cheader,
				   &cheader->SIT, sizeof(cheader->SIT));
	    if (code)
		ERROR_EXIT(VL_IO);
	}

	/* Write the address of this extension block into the base extension block */
	ex_addr[0]->ex_contaddrs[base] = htonl(blockindex);
	code = vlwrite_exblock(ctx, 0, ex_addr[0], ntohl(cheader->SIT),
			       ex_addr[0], sizeof(struct extentaddr));
	if (code)
	    ERROR_EXIT(VL_IO);
    }

  error_exit:
    return error;
}


afs_int32
FindExtentBlock(struct vl_ctx *ctx, afsUUID *uuidp,
		afs_int32 createit, afs_int32 hostslot,
		struct extentaddr **expp, afs_int32 *basep)
{
    afsUUID tuuid;
    struct extentaddr *exp;
    afs_int32 i, j, code, base, index, error = 0;

    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;
    afs_uint32 *hostaddress = cache->hostaddress;
    struct extentaddr **ex_addr = cache->ex_addr;

    *expp = NULL;
    *basep = 0;

    /* Create the first extension block if it does not exist */
    if (!cheader->SIT) {
	code = GetExtentBlock(ctx, 0);
	if (code)
	    ERROR_EXIT(code);
    }

    for (i = 0; i < MAXSERVERID + 1; i++) {
	if ((hostaddress[i] & 0xff000000) == 0xff000000) {
	    if ((base = (hostaddress[i] >> 16) & 0xff) > VL_MAX_ADDREXTBLKS) {
		ERROR_EXIT(VL_INDEXERANGE);
	    }
	    if ((index = hostaddress[i] & 0x0000ffff) > VL_MHSRV_PERBLK) {
		ERROR_EXIT(VL_INDEXERANGE);
	    }
	    exp = &ex_addr[base][index];
	    tuuid = exp->ex_hostuuid;
	    afs_ntohuuid(&tuuid);
	    if (afs_uuid_equal(uuidp, &tuuid)) {
		*expp = exp;
		*basep = base;
		ERROR_EXIT(0);
	    }
	}
    }

    if (createit) {
	if (hostslot == -1) {
	    for (i = 0; i < MAXSERVERID + 1; i++) {
		if (!hostaddress[i])
		    break;
	    }
	    if (i > MAXSERVERID)
		ERROR_EXIT(VL_REPSFULL);
	} else {
	    i = hostslot;
	}

	for (base = 0; base < VL_MAX_ADDREXTBLKS; base++) {
	    if (!ex_addr[0]->ex_contaddrs[base]) {
		code = GetExtentBlock(ctx, base);
		if (code)
		    ERROR_EXIT(code);
	    }
	    for (j = 1; j < VL_MHSRV_PERBLK; j++) {
		exp = &ex_addr[base][j];
		tuuid = exp->ex_hostuuid;
		afs_ntohuuid(&tuuid);
		if (afs_uuid_is_nil(&tuuid)) {
		    tuuid = *uuidp;
		    afs_htonuuid(&tuuid);
		    exp->ex_hostuuid = tuuid;
		    code = vlwrite_exblock(ctx, base, ex_addr[base],
					   ntohl(ex_addr[0]->ex_contaddrs[base]),
					   exp, sizeof(tuuid));
		    if (code)
			ERROR_EXIT(VL_IO);
		    hostaddress[i] =
			0xff000000 | ((base << 16) & 0xff0000) | (j & 0xffff);
		    *expp = exp;
		    *basep = base;
		    if (!vlctx_kv(ctx) && cache->vldbversion != VLDBVERSION_4) {
			cheader->vital_header.vldbversion =
			    htonl(VLDBVERSION_4);
			code = write_vital_vlheader(ctx);
			if (code)
			    ERROR_EXIT(VL_IO);
		    }
		    cheader->IpMappedAddr[i] = htonl(hostaddress[i]);
		    code = vlwrite_cheader(ctx, cheader,
					   &cheader->IpMappedAddr[i],
					   sizeof(afs_int32));
		    if (code)
			ERROR_EXIT(VL_IO);
		    ERROR_EXIT(0);
		}
	    }
	}
	ERROR_EXIT(VL_REPSFULL);	/* No reason to utilize a new error code */
    }

  error_exit:
    return error;
}

/* Allocate a free block of storage for entry, returning address of a new
   zeroed entry (or zero if something is wrong).  */
afs_int32
AllocBlock(struct vl_ctx *ctx, struct nvlentry *tentry)
{
    afs_int32 blockindex;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    if (vlctx_kv(ctx)) {
	/*
	 * Our KV variant doesn't use allocated blocks. Just return something
	 * nonzero to indicate non-failure. The actual value will be passed
	 * around to various functions, but won't be used for anything.
	 */
	return VL4KV_FAKE_BLOCKINDEX;
    }

    if (cheader->vital_header.freePtr) {
	/* allocate this dude */
	blockindex = ntohl(cheader->vital_header.freePtr);
	if (vlentryread(ctx, blockindex, tentry))
	    return 0;
	cheader->vital_header.freePtr = htonl(tentry->nextIdHash[0]);
    } else {
	afs_int32 code;
	/* hosed, nothing on free list, grow file */
	code = grow_eofPtr(cheader, sizeof(vlentry), &blockindex);
	if (code)
	    return 0;
    }
    cheader->vital_header.allocs++;
    if (write_vital_vlheader(ctx))
	return 0;
    memset(tentry, 0, sizeof(nvlentry));	/* zero new entry */
    return blockindex;
}


/* Free a block given its index.  It must already have been unthreaded. Returns zero for success or an error code on failure. */
int
FreeBlock(struct vl_ctx *ctx, afs_int32 blockindex)
{
    struct nvlentry tentry;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    /* check validity of blockindex just to be on the safe side */
    if (!index_OK(ctx, blockindex))
	return VL_BADINDEX;

    if (vlctx_kv(ctx)) {
	/*
	 * For KV, we don't actually alloc blocks, so we don't need to free
	 * them or maintain a freelist, etc, so just do nothing here. The
	 * actual key/value for the vlentry should have already been deleted
	 * when we "unhash"ed the RW id for that volume.
	 */
	return 0;
    }

    memset(&tentry, 0, sizeof(nvlentry));
    tentry.nextIdHash[0] = cheader->vital_header.freePtr;	/* already in network order */
    tentry.flags = htonl(VLFREE);
    cheader->vital_header.freePtr = htonl(blockindex);
    if (vlwrite(ctx, blockindex, &tentry, sizeof(nvlentry)))
	return VL_IO;
    cheader->vital_header.frees++;
    if (write_vital_vlheader(ctx))
	return VL_IO;
    return 0;
}

static afs_int32
kv_FindByID(struct vl_ctx *ctx, afs_uint32 volid, afs_int32 voltype,
	    struct nvlentry *tentry, afs_int32 *error)
{
    struct vl4kv_volidkey ikey;
    struct rx_opaque keybuf;
    afs_int32 code;

    /*
     * The callers of this function don't actually care what the returned
     * blockindex is on success; they only care if the entry exists
     * (blockindex != 0). Since we don't actually use physical file offsets for
     * KV, just return something nonzero to indicate success.
     */
    afs_int32 blockindex = VL4KV_FAKE_BLOCKINDEX;

    init_volidkey(&keybuf, &ikey, volid);

    code = kv_vlentryget(ctx, &keybuf, tentry);
    if (code != 0) {
	if (code == VL_NOENT) {
	    code = 0;
	}
	*error = code;
	return 0;
    }

    if (voltype == -1) {
	afs_int32 typeindex;
	for (typeindex = 0; typeindex < MAXTYPES; typeindex++) {
	    if (volid == tentry->volumeId[typeindex]) {
		return blockindex;
	    }
	}
    } else if (volid == tentry->volumeId[voltype]) {
	return blockindex;
    }

    /* Entry "not found"; we found a tentry for the given volid, but the given
     * volume ID doesn't appear in the tentry in the proper place. */
    VLog(0, ("vldb4-kv: Internal error: Looking up volume id %u found "
	     "volume entry with RW id %u\n", volid, tentry->volumeId[RWVOL]));
    *error = VL_BADENTRY;
    return 0;
}

/* Look for a block by volid and voltype (if not known use -1 which searches
 * all 3 volid hash lists. Note that the linked lists are read in first from
 * the database header.  If found read the block's contents into the area
 * pointed to by tentry and return the block's index.  If not found return 0.
 */
afs_int32
FindByID(struct vl_ctx *ctx, afs_uint32 volid, afs_int32 voltype,
	 struct nvlentry *tentry, afs_int32 *error)
{
    afs_int32 typeindex, hashindex, blockindex;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    *error = 0;

    if (vlctx_kv(ctx)) {
	return kv_FindByID(ctx, volid, voltype, tentry, error);
    }

    hashindex = IDHash(volid);
    if (voltype == -1) {
/* Should we have one big hash table for volids as opposed to the three ones? */
	for (typeindex = 0; typeindex < MAXTYPES; typeindex++) {
	    for (blockindex = ntohl(cheader->VolidHash[typeindex][hashindex]);
		 blockindex != NULLO;
		 blockindex = tentry->nextIdHash[typeindex]) {
		if (vlentryread(ctx, blockindex, tentry)) {
		    *error = VL_IO;
		    return 0;
		}
		if (volid == tentry->volumeId[typeindex])
		    return blockindex;
	    }
	}
    } else {
	for (blockindex = ntohl(cheader->VolidHash[voltype][hashindex]);
	     blockindex != NULLO; blockindex = tentry->nextIdHash[voltype]) {
	    if (vlentryread(ctx, blockindex, tentry)) {
		*error = VL_IO;
		return 0;
	    }
	    if (volid == tentry->volumeId[voltype])
		return blockindex;
	}
    }
    return 0;			/* no such entry */
}

static afs_int32
kv_FindByName(struct vl_ctx *ctx, char *aname, struct nvlentry *tentry,
	      afs_int32 *error)
{
    afs_int32 code;
    struct rx_opaque keybuf;
    struct vl4kv_volnamekey nkey;

    /*
     * The callers of this function don't actually care what the returned
     * blockindex is on success; they only care if the entry exists (blockindex
     * != 0). Since we don't actually use physical file offsets for KV, just
     * return nonzero to indicate success.
     */
    afs_int32 blockindex = VL4KV_FAKE_BLOCKINDEX;

    init_volnamekey(&keybuf, &nkey, aname);

    code = kv_vlentryget(ctx, &keybuf, tentry);
    if (code != 0) {
	if (code == VL_NOENT) {
	    code = 0;
	}
	*error = code;
	return 0;
    }

    if (strcmp(aname, tentry->name) == 0) {
	return blockindex;
    }

    /* Entry "not found"; we found a tentry for the given name, but the tentry
     * we found doesn't seem to match the given name. */
    VLog(0, ("vldb4-kv: Internal error: Looking up volume name '%s' found "
	     "volume entry with name '%s'\n", aname, tentry->name));
    *error = VL_BADENTRY;
    return 0;
}

/* Look for a block by volume name. If found read the block's contents into
 * the area pointed to by tentry and return the block's index.  If not
 * found return 0.
 */
afs_int32
FindByName(struct vl_ctx *ctx, char *volname, struct nvlentry *tentry,
	   afs_int32 *error)
{
    afs_int32 hashindex;
    afs_int32 blockindex;
    char tname[VL_MAXNAMELEN];
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    /* remove .backup or .readonly extensions for stupid backwards
     * compatibility
     */
    hashindex = strlen(volname);	/* really string length */
    if (hashindex >= 8 && strcmp(volname + hashindex - 7, ".backup") == 0) {
	/* this is a backup volume */
	if (strlcpy(tname, volname, sizeof(tname)) >= sizeof(tname)) {
	    *error = VL_BADNAME;
	    return 0;
	}
	tname[hashindex - 7] = 0;	/* zap extension */
    } else if (hashindex >= 10
	       && strcmp(volname + hashindex - 9, ".readonly") == 0) {
	/* this is a readonly volume */
	if (strlcpy(tname, volname, sizeof(tname)) >= sizeof(tname)) {
	    *error = VL_BADNAME;
	    return 0;
	}
	tname[hashindex - 9] = 0;	/* zap extension */
    } else {
	if (strlcpy(tname, volname, sizeof(tname)) >= sizeof(tname)) {
	    *error = VL_BADNAME;
	    return 0;
	}
    }

    *error = 0;

    if (vlctx_kv(ctx)) {
	return kv_FindByName(ctx, tname, tentry, error);
    }

    hashindex = NameHash(tname);
    for (blockindex = ntohl(cheader->VolnameHash[hashindex]);
	 blockindex != NULLO; blockindex = tentry->nextNameHash) {
	if (vlentryread(ctx, blockindex, tentry)) {
	    *error = VL_IO;
	    return 0;
	}
	if (!strcmp(tname, tentry->name))
	    return blockindex;
    }
    return 0;			/* no such entry */
}

/**
 * Returns whether or not any of the supplied volume IDs already exist
 * in the vldb.
 *
 * @param ctx      transaction context
 * @param ids      an array of volume IDs
 * @param ids_len  the number of elements in the 'ids' array
 * @param error    filled in with an error code in case of error
 *
 * @return whether any of the volume IDs are already used
 *  @retval 1  at least one of the volume IDs is already used
 *  @retval 0  none of the volume IDs are used, or an error occurred
 */
int
EntryIDExists(struct vl_ctx *ctx, const afs_uint32 *ids,
	      afs_int32 ids_len, afs_int32 *error)
{
    afs_int32 typeindex;
    struct nvlentry tentry;

    *error = 0;

    for (typeindex = 0; typeindex < ids_len; typeindex++) {
	if (ids[typeindex]
	    && FindByID(ctx, ids[typeindex], -1, &tentry, error)) {

	    return 1;
	} else if (*error) {
	    return 0;
	}
    }

    return 0;
}

/**
 * Finds the next range of unused volume IDs in the vldb.
 *
 * @param ctx       transaction context
 * @param maxvolid  the current max vol ID, and where to start looking
 *                  for an unused volume ID range
 * @param bump      how many volume IDs we need to be unused
 * @param error     filled in with an error code in case of error
 *
 * @return the next volume ID 'volid' such that the range
 *         [volid, volid+bump) of volume IDs is unused, or 0 if there's
 *         an error
 */
afs_uint32
NextUnusedID(struct vl_ctx *ctx, afs_uint32 maxvolid, afs_uint32 bump,
	     afs_int32 *error)
{
    struct nvlentry tentry;
    afs_uint32 id;
    afs_uint32 nfree;

    *error = 0;

     /* we simply start at the given maxvolid, keep a running tally of
      * how many free volume IDs we've seen in a row, and return when
      * we've seen 'bump' unused IDs in a row */
    for (id = maxvolid, nfree = 0; nfree < bump; ++id) {
	if (FindByID(ctx, id, -1, &tentry, error)) {
	    nfree = 0;
	} else if (*error) {
	    return 0;
	} else {
	    ++nfree;
	}
    }

    /* 'id' is now at the end of the [maxvolid,maxvolid+bump) range,
     * but we need to return the first unused id, so subtract the
     * number of current running free IDs to get the beginning */
    return id - nfree;
}

int
HashNDump(struct vl_ctx *ctx, int hashindex)
{
    int i = 0;
    int blockindex;
    struct nvlentry tentry;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    if (vlctx_kv(ctx)) {
	VLog(0, ("[%d]: Using KV store: no internal hash tables\n", hashindex));
	return 0;
    }

    for (blockindex = ntohl(cheader->VolnameHash[hashindex]);
	 blockindex != NULLO; blockindex = tentry.nextNameHash) {
	if (vlentryread(ctx, blockindex, &tentry))
	    return 0;
	i++;
	VLog(0,
	     ("[%d]#%d: %10d %d %d (%s)\n", hashindex, i, tentry.volumeId[0],
	      tentry.nextIdHash[0], tentry.nextNameHash, tentry.name));
    }
    return 0;
}


int
HashIdDump(struct vl_ctx *ctx, int hashindex)
{
    int i = 0;
    int blockindex;
    struct nvlentry tentry;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    if (vlctx_kv(ctx)) {
	VLog(0, ("[%d]: Using KV store: no internal hash tables\n", hashindex));
	return 0;
    }

    for (blockindex = ntohl(cheader->VolidHash[0][hashindex]);
	 blockindex != NULLO; blockindex = tentry.nextIdHash[0]) {
	if (vlentryread(ctx, blockindex, &tentry))
	    return 0;
	i++;
	VLog(0,
	     ("[%d]#%d: %10d %d %d (%s)\n", hashindex, i, tentry.volumeId[0],
	      tentry.nextIdHash[0], tentry.nextNameHash, tentry.name));
    }
    return 0;
}


/* Add a block to the hash table given a pointer to the block and its index.
 * The block is threaded onto both hash tables and written to disk.  The
 * routine returns zero if there were no errors.
 */
int
ThreadVLentry(struct vl_ctx *ctx, afs_int32 blockindex,
	      struct nvlentry *tentry)
{
    int errorcode;

    if (!index_OK(ctx, blockindex))
	return VL_BADINDEX;
    /* Insert into volid's hash linked list */
    if ((errorcode = HashVolid(ctx, RWVOL, blockindex, tentry)))
	return errorcode;

    /* For rw entries we also enter the RO and BACK volume ids (if they
     * exist) in the hash tables; note all there volids (RW, RO, BACK)
     * should not be hashed yet! */
    if (tentry->volumeId[ROVOL]) {
	if ((errorcode = HashVolid(ctx, ROVOL, blockindex, tentry)))
	    return errorcode;
    }
    if (tentry->volumeId[BACKVOL]) {
	if ((errorcode = HashVolid(ctx, BACKVOL, blockindex, tentry)))
	    return errorcode;
    }

    /* Insert into volname's hash linked list */
    HashVolname(ctx, blockindex, tentry);

    /* Update cheader entry */
    if (write_vital_vlheader(ctx))
	return VL_IO;

    /* Update hash list pointers in the entry itself */
    if (vlentrywrite(ctx, blockindex, tentry))
	return VL_IO;
    return 0;
}


/* Remove a block from both the hash tables.  If success return 0, else
 * return an error code. */
int
UnthreadVLentry(struct vl_ctx *ctx, afs_int32 blockindex,
		struct nvlentry *aentry)
{
    afs_int32 errorcode, typeindex;

    if (!index_OK(ctx, blockindex))
	return VL_BADINDEX;
    if ((errorcode = UnhashVolid(ctx, RWVOL, blockindex, aentry)))
	return errorcode;

    /* Take the RO/RW entries of their respective hash linked lists. */
    for (typeindex = ROVOL; typeindex <= BACKVOL; typeindex++) {
	if ((errorcode = UnhashVolid(ctx, typeindex, blockindex, aentry))) {
	    if (errorcode == VL_NOENT) {
		VLog(0, ("Unable to unhash vlentry '%s' (address %d) from hash "
			 "chain for volid %u (type %d).\n",
			 aentry->name, blockindex, aentry->volumeId[typeindex], typeindex));
		VLog(0, ("... The VLDB may be partly corrupted; see vldb_check "
			 "for how to check for and fix errors.\n"));
		return VL_DBBAD;
	    }
	    return errorcode;
	}
    }

    /* Take it out of the Volname hash list */
    if ((errorcode = UnhashVolname(ctx, blockindex, aentry))) {
	if (errorcode == VL_NOENT) {
	    VLog(0, ("Unable to unhash vlentry '%s' (address %d) from name "
		     "hash chain.\n", aentry->name, blockindex));
	    VLog(0, ("... The VLDB may be partly corrupted; see vldb_check "
		     "for how to check for and fix errors.\n"));
	    return VL_DBBAD;
	}
	return errorcode;
    }

    /* Update cheader entry */
    write_vital_vlheader(ctx);

    return 0;
}

/* cheader must have be read before this routine is called. */
int
HashVolid(struct vl_ctx *ctx, afs_int32 voltype, afs_int32 blockindex,
          struct nvlentry *tentry)
{
    afs_int32 hashindex, errorcode;
    struct nvlentry ventry;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    if (FindByID
	(ctx, tentry->volumeId[voltype], voltype, &ventry, &errorcode))
	return VL_IDALREADYHASHED;
    else if (errorcode)
	return errorcode;

    if (vlctx_kv(ctx)) {
	/* For KV, we hash the vlentry when we write out the vlentry itself. */
	return 0;
    }

    hashindex = IDHash(tentry->volumeId[voltype]);
    tentry->nextIdHash[voltype] =
	ntohl(cheader->VolidHash[voltype][hashindex]);
    cheader->VolidHash[voltype][hashindex] = htonl(blockindex);
    if (vlwrite_cheader(ctx, cheader,
			&cheader->VolidHash[voltype][hashindex],
			sizeof(afs_int32)))
	return VL_IO;
    return 0;
}

/*
 * Make the given key (a volid or volname key) no longer point to the given
 * vlentry. Check that the key is pointing at the right vlentry before deleting
 * it.
 */
static int
kv_unhashkey(struct vl_ctx *ctx, struct rx_opaque *key,
	     struct nvlentry *aentry)
{
    afs_uint32 found_rwid = 0;
    afs_uint32 rwid = aentry->volumeId[RWVOL];
    int code;
    int noent = 0;

    /* Make sure the given key is actually pointing to the RW volid for this
     * vlentry. */
    code = ubik_KVGetCopy(ctx->trans, key, &found_rwid, sizeof(found_rwid),
			  &noent);
    if (code != 0) {
	return code;
    }
    if (noent) {
	/*
	 * Entry doesn't exist; our work is already done.
	 *
	 * There is perhaps an argument here that we should return an error if
	 * the given key does not exist, since a caller expects the key to
	 * exist (that's why we're deleting it), and the lack of the key
	 * existing indicates a mistake or corruption somewhere.
	 *
	 * However, the current scheme of "hashing" volume entries means that
	 * we must unhash all entries for a volume when the RW id is unhashed
	 * (since we store volume entries by RW id), which makes it difficult
	 * to return an error here. The existing vldb4 code for deleting a
	 * volume entry, for example, unhashes the RW id, then the other ids,
	 * and then the volume name. This means that we unhash everything, and
	 * then try to unhash the other volids/name, which have already been
	 * unhashed.
	 *
	 * It's possible to accommodate for this at the higher levels of the db
	 * logic, but it seems easier to just not throw an error when we try to
	 * delete a key that's not there. In other words, deleting the 'hash'
	 * keys for a volume is treated as idempotent, like free(NULL).
	 */
	return 0;
    }

    found_rwid = ntohl(found_rwid);
    rwid = aentry->volumeId[RWVOL];
    if (found_rwid != rwid) {
	/*
	 * The entry for 'volid' seems to be pointing to 'found_rwid', not
	 * the actual rwid in 'aentry'. That's weird; bail out with an
	 * error.
	 */
	VLog(0, ("Error: Tried to unhash volume RW id %u, but existing hash "
		 "entry was found for RW id %u.\n", rwid, found_rwid));
	return VL_DBBAD;
    }

    return ubik_KVDelete(ctx->trans, key, NULL);
}

static int
kv_UnhashVolname(struct vl_ctx *ctx, struct nvlentry *aentry)
{
    struct vl4kv_volnamekey nkey;
    struct rx_opaque keybuf;

    opr_StaticAssert(sizeof(nkey.name) == sizeof(aentry->name));

    init_volnamekey(&keybuf, &nkey, aentry->name);
    return kv_unhashkey(ctx, &keybuf, aentry);
}

static int
kv_UnhashVolid(struct vl_ctx *ctx, afs_int32 voltype, struct nvlentry *aentry)
{
    int code;
    afs_uint32 volid;
    struct vl4kv_volidkey ikey;
    struct rx_opaque keybuf;

    volid = aentry->volumeId[voltype];
    init_volidkey(&keybuf, &ikey, volid);

    if (voltype != RWVOL) {
	return kv_unhashkey(ctx, &keybuf, aentry);
    }

    /*
     * For unhashing the RW volid, we must remove the KV hash entries for
     * all the other volids and the volume name too, since those refer to
     * this vlentry by its RW volid. If we change the RW id, those
     * references will no longer be valid, so remove them here. (We'll add
     * them back with the proper RW id when we write out the vlentry).
     */
    for (voltype = ROVOL; voltype <= BACKVOL; voltype++) {
	code = kv_UnhashVolid(ctx, voltype, aentry);
	if (code != 0) {
	    return code;
	}
    }
    code = kv_UnhashVolname(ctx, aentry);
    if (code != 0) {
	return code;
    }

    /*
     * In kv_unhashkey, we return 0 if the given key doesn't exist. Here, we
     * return an error if the key doesn't exist (since we give a NULL to
     * ubik_KVDelete); why the difference in behavior?
     *
     * The key for the RW id for the volume really should exist at this point.
     * The other keys may get "unhashed" during various times of
     * unhashing/rehashing a volume, but the key for the RW id is for the
     * volume itself, and really shouldn't go away unless we really are
     * unhashing the RW id.
     */
    return ubik_KVDelete(ctx->trans, &keybuf, NULL);
}

/* cheader must have be read before this routine is called. */
int
UnhashVolid(struct vl_ctx *ctx, afs_int32 voltype, afs_int32 blockindex,
	    struct nvlentry *aentry)
{
    int hashindex, nextblockindex, prevblockindex;
    struct nvlentry tentry;
    afs_int32 code;
    afs_int32 temp;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    if (aentry->volumeId[voltype] == NULLO)	/* Assume no volume id */
	return 0;

    if (vlctx_kv(ctx)) {
	return kv_UnhashVolid(ctx, voltype, aentry);
    }

    /* Take it out of the VolId[voltype] hash list */
    hashindex = IDHash(aentry->volumeId[voltype]);
    nextblockindex = ntohl(cheader->VolidHash[voltype][hashindex]);
    if (nextblockindex == blockindex) {
	/* First on the hash list; just adjust pointers */
	cheader->VolidHash[voltype][hashindex] =
	    htonl(aentry->nextIdHash[voltype]);
	code = vlwrite_cheader(ctx, cheader,
			       &cheader->VolidHash[voltype][hashindex],
			       sizeof(afs_int32));
	if (code)
	    return VL_IO;
    } else {
	while (nextblockindex != blockindex) {
	    prevblockindex = nextblockindex;	/* always done once */
	    if (vlentryread(ctx, nextblockindex, &tentry))
		return VL_IO;
	    if ((nextblockindex = tentry.nextIdHash[voltype]) == NULLO)
		return VL_NOENT;
	}
	temp = tentry.nextIdHash[voltype] = aentry->nextIdHash[voltype];
	temp = htonl(temp);	/* convert to network byte order before writing */
	if (vlwrite
	    (ctx,
	     DOFFSET(prevblockindex, &tentry, &tentry.nextIdHash[voltype]),
	     &temp, sizeof(afs_int32)))
	    return VL_IO;
    }
    aentry->nextIdHash[voltype] = 0;
    return 0;
}


int
HashVolname(struct vl_ctx *ctx, afs_int32 blockindex,
	    struct nvlentry *aentry)
{
    afs_int32 hashindex;
    afs_int32 code;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    if (vlctx_kv(ctx)) {
	/* For KV, we hash the vlentry when we write out the vlentry itself. */
	return 0;
    }

    /* Insert into volname's hash linked list */
    hashindex = NameHash(aentry->name);
    aentry->nextNameHash = ntohl(cheader->VolnameHash[hashindex]);
    cheader->VolnameHash[hashindex] = htonl(blockindex);
    code = vlwrite_cheader(ctx, cheader,
			   &cheader->VolnameHash[hashindex],
			   sizeof(afs_int32));
    if (code)
	return VL_IO;
    return 0;
}


int
UnhashVolname(struct vl_ctx *ctx, afs_int32 blockindex,
	      struct nvlentry *aentry)
{
    afs_int32 hashindex, nextblockindex, prevblockindex;
    struct nvlentry tentry;
    afs_int32 temp;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    if (vlctx_kv(ctx)) {
	return kv_UnhashVolname(ctx, aentry);
    }

    /* Take it out of the Volname hash list */
    hashindex = NameHash(aentry->name);
    nextblockindex = ntohl(cheader->VolnameHash[hashindex]);
    if (nextblockindex == blockindex) {
	/* First on the hash list; just adjust pointers */
	cheader->VolnameHash[hashindex] = htonl(aentry->nextNameHash);
	if (vlwrite_cheader(ctx, cheader,
			    &cheader->VolnameHash[hashindex],
			    sizeof(afs_int32)))
	    return VL_IO;
    } else {
	while (nextblockindex != blockindex) {
	    prevblockindex = nextblockindex;	/* always done at least once */
	    if (vlentryread(ctx, nextblockindex, &tentry))
		return VL_IO;
	    if ((nextblockindex = tentry.nextNameHash) == NULLO)
		return VL_NOENT;
	}
	tentry.nextNameHash = aentry->nextNameHash;
	temp = htonl(tentry.nextNameHash);
	if (vlwrite
	    (ctx, DOFFSET(prevblockindex, &tentry, &tentry.nextNameHash),
	     &temp, sizeof(afs_int32)))
	    return VL_IO;
    }
    aentry->nextNameHash = 0;
    return 0;
}

static afs_int32
kv_NextEntry(struct vl_ctx *ctx, afs_int32 blockindex, struct nvlentry *tentry,
	     afs_int32 *remaining)
{
    afs_uint32 volid = blockindex;
    int code;
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;
    struct vl4kv_volidkey id_key;

    memset(&valbuf, 0, sizeof(valbuf));

    /*
     * Our strategy for traversing all possible volume entries is that we scan
     * through all key/value pairs, looking for keys that start with
     * VL4KV_KEY_VOLID, and whose value has the same size as an nvlentry. When
     * we find one, we return the RW volume id for that nvlentry as the
     * 'blockindex' to start from the next time we are called.
     */

    if (volid == 0) {
	/* First call; get the first item in the KV store. */
	memset(&keybuf, 0, sizeof(keybuf));
    } else {
	init_volidkey(&keybuf, &id_key, volid);
    }

    for (;;) {
	int eof = 0;
	code = ubik_KVNext(ctx->trans, &keybuf, &valbuf, &eof);
	if (code != 0) {
	    goto error;
	}
	if (eof) {
	    goto eof;
	}

	if (keybuf.len == sizeof(id_key) && valbuf.len == sizeof(*tentry)) {

	    opaque_copy(&keybuf, &id_key, sizeof(id_key));

	    id_key.tag = ntohl(id_key.tag);
	    id_key.volid = ntohl(id_key.volid);

	    if (id_key.tag == VL4KV_KEY_VOLID) {
		/* We found a key/value pair for a vlentry. Return it to the
		 * caller. */
		struct nvlentry spare_entry;
		opaque_copy(&valbuf, &spare_entry, sizeof(spare_entry));
		nvlentry_ntohl(&spare_entry, tentry);
		volid = id_key.volid;

		if (volid == 0) {
		    /*
		     * Skip keys that appear to be for volume id 0. These
		     * shouldn't happen, but sometimes old crufty vldb's have
		     * weird entries in them that get converted. And returning
		     * 0 indicates EOF, so just make sure we don't return 0 if
		     * there are possibly more entries remaining.
		     */
		    continue;
		}

		goto success;
	    }
	}
    }

 error:
    *remaining = -1;
    return 0;

 eof:
    *remaining = 0;
    return 0;

 success:
    /*
     * We don't actually know how many entries are left. All in-tree callers at
     * least don't seem to care; just return 1 to at least indicate that more
     * entries are possible.
     */
    *remaining = 1;
    return volid;
}

/* Returns the vldb entry tentry at offset index; remaining is the number of
 * entries left; the routine also returns the index of the next sequential
 * entry in the vldb
 */

afs_int32
NextEntry(struct vl_ctx *ctx, afs_int32 blockindex,
	  struct nvlentry *tentry, afs_int32 *remaining)
{
    afs_int32 lastblockindex;
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    if (vlctx_kv(ctx)) {
	return kv_NextEntry(ctx, blockindex, tentry, remaining);
    }

    if (blockindex == 0)	/* get first one */
	blockindex = sizeof(*cheader);
    else {
	if (!index_OK(ctx, blockindex)) {
	    *remaining = -1;	/* error */
	    return 0;
	}
	blockindex += sizeof(nvlentry);
    }
    /* now search for the first entry that isn't free */
    for (lastblockindex = ntohl(cheader->vital_header.eofPtr);
	 blockindex < lastblockindex;) {
	if (vlentryread(ctx, blockindex, tentry)) {
	    *remaining = -1;
	    return 0;
	}
	if (tentry->flags == VLCONTBLOCK) {
	    /*
	     * This is a special mh extension block just simply skip over it
	     */
	    blockindex += VL_ADDREXTBLK_SIZE;
	} else {
	    if (tentry->flags != VLFREE) {
		/* estimate remaining number of entries, not including this one */
		*remaining =
		    (lastblockindex - blockindex) / sizeof(nvlentry) - 1;
		return blockindex;
	    }
	    blockindex += sizeof(nvlentry);
	}
    }
    *remaining = 0;		/* no more entries */
    return 0;
}


/* Routine to verify that index is a legal offset to a vldb entry in the
 * table
 */
static int
index_OK(struct vl_ctx *ctx, afs_int32 blockindex)
{
    struct vl_cache *cache = ctx->cache;
    struct vlheader *cheader = &cache->cheader;
    if (vlctx_kv(ctx)) {
	/* We don't use file offsets in KV, so just check if the given
	 * blockindex matches the "fake" one we give everyone. */
	if (blockindex == VL4KV_FAKE_BLOCKINDEX) {
	    return 1;
	}
	return 0;
    }
    if ((blockindex < sizeof(*cheader))
	|| (blockindex >= ntohl(cheader->vital_header.eofPtr)))
	return 0;
    return 1;
}

int
vlsynccache(void)
{
    return vlcache_copy(&rd_vlcache, &wr_vlcache);
}

/* dbcheck_func callback; just checks if the database sesms usable. */
int
vl_checkdb(struct ubik_trans *trans)
{
    struct vl_ctx ctx_s;
    struct vl_ctx *ctx = &ctx_s;
    struct vl_cache *cache;
    int code;
    int ex_i;

    memset(ctx, 0, sizeof(*ctx));

    cache = calloc(1, sizeof(*cache));
    if (cache == NULL) {
	return UNOMEM;
    }

    ctx->trans = trans;
    ctx->cache = cache;

    code = UpdateCache(trans, ctx);

    for (ex_i = 0; ex_i < VL_MAX_ADDREXTBLKS; ex_i++) {
	free(cache->ex_addr[ex_i]);
	cache->ex_addr[ex_i] = NULL;
    }
    free(cache);

    return code;
}

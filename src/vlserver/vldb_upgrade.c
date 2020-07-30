/*
 * Copyright (c) 2021 Sine Nomine Associates
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>

#include <afs/cellconfig.h>
#include <afs/cmd.h>
#include <afs/com_err.h>
#include <afs/okv.h>
#include <afs/afsctl.h>

#include "ubik_internal.h"
#include "vlserver_internal.h"

#include <sys/mman.h>

enum {
    OPT_input,
    OPT_output,
    OPT_to,
    OPT_quiet,
    OPT_progress,
    OPT_no_progress,
    OPT_ignore_epoch,
    OPT_force_type,

    OPT_online,
    OPT_backup_suffix,
    OPT_no_backup,
    OPT_dist,
    OPT_ctl_socket,
};

struct vltype;
struct vlupgrade_ctx;
struct vlupgrade_db;
struct vl4_addrs;

struct vlloader_ops {
    int (*vlo_init)(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db);
    int (*vlo_getaddrs)(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db,
			struct vl4_addrs *addrs);
    int (*vlo_getvol)(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db,
		      struct nvlentry *avol, int *a_eof);
};

struct vldumper_ops {
    int (*vdo_init)(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db);
    int (*vdo_dumpaddrs)(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db,
			 struct vl4_addrs *addrs);
    int (*vdo_writevol)(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db,
			struct nvlentry *avol);
    int (*vdo_finish)(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db);
};

struct vltype {
    char *vlt_name;
    int vlt_kv;
    struct vlloader_ops *vlt_lops;
    struct vldumper_ops *vlt_dops;
};

static struct vlloader_ops vl4x_lops;
static struct vldumper_ops vl4x_dops;

static struct vltype vlt_vldb4 = {
    .vlt_name = "vldb4",
    .vlt_lops = &vl4x_lops,
    .vlt_dops = &vl4x_dops,
};
static struct vltype vlt_vldb4kv = {
    .vlt_name = "vldb4-kv",
    .vlt_kv = 1,
    .vlt_lops = &vl4x_lops,
    .vlt_dops = &vl4x_dops,
};

struct vl4_addrs {
    afs_uint32 hostaddress[MAXSERVERID+1];
    struct extentaddr *ex_addr[VL_MAX_ADDREXTBLKS];
};

struct vlupgrade_vl4 {
    struct vl_ctx vl_ctx;
    struct vl_cache vlcache_s;
};

struct vlupgrade_db {
    struct vltype *vltype;
    char *path;

    struct ubik_dbase *udbase;
    struct ubik_trans *utrans;

    struct ubik_version uversion;

    afs_uint32 vol_curpos;
    afs_uint64 maxvolid;

    struct vlupgrade_vl4 *vl4;
    struct vlupgrade_vl4 vl4_s;
};

struct vlupgrade_ctx {
    /* Options etc */
    char *src_path;
    char *dest_path;
    char *dest_suffix;
    struct vltype *dest_vltype;

    int quiet;
    int ignore_epoch;
    int force_type;

    struct opr_progress_opts progopts;
    struct opr_progress *progress;

    /* Online options */
    int online;
    char *backup_suffix;
    char *backup_path;
    int no_dist;
    int need_dist;
    struct afsctl_clientinfo ctl_cinfo;

    /* Online data */
    struct ubik_freeze_client *freeze;
    struct ubik_version64 freeze_version;

    /* Database data */
    struct vlupgrade_db src_db;
    struct vlupgrade_db dest_db;
};

static void
AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3)
print_error(int code, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "\n%s: ", getprogname());
    if (fmt != NULL) {
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (code != 0) {
	    fprintf(stderr, ": ");
	}
    }
    if (code != 0) {
	fprintf(stderr, "%s\n", afs_error_message(code));
    } else {
	fprintf(stderr, "\n");
    }
}

static void
AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3)
print_nq(struct vlupgrade_ctx *vlu, const char *fmt, ...)
{
    if (!vlu->quiet) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fflush(stderr);
    }
}

static void
vl4_init(struct vlupgrade_db *db)
{
    struct vlupgrade_vl4 *vl4;

    opr_Assert(db->utrans != NULL);

    vl4 = db->vl4 = &db->vl4_s;

    vl4->vl_ctx.trans = db->utrans;
    vl4->vl_ctx.cache = &vl4->vlcache_s;

    /*
     * Don't write the cheader when writing a vldb4 db; we'll write the cheader
     * ourselves at the end. Skipping this makes writing a vldb4-flatfile db go
     * much faster, since otherwise we'll write to disk to update the cheader
     * every time we write out a volume.
     */
    vl4->vl_ctx.cheader_nowrite = 1;

    /*
     * Don't check if a volume already exists in the db; assume the source db
     * doesn't have duplicate records. Skipping this makes writing a
     * vldb4-flatfile db go much faster.
     */
    vl4->vl_ctx.hash_nocollide = 1;
}

static int
vl4xload_init(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db)
{
    int addr_i;
    int code;
    struct vlupgrade_vl4 *vl4;
    struct vl_ctx *vl_ctx;
    struct vl_cache *cache;
    struct vlheader *cheader;
    afs_uint32 vlvers;

    vl4_init(db);
    vl4 = db->vl4;
    vl_ctx = &vl4->vl_ctx;
    cache = vl_ctx->cache;
    cheader = &cache->cheader;

    code = vlread_cheader(vl_ctx, cheader);
    if (code != 0) {
	print_error(code, "Cannot read cheader from %s", db->path);
	goto error;
    }

    db->maxvolid = ntohl(cheader->vital_header.MaxVolumeId);
    vlvers = ntohl(cheader->vital_header.vldbversion);
    cache->vldbversion = vlvers;
    cache->maxnservers = 13;

    if (db->vltype->vlt_kv) {
	if (vlvers != VLDBVERSION_4_KV) {
	    fprintf(stderr, "vldb4-kv: Found unrecognized vldb version %x\n", vlvers);
	    goto error;
	}
    } else {
	if (vlvers != VLDBVERSION_3 && vlvers != VLDBVERSION_4) {
	    fprintf(stderr, "vldb4: Found unrecognized vldb version %x\n", vlvers);
	    goto error;
	}
    }

    opr_StaticAssert(sizeof(cache->hostaddress) ==
		     sizeof(cache->hostaddress[0]) * (MAXSERVERID+1));
    opr_StaticAssert(sizeof(cheader->IpMappedAddr) ==
		     sizeof(cheader->IpMappedAddr[0]) * (MAXSERVERID+1));
    opr_StaticAssert(sizeof(cache->hostaddress) ==
		     sizeof(cheader->IpMappedAddr));
    for (addr_i = 0; addr_i < MAXSERVERID + 1; addr_i++) {
	cache->hostaddress[addr_i] = ntohl(cheader->IpMappedAddr[addr_i]);
    }

    code = readExtents(vl_ctx);
    if (code != 0) {
	fprintf(stderr, "Error loading extents from %s\n", db->path);
	goto error;
    }

    if (db->vltype->vlt_kv) {
	struct ubik_stat ustat;

	memset(&ustat, 0, sizeof(ustat));

	code = ukv_stat(db->path, &ustat);
	if (code != 0) {
	    print_error(code, "ukv_stat failed");
	    goto error;
	}

	if (ustat.n_items > 0) {
	    vlu->progopts.max_val = ustat.n_items;
	    vlu->progopts.start_val = 1;
	}

    } else {
	vlu->progopts.max_val = ntohl(cheader->vital_header.eofPtr);
	vlu->progopts.start_val = sizeof(*cheader);
    }

    return 0;

 error:
    return 1;
}

static int
vl4xload_getaddrs(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db,
		  struct vl4_addrs *addrs)
{
    struct vlupgrade_vl4 *vl4 = db->vl4;
    struct vl_cache *cache = vl4->vl_ctx.cache;

    opr_StaticAssert(sizeof(addrs->hostaddress) ==
		     sizeof(cache->hostaddress));
    memcpy(addrs->hostaddress, cache->hostaddress,
	   sizeof(addrs->hostaddress));

    opr_StaticAssert(sizeof(addrs->ex_addr) ==
		     sizeof(cache->ex_addr));
    memcpy(addrs->ex_addr, cache->ex_addr, sizeof(addrs->ex_addr));

    if (db->vltype->vlt_kv) {
	int base;
	for (base = 0; base < VL_MAX_ADDREXTBLKS; base++) {
	    if (cache->ex_addr[base] != NULL) {
		opr_progress_add(vlu->progress, 1);
	    }
	}
    }

    return 0;
}

static int
vl4xload_getvol(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db,
		struct nvlentry *avol, int *a_eof)
{
    struct vlupgrade_vl4 *vl4 = db->vl4;
    struct vl_ctx *vl_ctx = &vl4->vl_ctx;
    afs_int32 remain = 0;

    db->vol_curpos = NextEntry(vl_ctx, db->vol_curpos, avol, &remain);
    if (db->vol_curpos == 0) {
	if (remain == 0) {
	    *a_eof = 1;
	    return 0;
	}
	fprintf(stderr, "Failed to load next volume from database\n");
	goto error;
    }

    if (db->vltype->vlt_kv) {
	afs_int32 voltype;
	/*
	 * In vldb4-kv, we have one record for the volume itself (keyed by the
	 * RW id), one record for the volume name, and one record for each
	 * non-RW volid. So count 2 records for the volume, plus however many
	 * non-RW volids we have.
	 */
	opr_progress_add(vlu->progress, 2);
	for (voltype = ROVOL; voltype <= BACKVOL; voltype++) {
	    if (avol->volumeId[voltype] != 0) {
		opr_progress_add(vlu->progress, 1);
	    }
	}
    } else {
	opr_progress_set(vlu->progress, db->vol_curpos);
    }

    return 0;

 error:
    return 1;
}

static int
vl4xdump_init(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db)
{
    struct vlupgrade_vl4 *vl4;
    struct vl_ctx *vl_ctx;
    struct vl_cache *cache;
    struct vlheader *cheader;
    afs_uint32 vlvers;

    vl4_init(db);
    vl4 = db->vl4;
    vl_ctx = &vl4->vl_ctx;
    cache = vl_ctx->cache;
    cheader = &cache->cheader;

    if (db->vltype->vlt_kv) {
	vlvers = VLDBVERSION_4_KV;
    } else {
	vlvers = VLDBVERSION_4;
    }

    cheader->vital_header.vldbversion = htonl(vlvers);
    cheader->vital_header.headersize = htonl(sizeof(*cheader));
    cheader->vital_header.MaxVolumeId = htonl(vlu->src_db.maxvolid);
    cheader->vital_header.eofPtr = htonl(sizeof(*cheader));

    cache->vldbversion = vlvers;
    cache->maxnservers = 13;

    return 0;
}

static int
vl4xdump_dumpaddrs(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db, struct vl4_addrs *addrs)
{
    int code;
    int base;
    int addr_i;
    struct vlupgrade_vl4 *vl4 = db->vl4;
    struct vl_ctx *vl_ctx = &vl4->vl_ctx;
    struct vl_cache *cache = vl_ctx->cache;
    struct vlheader *cheader = &cache->cheader;

    opr_StaticAssert(sizeof(addrs->hostaddress) ==
		     sizeof(cache->hostaddress));
    memcpy(cache->hostaddress, addrs->hostaddress,
	   sizeof(addrs->hostaddress));

    opr_StaticAssert(sizeof(cache->hostaddress) ==
		     sizeof(cache->hostaddress[0]) * (MAXSERVERID+1));
    opr_StaticAssert(sizeof(cheader->IpMappedAddr) ==
		     sizeof(cheader->IpMappedAddr[0]) * (MAXSERVERID+1));
    opr_StaticAssert(sizeof(cache->hostaddress) ==
		     sizeof(cheader->IpMappedAddr));
    for (addr_i = 0; addr_i < MAXSERVERID + 1; addr_i++) {
	cheader->IpMappedAddr[addr_i] = htonl(cache->hostaddress[addr_i]);
    }

    code = vlexcpy(cache->ex_addr, addrs->ex_addr);
    if (code != 0) {
	print_error(code, "Failed to copy ex_addr blocks");
	goto error;
    }

    /* Alloc blocks for each ex_addr. */
    for (base = 0; base < VL_MAX_ADDREXTBLKS; base++) {
	afs_int32 blockindex;

	if (cache->ex_addr[base] == NULL) {
	    continue;
	}

	code = vlgrow_eofPtr(cheader, VL_ADDREXTBLK_SIZE, &blockindex);
	if (code != 0) {
	    print_error(code, "Failed to alloc ex_addr block");
	    goto error;
	}

	if (base == 0) {
	    cache->cheader.SIT = htonl(blockindex);
	}
	cache->ex_addr[0]->ex_contaddrs[base] = htonl(blockindex);
    }

    /* Write out the ex_addr blocks. */
    for (base = 0; base < VL_MAX_ADDREXTBLKS; base++) {
	afs_int32 blockindex;

	if (cache->ex_addr[base] == NULL) {
	    continue;
	}

	blockindex = ntohl(cache->ex_addr[0]->ex_contaddrs[base]);
	opr_Assert(blockindex > 0);

	code = vlwrite_exblock(vl_ctx, base,
			       cache->ex_addr[base], blockindex,
			       cache->ex_addr[base],
			       VL_ADDREXTBLK_SIZE);
	if (code != 0) {
	    print_error(code, "Failed to write ex_addr block");
	    goto error;
	}
    }

    return 0;

 error:
    return 1;
}

static int
vl4xdump_writevol(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db,
		  struct nvlentry *vol)
{
    int code;
    afs_int32 blockindex;
    struct vlupgrade_vl4 *vl4 = db->vl4;
    struct vl_ctx *vl_ctx = &vl4->vl_ctx;
    struct nvlentry spare;

    blockindex = AllocBlock(vl_ctx, &spare);
    if (blockindex == 0) {
	fprintf(stderr, "Failed to alloc block for volume\n");
	goto error;
    }

    code = ThreadVLentry(vl_ctx, blockindex, vol);
    if (code != 0) {
	print_error(code, "Failed to write vlentry for %s (%d)", vol->name,
		    vol->volumeId[0]);
	goto error;
    }

    return 0;

 error:
    return 1;
}

static int
vl4xdump_finish(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db)
{
    int code;
    struct vlupgrade_vl4 *vl4;
    struct vl_ctx *vl_ctx;
    struct vl_cache *cache;
    struct vlheader *cheader;

    vl4 = db->vl4;
    vl_ctx = &vl4->vl_ctx;
    cache = vl_ctx->cache;
    cheader = &cache->cheader;

    /* Now we actually want to write the cheader. */
    vl_ctx->cheader_nowrite = 0;

    code = vlwrite_cheader(vl_ctx, cheader, cheader, sizeof(*cheader));
    if (code != 0) {
	print_error(code, "Failed to write vldb header to %s", db->path);
	goto error;
    }

    return 0;

 error:
    return 1;
}

static struct vlloader_ops vl4x_lops = {
    .vlo_init = vl4xload_init,
    .vlo_getaddrs = vl4xload_getaddrs,
    .vlo_getvol = vl4xload_getvol,
};

static struct vldumper_ops vl4x_dops = {
    .vdo_init = vl4xdump_init,
    .vdo_dumpaddrs = vl4xdump_dumpaddrs,
    .vdo_writevol = vl4xdump_writevol,
    .vdo_finish = vl4xdump_finish,
};

static int
db_create(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db)
{
    int code;
    struct ubik_rawinit_opts ropts;

    memset(&ropts, 0, sizeof(ropts));

    ropts.r_rw = 1;
    if (db->vltype->vlt_kv) {
	ropts.r_create_kv = 1;
    } else {
	ropts.r_create_flat = 1;
    }

    code = ubik_RawInit(db->path, &ropts, &db->udbase);
    if (code != 0) {
	print_error(code, "Failed to create db %s", db->path);
	goto error;
    }

    code = ubik_BeginTrans(db->udbase, UBIK_WRITETRANS, &db->utrans);
    if (code != 0) {
	print_error(code, "Failed to start write trans for %s", db->path);
	goto error;
    }

    code = ubik_SetLock(db->utrans, 1, 1, LOCKWRITE);
    if (code != 0) {
	print_error(code, "Failed to write-lock db %s", db->path);
	goto error;
    }

    return 0;

 error:
    return 1;
}

static void
db_close(struct vlupgrade_db *db)
{
    if (db->utrans) {
	ubik_AbortTrans(db->utrans);
	db->utrans = NULL;
    }
    ubik_RawClose(&db->udbase);
}

static int
db_commit(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db)
{
    int code;

    code = ubik_RawSetVersion(db->utrans, &db->uversion);
    if (code != 0) {
	print_error(code, "Failed to set version for %s", db->path);
	goto error;
    }

    code = ubik_EndTrans(db->utrans);
    db->utrans = NULL;
    if (code != 0) {
	print_error(code, "Failed to commit trans to %s", db->path);
	goto error;
    }

    code = 0;

 done:
    db_close(db);
    return code;

 error:
    code = 1;
    goto done;
}

static int
open_src_db(struct vlupgrade_ctx *vlu, struct vlupgrade_db *db)
{
    int code;

    code = ubik_RawInit(db->path, NULL, &db->udbase);
    if (code != 0) {
	print_error(code, "Failed to open %s", db->path);
	goto error;
    }

    code = ubik_BeginTransReadAny(db->udbase, UBIK_READTRANS, &db->utrans);
    if (code != 0) {
	print_error(code, "Failed to start read trans on %s", db->path);
	goto error;
    }

    code = ubik_SetLock(db->utrans, 1, 1, LOCKREAD);
    if (code != 0) {
	print_error(code, "Failed to read lock %s", db->path);
	goto error;
    }

    code = ubik_RawGetVersion(db->utrans, &db->uversion);
    if (code != 0) {
	print_error(code, "Failed to get ubik label from %s", db->path);
	goto error;
    }

    if (db->uversion.epoch == 0 || db->uversion.counter == 0) {
	fprintf(stderr, "Invalid loading input database ubik version %d.%d\n",
		db->uversion.epoch, db->uversion.counter);
	goto error;
    }

    if (vlu->online) {
	struct ubik_version64 version;
	udb_v32to64(&db->uversion, &version);
	if (udb_vcmp64(&version, &vlu->freeze_version) != 0) {
	    fprintf(stderr, "Error loading input database: ubik version does "
		    "not match version reported by vlserver freeze: "
		    "%lld.%lld != %lld.%lld.\n",
		    version.epoch64.clunks,
		    version.counter64,
		    vlu->freeze_version.epoch64.clunks,
		    vlu->freeze_version.counter64);
	    goto error;
	}
    }

    /*
     * For now, assume a KV database is vldb4-kv, and a flatfile database is
     * vldb4. When we support more formats, this should contain some actual
     * logic for detecting the db format.
     */
    if (ubik_KVDbase(db->udbase)) {
	db->vltype = &vlt_vldb4kv;
    } else {
	db->vltype = &vlt_vldb4;
    }

    return 0;

 error:
    return 1;
}

static int
upgrade_addrs(struct vlupgrade_ctx *vlu)
{
    struct vlupgrade_db *src_db = &vlu->src_db;
    struct vlupgrade_db *dest_db = &vlu->dest_db;
    struct vlloader_ops *loader = src_db->vltype->vlt_lops;
    struct vldumper_ops *dumper = dest_db->vltype->vlt_dops;
    struct vl4_addrs addrs;
    int code;

    memset(&addrs, 0, sizeof(addrs));

    print_nq(vlu, "Converting fileserver entries... ");

    /*
     * Convert extents/addrs. For now, this is all very vldb4-specific, since
     * vldb4 and vldb4-kv use the same structures. When we want to convert to a
     * non-vldb4-based format, this should probably be replaced by logic that
     * loads/dumps individual servers.
     */
    code = (*loader->vlo_getaddrs)(vlu, src_db, &addrs);
    if (code != 0) {
	goto error;
    }

    code = (*dumper->vdo_dumpaddrs)(vlu, dest_db, &addrs);
    if (code != 0) {
	goto error;
    }

    print_nq(vlu, "done.\n");

    return 0;

 error:
    return 1;
}

static int
upgrade_vols(struct vlupgrade_ctx *vlu)
{
    struct vlupgrade_db *src_db = &vlu->src_db;
    struct vlupgrade_db *dest_db = &vlu->dest_db;
    struct vlloader_ops *loader = src_db->vltype->vlt_lops;
    struct vldumper_ops *dumper = dest_db->vltype->vlt_dops;
    int code = 0;

    vlu->progress = opr_progress_start(&vlu->progopts, "Converting volumes");

    for (;;) {
	int eof = 0;
	struct nvlentry vol;

	memset(&vol, 0, sizeof(vol));

	code = (*loader->vlo_getvol)(vlu, src_db, &vol, &eof);
	if (code != 0) {
	    fprintf(stderr, "Error loading volume\n");
	    goto done;
	}
	if (eof) {
	    break;
	}

	code = (*dumper->vdo_writevol)(vlu, dest_db, &vol);
	if (code != 0) {
	    fprintf(stderr, "Error writing volume '%s' (%d)\n", vol.name,
		    vol.volumeId[RWVOL]);
	    goto done;
	}
    }

 done:
    opr_progress_done(&vlu->progress, code);
    return code;
}

static int
online_start(struct vlupgrade_ctx *vlu)
{
    int code;
    struct ubik_freezeinit_opts fopts;
    afs_uint64 freezeid = 0;

    memset(&fopts, 0, sizeof(fopts));

    opr_Assert(vlu->dest_path == NULL);

    fopts.fi_cinfo = &vlu->ctl_cinfo;
    fopts.fi_needrw = 1;

    code = ubik_FreezeInit(&fopts, &vlu->freeze);
    if (code != 0) {
	print_error(code, "Failed to initialize freeze");
	goto error;
    }

    print_nq(vlu, "Freezing VLDB... ");

    code = ubik_FreezeBegin(vlu->freeze, &freezeid, &vlu->freeze_version,
			    &vlu->src_path);
    if (code != 0) {
	print_error(code, "Failed to start freeze");
	goto error;
    }

    if (vlu->backup_suffix != NULL) {
	if (asprintf(&vlu->backup_path, "%s%s", vlu->src_path,
		     vlu->backup_suffix) < 0) {
	    print_error(code, NULL);
	    goto error;
	}

	if (access(vlu->backup_path, F_OK) == 0) {
	    print_error(EEXIST, "%s", vlu->backup_path);
	    goto error;
	}
    }

    if (asprintf(&vlu->dest_suffix, ".CONV.%lld", (long long)time(NULL)) < 0) {
	print_error(errno, NULL);
	vlu->dest_suffix = NULL;
	goto error;
    }
    if (asprintf(&vlu->dest_path, "%s%s", vlu->src_path, vlu->dest_suffix) < 0) {
	print_error(errno, NULL);
	vlu->dest_path = NULL;
	goto error;
    }

    print_nq(vlu, "done (freezeid %llu).\n", freezeid);

    return 0;

 error:
    return 1;
}

static int
online_commit(struct vlupgrade_ctx *vlu)
{
    int code;
    struct opr_progress *progress = NULL;
    struct opr_progress_opts progopts = vlu->progopts;

    print_nq(vlu, "Installing %s to ubik... ", vlu->dest_db.path);

    code = ubik_FreezeInstall(vlu->freeze, vlu->dest_db.path,
			      vlu->backup_suffix);
    if (code != 0) {
	print_error(code, "Failed to install upgraded db");
	goto error;
    }

    print_nq(vlu, "done.\n");

    if (!vlu->no_dist) {
	progopts.bkg_spinner = 1;
	progress = opr_progress_start(&progopts, "Distributing new database");
	code = ubik_FreezeDistribute(vlu->freeze);
	opr_progress_done(&progress, code);

	if (code != 0) {
	    print_error(code, "Failed to distribute db");
	    if (vlu->need_dist) {
		goto error;
	    }

	    fprintf(stderr, "warning: We failed to distribute the new database "
		    "to other ubik sites, but the\n");
	    fprintf(stderr, "warning: database was installed successfully. Ubik "
		    "itself may distribute the db\n");
	    fprintf(stderr, "warning: on its own in the background.\n");
	    fprintf(stderr, "\n");
	}
    }

    print_nq(vlu, "Unfreezing VLDB... ");

    code = ubik_FreezeEnd(vlu->freeze, NULL);
    if (code != 0) {
	print_error(code, "Failed to unfreeze VLDB");
	goto error;
    }

    print_nq(vlu, "done.\n");
    ubik_FreezeDestroy(&vlu->freeze);
    return 0;

 error:
    ubik_FreezeDestroy(&vlu->freeze);
    return 1;
}

static void
online_abort(struct vlupgrade_ctx *vlu)
{
    int code;

    if (vlu->freeze == NULL) {
	return;
    }

    print_nq(vlu, "Aborting VLDB freeze... ");

    code = ubik_FreezeAbort(vlu->freeze, NULL);
    if (code != 0) {
	print_error(code, "Failed to abort freeze");
	return;
    }

    print_nq(vlu, "done.\n");
}

static int
upgrade_all(struct vlupgrade_ctx *vlu)
{
    struct vlloader_ops *loader;
    struct vldumper_ops *dumper;
    struct vlupgrade_db *src_db;
    struct vlupgrade_db *dest_db;
    afs_uint64 now;
    int code;

    if (vlu->online) {
	code = online_start(vlu);
	if (code != 0) {
	    goto error;
	}
    }

    src_db = &vlu->src_db;
    dest_db = &vlu->dest_db;

    src_db->path = vlu->src_path;
    dest_db->path = vlu->dest_path;
    dest_db->vltype = vlu->dest_vltype;

    /* Open source dbase */
    code = open_src_db(vlu, src_db);
    if (code != 0) {
	fprintf(stderr, "Error opening input database %s\n", vlu->src_path);
	goto error;
    }

    loader = src_db->vltype->vlt_lops;
    dumper = dest_db->vltype->vlt_dops;

    if (src_db->vltype == dest_db->vltype) {
	if (vlu->force_type) {
	    fprintf(stderr, "%s is already a %s database. Upgrading anyway, "
		    "due to -force-type.\n",
		    src_db->path, src_db->vltype->vlt_name);
	} else {
	    fprintf(stderr, "%s is already a %s database. Use -force-type to "
		    "ignore, or specify a different db type with -to.\n",
		    src_db->path, src_db->vltype->vlt_name);
	    goto error;
	}
    }

    code = (*loader->vlo_init)(vlu, src_db);
    if (code != 0) {
	fprintf(stderr, "Error initializing input database\n");
	goto error;
    }

    now = time(NULL);

    if (src_db->uversion.epoch >= now) {
	if (vlu->ignore_epoch) {
	    fprintf(stderr, "Warning: input database does not look older than "
		    "the current time (%d >= %llu).\n",
		    src_db->uversion.epoch, now);
	    fprintf(stderr, "Creating new db based on the current time anyway, "
		    "due to -ignore-epoch.\n");

	} else {
	    fprintf(stderr, "Error: input database does not look older than "
		    "the current time (%d >= %llu). (Use -ignore-epoch to ignore.)\n",
		    src_db->uversion.epoch, now);
	    goto error;
	}
    }

    /*
     * Set the version of our new database to have a higher epoch than the
     * original, to make sure it is distinguishable from the original db. (We
     * don't just increment the counter, to try to avoid collisions if the
     * original db gets updated separately, etc.)
     *
     * But we don't simply set the epoch to time(NULL), since that causes some
     * delays for -online upgrades, due to some safety checks in ubik. So
     * instead, just increment the epoch by one; if that causes us to go in the
     * "future", the ubik freezing code will handle the necessary sleeps etc to
     * make it so the epoch appears in the past.
     */
    dest_db->uversion.epoch = src_db->uversion.epoch + 1;
    dest_db->uversion.counter = 1;

    code = db_create(vlu, dest_db);
    if (code != 0) {
	goto error;
    }

    print_nq(vlu, "Converting %s (%s) -> %s (%s)\n",
	     src_db->path, src_db->vltype->vlt_name,
	     dest_db->path, dest_db->vltype->vlt_name);

    code = (*dumper->vdo_init)(vlu, dest_db);
    if (code != 0) {
	fprintf(stderr, "Error initializing destination database\n");
	goto error;
    }

    code = upgrade_addrs(vlu);
    if (code != 0) {
	goto error;
    }

    code = upgrade_vols(vlu);
    if (code != 0) {
	goto error;
    }

    print_nq(vlu, "Committing changes... ");

    db_close(src_db);

    code = (*dumper->vdo_finish)(vlu, dest_db);
    if (code != 0) {
	goto error;
    }

    code = db_commit(vlu, dest_db);
    if (code != 0) {
	goto error;
    }

    print_nq(vlu, "done.\n");

    if (vlu->online) {
	code = online_commit(vlu);
	if (code != 0) {
	    goto error;
	}
    }

    if (vlu->online) {
	print_nq(vlu, "\nConverted %s from %s to %s (%d.%d -> %d.%d)\n",
		 src_db->path, src_db->vltype->vlt_name,
		 dest_db->vltype->vlt_name,
		 src_db->uversion.epoch,
		 src_db->uversion.counter,
		 dest_db->uversion.epoch,
		 dest_db->uversion.counter);
	if (vlu->backup_suffix != NULL) {
	    print_nq(vlu, "Backup saved in %s\n", vlu->backup_path);
	}

    } else {
	print_nq(vlu, "\nConverted %s (%s, %d.%d) -> %s (%s, %d.%d)\n",
		 src_db->path, src_db->vltype->vlt_name,
		 src_db->uversion.epoch, src_db->uversion.counter,
		 dest_db->path, dest_db->vltype->vlt_name,
		 dest_db->uversion.epoch, dest_db->uversion.counter);
    }

    db_close(src_db);
    db_close(dest_db);
    ubik_FreezeDestroy(&vlu->freeze);

    return 0;

 error:
    db_close(src_db);
    db_close(dest_db);
    online_abort(vlu);
    ubik_FreezeDestroy(&vlu->freeze);
    return 1;
}

static int
handleit(struct cmd_syndesc *as, void *arock)
{
    char *fmt_str = NULL;
    struct vlupgrade_ctx vlu_s;
    struct vlupgrade_ctx *vlu = &vlu_s;
    int no_backup = 0;
    char *dist = NULL;

    memset(vlu, 0, sizeof(*vlu));

    vlu->dest_vltype = &vlt_vldb4kv;
    vlu->ctl_cinfo.server_type = "vlserver";

    if (cmd_OptionAsString(as, OPT_to, &fmt_str) == 0) {
	if (strcmp(fmt_str, "vldb4") == 0) {
	    vlu->dest_vltype = &vlt_vldb4;
	} else if (strcmp(fmt_str, "vldb4-kv") == 0) {
	    vlu->dest_vltype = &vlt_vldb4kv;
	} else {
	    fprintf(stderr, "%s: Unrecognized format '%s' given to -to.\n",
		    getprogname(), fmt_str);
	    return 1;
	}
	free(fmt_str);
	fmt_str = NULL;
    }

    cmd_OptionAsString(as, OPT_input, &vlu->src_path);
    cmd_OptionAsString(as, OPT_output, &vlu->dest_path);
    cmd_OptionAsFlag(as, OPT_quiet, &vlu->quiet);
    cmd_OptionAsFlag(as, OPT_progress, &vlu->progopts.force_enable);
    cmd_OptionAsFlag(as, OPT_no_progress, &vlu->progopts.force_disable);
    cmd_OptionAsFlag(as, OPT_ignore_epoch, &vlu->ignore_epoch);
    cmd_OptionAsFlag(as, OPT_force_type, &vlu->force_type);

    cmd_OptionAsFlag(as, OPT_online, &vlu->online);
    cmd_OptionAsString(as, OPT_backup_suffix, &vlu->backup_suffix);
    cmd_OptionAsFlag(as, OPT_no_backup, &no_backup);
    cmd_OptionAsString(as, OPT_dist, &dist);
    cmd_OptionAsString(as, OPT_ctl_socket, &vlu->ctl_cinfo.sock_path);

    vlu->progopts.quiet = vlu->quiet;

#ifndef AFS_CTL_ENV
    if (vlu->online) {
	fprintf(stderr, "%s: -online not supported: we were not built with "
			"afsctl support\n", getprogname());
	return 1;
    }
#endif

    if (vlu->online) {
	if (vlu->src_path != NULL || vlu->dest_path != NULL) {
	    fprintf(stderr, "%s: -online cannot be given with -input/-output.\n",
		    getprogname());
	    return 1;
	}

	if (vlu->backup_suffix == NULL && !no_backup) {
	    fprintf(stderr, "%s: With -online, you must specify either "
		    "-backup-suffix or -no-backup.\n", getprogname());
	    return 1;
	}
	if (no_backup) {
	    free(vlu->backup_suffix);
	    vlu->backup_suffix = NULL;
	}

	if (dist == NULL || strcmp(dist, "try") == 0) {
	    /* noop; this is the default */
	} else if (strcmp(dist, "skip") == 0) {
	    vlu->no_dist = 1;
	} else if (strcmp(dist, "required") == 0) {
	    vlu->need_dist = 1;
	} else {
	    print_error(0, "Bad value for -dist: %s", dist);
	    return 1;
	}

    } else {
	char *badopt = NULL;

	if (vlu->src_path == NULL || vlu->dest_path == NULL) {
	    fprintf(stderr, "%s: You must specify either -online, or -input and "
		    "-output.\n", getprogname());
	    return 1;
	}

	if (vlu->backup_suffix != NULL) {
	    badopt = "-backup-suffix";
	} else if (no_backup) {
	    badopt = "-no-backup";
	} else if (vlu->ctl_cinfo.sock_path != NULL) {
	    badopt = "-ctl-socket";
	} else if (dist != NULL) {
	    badopt = "-dist";
	}
	if (badopt != NULL) {
	    fprintf(stderr, "%s: %s can only be given with -online\n",
		    getprogname(), badopt);
	    return 1;
	}
    }

    if (cmd_OptionPresent(as, OPT_progress) && cmd_OptionPresent(as, OPT_no_progress)) {
	fprintf(stderr, "%s: You cannot specify both -progress and "
		"-no-progress.\n", getprogname());
	return 1;
    }

    return upgrade_all(vlu);
}

int
main(int argc, char **argv)
{
    struct cmd_syndesc *ts;

    setprogname(argv[0]);

    initialize_ACFG_error_table();
    initialize_CMD_error_table();
    initialize_VL_error_table();
    initialize_U_error_table();

    ts = cmd_CreateSyntax(NULL, handleit, NULL, 0,
			  "Convert a vldb between database formats");
    cmd_AddParmAtOffset(ts, OPT_input, "-input", CMD_SINGLE, CMD_OPTIONAL,
			"input vldb path");
    cmd_AddParmAlias(ts, OPT_input, "-i");
    cmd_AddParmAtOffset(ts, OPT_output, "-output", CMD_SINGLE, CMD_OPTIONAL,
			"output vldb path");
    cmd_AddParmAlias(ts, OPT_output, "-o");
    cmd_AddParmAtOffset(ts, OPT_to, "-to", CMD_SINGLE, CMD_OPTIONAL,
			"output format type (vldb4 | vldb4-kv)");
    cmd_AddParmAtOffset(ts, OPT_ignore_epoch, "-ignore-epoch", CMD_FLAG, CMD_OPTIONAL,
			"Skip epoch check for input VLDB");
    cmd_AddParmAtOffset(ts, OPT_force_type, "-force-type", CMD_FLAG, CMD_OPTIONAL,
			"Create output db even if it is the same type as the input");

    cmd_AddParmAtOffset(ts, OPT_online, "-online", CMD_FLAG, CMD_OPTIONAL,
			"Perform an online upgrade with a running vlserver");
    cmd_AddParmAtOffset(ts, OPT_backup_suffix, "-backup-suffix", CMD_SINGLE,
			CMD_OPTIONAL,
			"backup VLDB suffix (for -online)");
    cmd_AddParmAtOffset(ts, OPT_no_backup, "-no-backup", CMD_FLAG, CMD_OPTIONAL,
			"Do not generate backup VLDB (for -online)");
    cmd_AddParmAtOffset(ts, OPT_dist, "-dist", CMD_SINGLE, CMD_OPTIONAL,
			"try | skip | required");
    cmd_AddParmAtOffset(ts, OPT_ctl_socket, "-ctl-socket", CMD_SINGLE,
			CMD_OPTIONAL,
			"path to ctl unix socket (for -online)");

    cmd_AddParmAtOffset(ts, OPT_quiet, "-quiet", CMD_FLAG, CMD_OPTIONAL,
			"Talk less");
    cmd_AddParmAtOffset(ts, OPT_progress, "-progress", CMD_FLAG, CMD_OPTIONAL,
			"Enable progress reporting");
    cmd_AddParmAtOffset(ts, OPT_no_progress, "-no-progress", CMD_FLAG, CMD_OPTIONAL,
			"Disable progress reporting");

    if (argc == 1) {
	/*
	 * If the user hasn't given us _any_ arguments, make libcmd print out
	 * its normal help message. Normally libcmd will not do this, since
	 * none of our arguments are required on their own (you must specify
	 * either -online or -input, but neither are always required), so it
	 * thinks it's okay for the user to specify no arguments.
	 *
	 * To work around this, add a single required arg if we can see that
	 * the user didn't specify any arguments at all. It's a hack, but it
	 * works.
	 */
	cmd_AddParm(ts, "-dummy", CMD_SINGLE, 0, "dummy arg");
    }

    return cmd_Dispatch(argc, argv);
}

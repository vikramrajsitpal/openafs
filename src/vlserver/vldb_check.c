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

#ifdef AFS_NT40_ENV
#include <WINNT/afsevent.h>
#endif

#include <afs/afsutil.h>
#include <afs/cmd.h>
#include <afs/opr.h>
#include <afs/okv.h>

#include "ubik_internal.h"
#include "vlserver_internal.h"
#include "vldbint.h"

/* Read a VLDB file and verify it for correctness */

#define VL  0x001		/* good volume entry */
#define FR  0x002		/* free volume entry */
#define MH  0x004		/* multi-homed entry */

#define RWH 0x010		/* on rw hash chain */
#define ROH 0x020		/* on ro hash chain */
#define BKH 0x040		/* on bk hash chain */
#define NH  0x080		/* on name hash chain */

#define MHC 0x100		/* on multihomed chain */
#define FRC 0x200		/* on free chain */

#define REFRW 0x1000            /* linked from something (RW) */
#define REFRO 0x2000            /* linked from something (RO) */
#define REFBK 0x4000            /* linked from something (BK) */
#define REFN  0x8000            /* linked from something (name) */

#define MULTRW 0x10000         /* multiply-chained (RW) */
#define MULTRO 0x20000         /* multiply-chained (RO) */
#define MULTBK 0x40000         /* multiply-chained (BK) */
#define MULTN  0x80000         /* multiply-chained (name) */

#define MISRWH 0x100000          /* mischained (RW) */
#define MISROH 0x200000          /* mischained (RO) */
#define MISBKH 0x400000          /* mischained (BK) */
#define MISNH  0x800000          /* mischained (name) */

#define VLDB_CHECK_NO_VLDB_CHECK_ERROR 0
#define VLDB_CHECK_WARNING  1
#define VLDB_CHECK_ERROR    2
#define VLDB_CHECK_FATAL    4
#define vldbread(x,y,z) vldbio(x,y,z,0)
#define vldbwrite(x,y,z) vldbio(x,y,z,1)

#define HDRSIZE 64
#define ADDR(x) ((x)/sizeof(struct nvlentry))
#define OFFSET(addr) ((addr) + HDRSIZE)

static int fd = -1;
static int listentries, listservers, listheader, listuheader, verbose, quiet;
static struct okv_dbhandle *dbh;
static struct okv_trans *kvtx;

static int fix = 0;
/* if quiet, don't send anything to stdout */
static int quiet = 0;
/*  error level. 0 = no error, 1 = warning, 2 = error, 4 = fatal */
static int error_level  = 0;

static struct er {
    afs_int64 addr;
    int type;
} *record;
static afs_int64 maxentries;
static int serveraddrs[MAXSERVERID + 2];
static u_char serverxref[MAXSERVERID + 2];  /**< to resolve cross-linked mh entries */
static int serverref[MAXSERVERID + 2];      /**< which addrs are referenced by vl entries */

static struct mhinfo {
    afs_int64 addr;			/**< vldb file record */
    char orphan[VL_MHSRV_PERBLK];	/**< unreferenced mh enties */
} mhinfo[VL_MAX_ADDREXTBLKS];

/* Context for 'nextentry()'; acts like an iterator iterating over all of the
 * entries in the database. */
struct nextinfo {
    int valid;		/**< is this struct valid? (that is, has it been
			 *   initialized) */
    afs_int32 type;	/**< type of the current entry (e.g. VL, MH) */
    afs_int32 rindex;	/**< index of the current entry */
    afs_int64 addr;	/**< the database "address" of the current entry */
    struct rx_opaque keybuf;
			/**< for vldb4-kv: the key of the current entry */
};

static_inline void
opaque_set(struct rx_opaque *buf, void *val, size_t len)
{
    buf->len = len;
    buf->val = val;
}

static_inline void
opaque_copy(struct rx_opaque *buf, void *dest, size_t destsize)
{
    opr_Assert(buf->val != NULL);
    opr_Assert(buf->len >= destsize);
    memcpy(dest, buf->val, destsize);
}

/*  Used to control what goes to stdout based on quiet flag */
static void
AFS_ATTRIBUTE_FORMAT(__printf__, 1, 2)
quiet_println(const char *fmt,...)
{
    va_list args;
    if (!quiet) {
        va_start(args, fmt);
        vfprintf(stdout, fmt, args);
        va_end(args);
    }
}

/*  Used to set the error level and ship messages to stderr */
static void
AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3)
log_error(int eval, const char *fmt, ...)
{
    va_list args;
    if (error_level < eval) error_level  = eval ;  /*  bump up the severity */
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (error_level  == VLDB_CHECK_FATAL) exit(VLDB_CHECK_FATAL);
}

/* Are we operating on a kv database? */
static_inline int
is_kv(void)
{
    if (kvtx == NULL) {
	return 0;
    }
    return 1;
}

static int
readUbikHeader(void)
{
    int offset, r;
    struct ubik_hdr uheader;
    int code;

    memset(&uheader, 0, sizeof(uheader));

    if (!is_kv()) {
	offset = lseek(fd, 0, 0);
	if (offset != 0) {
	    log_error(VLDB_CHECK_FATAL, "error: lseek to 0 failed: %d %d\n",
		      offset, errno);
	    return (VLDB_CHECK_FATAL);
	}

	/* now read the info */
	r = read(fd, &uheader, sizeof(uheader));
	if (r != sizeof(uheader)) {
	    log_error(VLDB_CHECK_FATAL,
		      "error: read of %d bytes failed: %d %d\n",
		      (int)sizeof(uheader), r, errno);
	    return (VLDB_CHECK_FATAL);
	}

	uheader.magic = ntohl(uheader.magic);
	uheader.size = ntohs(uheader.size);
	uheader.version.epoch = ntohl(uheader.version.epoch);
	uheader.version.counter = ntohl(uheader.version.counter);

    } else {
	code = ukv_getlabel(kvtx, &uheader.version);
	if (code != 0) {
	    log_error(VLDB_CHECK_FATAL, "error: ukv_getlabel failed: %d\n",
		      code);
	    return VLDB_CHECK_FATAL;
	}

    }

    if (listuheader) {
	quiet_println("Ubik Header\n");
	if (!is_kv()) {
	    quiet_println("   Magic           = 0x%x\n", uheader.magic);
	    quiet_println("   Size            = %u\n", uheader.size);
	}
	quiet_println("   Version.epoch   = %u\n", uheader.version.epoch);
	quiet_println("   Version.counter = %u\n", uheader.version.counter);
    }

    if (!is_kv()) {
	if (uheader.size != HDRSIZE)
	    log_error(VLDB_CHECK_WARNING, "VLDB_CHECK_WARNING: Ubik header size is "
		      "%u (should be %u)\n", uheader.size, HDRSIZE);
	if (uheader.magic != UBIK_MAGIC)
	    log_error(VLDB_CHECK_ERROR, "Ubik header magic is 0x%x (should be 0x%x)\n",
		      uheader.magic, UBIK_MAGIC);
    }

    return (0);
}

static int
vldbio(afs_uint64 position, void *buffer, int size, int rdwr)
{
    int offset, r, p;

    opr_Assert(fd >= 0);

    /* seek to the correct spot. skip ubik stuff */
    p = OFFSET(position);
    offset = lseek(fd, p, 0);
    if (offset != p) {
	log_error(VLDB_CHECK_FATAL, "error: lseek to %d failed: %d %d\n", p,
		  offset, errno);
	return (-1);
    }

    if (rdwr == 1)
	r = write(fd, buffer, size);
    else
	r = read(fd, buffer, size);

    if (r != size) {
	log_error(VLDB_CHECK_FATAL, "error: %s of %d bytes failed: %d %d\n",
		  rdwr == 1 ? "write" : "read",
		  size, r, errno);
	return (-1);
    }
    return (0);
}

static void
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

static void
kv2cheader(struct vlheader_kv *cheader_kv, struct vlheader *cheader)
{
    opr_StaticAssert(sizeof(cheader->IpMappedAddr) == sizeof(cheader_kv->IpMappedAddr));

    memset(cheader, 0, sizeof(*cheader));

    cheader->vital_header = cheader_kv->vital_header;
    memcpy(cheader->IpMappedAddr, cheader_kv->IpMappedAddr,
	   sizeof(cheader->IpMappedAddr));
    cheader->SIT = cheader_kv->SIT;
}

static int
vldbread_cheader(struct vlheader *headerp)
{
    afs_uint32 ckey = htonl(VL4KV_KEY_CHEADERKV);
    struct vlheader_kv cheader_kv;
    struct rx_opaque keybuf;
    int code;

    if (!is_kv()) {
	return vldbread(0, headerp, sizeof(*headerp));
    }

    opaque_set(&keybuf, &ckey, sizeof(ckey));
    code = okv_get_copy(kvtx, &keybuf, &cheader_kv,
			sizeof(cheader_kv), NULL);
    if (code != 0) {
	log_error(VLDB_CHECK_FATAL, "error: can't get vldb4-kv header: %d\n",
		  code);
	return -1;
    }

    kv2cheader(&cheader_kv, headerp);
    return 0;
}

static int
vldbwrite_cheader(struct vlheader *headerp)
{
    afs_uint32 ckey = htonl(VL4KV_KEY_CHEADERKV);
    struct vlheader_kv cheader_kv;
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;
    int code;

    if (!is_kv()) {
	return vldbwrite(0, headerp, sizeof(*headerp));
    }

    cheader2kv(headerp, &cheader_kv);

    opaque_set(&keybuf, &ckey, sizeof(ckey));
    opaque_set(&valbuf, &cheader_kv, sizeof(cheader_kv));

    code = okv_put(kvtx, &keybuf, &valbuf, OKV_PUT_REPLACE);
    if (code != 0) {
	log_error(VLDB_CHECK_FATAL, "error: can't put vldb4-kv header: %d\n",
		  code);
	return -1;
    }
    return 0;
}

static void
key2addr(struct rx_opaque *key, afs_int64 *addr)
{
    /*
     * Translate a 64-bit KV key into an "address", used in various code paths
     * in vldb_check. We treat the key as two net-byte-order 32-bit integers,
     * which we then stuff into the 64-bit address. This allows keys that are
     * logically e.g. 0x12345678 to be printed by vldb_check as 0x12345678.
     */
    afs_uint32 vals[2];

    opr_StaticAssert(sizeof(vals) == sizeof(*addr));
    opr_Assert(key->len == sizeof(*addr));

    opaque_copy(key, vals, sizeof(vals));
    vals[0] = ntohl(vals[0]);
    vals[1] = ntohl(vals[1]);

    *addr = ((afs_uint64)vals[0] << 32) | vals[1];
}

static void
addr2key(afs_uint64 *addr, struct rx_opaque *key)
{
    afs_uint32 vals[2];

    vals[0] = htonl(*addr >> 32);
    vals[1] = htonl(*addr & 0xffffffff);

    opr_StaticAssert(sizeof(*addr) == sizeof(vals));
    memcpy(addr, vals, sizeof(vals));

    opaque_set(key, addr, sizeof(*addr));
}

static int
vldbread_exblock(afs_int64 addr, struct extentaddr *mhblockP, int base)
{
    struct vl4kv_exkey exkey;
    struct rx_opaque keybuf;
    int code;

    if (!is_kv()) {
	return vldbread(addr, mhblockP, VL_ADDREXTBLK_SIZE);
    }

    memset(&exkey, 0, sizeof(exkey));
    exkey.tag = htonl(VL4KV_KEY_EXBLOCK);
    exkey.base = htonl(base);

    opaque_set(&keybuf, &exkey, sizeof(exkey));
    code = okv_get_copy(kvtx, &keybuf, mhblockP, VL_ADDREXTBLK_SIZE, NULL);
    if (code != 0) {
	log_error(VLDB_CHECK_FATAL, "error: can't get ex block %d: %d\n",
		  base, code);
	return -1;
    }

    return 0;
}

static int
vldbwrite_exblock(afs_int64 addr, struct extentaddr *mhblockP, int base)
{
    struct vl4kv_exkey exkey;
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;
    int code;

    if (!is_kv()) {
	return vldbwrite(addr, mhblockP, VL_ADDREXTBLK_SIZE);
    }

    memset(&exkey, 0, sizeof(exkey));
    exkey.tag = htonl(VL4KV_KEY_EXBLOCK);
    exkey.base = htonl(base);

    opaque_set(&keybuf, &exkey, sizeof(exkey));
    opaque_set(&valbuf, mhblockP, VL_ADDREXTBLK_SIZE);

    code = okv_put(kvtx, &keybuf, &valbuf, OKV_PUT_REPLACE);
    if (code != 0) {
	log_error(VLDB_CHECK_FATAL, "error: can't put ex block %d: %d\n",
		  base, code);
	return -1;
    }

    return 0;
}

static char *
vtype(int type)
{
    static char Type[3];

    if (type == 0)
	strcpy(Type, "rw");
    else if (type == 1)
	strcpy(Type, "ro");
    else if (type == 2)
	strcpy(Type, "bk");
    else
	strcpy(Type, "??");
    return (Type);
}

static afs_int32
NameHash(char *volname)
{
    unsigned int hash;
    char *vchar;

    hash = 0;
    for (vchar = volname + strlen(volname) - 1; vchar >= volname; vchar--)
	hash = (hash * 63) + (*((unsigned char *)vchar) - 63);
    return (hash % HASHSIZE);
}

static afs_int32
IdHash(afs_uint32 volid)
{
    return (volid % HASHSIZE);
}

#define LEGALCHARS ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
static int
InvalidVolname(char *volname)
{
    char *map;
    size_t slen;

    map = LEGALCHARS;
    slen = strlen(volname);
    if (slen >= VL_MAXNAMELEN)
	return 1;
    if (slen == 0)
	return 1;
    return (slen != strspn(volname, map));
}

static int
validVolumeAddr(afs_uint32 fileOffset)
{
    if (ADDR(fileOffset) >= maxentries) {
	/* Are we in range */
	return 0;
    }
    /*
     * We cannot test whether the offset is aligned
     * since the vl entries are not in a regular array
     */
    return 1;
}

static void
readheader(struct vlheader *headerp)
{
    int i, j;
    int actual_size = sizeof(*headerp);
    if (is_kv()) {
	actual_size = sizeof(struct vlheader_kv);
    }

    vldbread_cheader(headerp);

    headerp->vital_header.vldbversion =
	ntohl(headerp->vital_header.vldbversion);
    headerp->vital_header.headersize =
	ntohl(headerp->vital_header.headersize);
    headerp->vital_header.freePtr = ntohl(headerp->vital_header.freePtr);
    headerp->vital_header.eofPtr = ntohl(headerp->vital_header.eofPtr);
    headerp->vital_header.allocs = ntohl(headerp->vital_header.allocs);
    headerp->vital_header.frees = ntohl(headerp->vital_header.frees);
    headerp->vital_header.MaxVolumeId =
	ntohl(headerp->vital_header.MaxVolumeId);
    headerp->vital_header.totalEntries[0] =
	ntohl(headerp->vital_header.totalEntries[0]);
    for (i = 0; i < MAXTYPES; i++)
	headerp->vital_header.totalEntries[i] =
	    ntohl(headerp->vital_header.totalEntries[1]);

    headerp->SIT = ntohl(headerp->SIT);
    for (i = 0; i <= MAXSERVERID; i++)
	headerp->IpMappedAddr[i] = ntohl(headerp->IpMappedAddr[i]);
    for (i = 0; i < HASHSIZE; i++)
	headerp->VolnameHash[i] = ntohl(headerp->VolnameHash[i]);
    for (i = 0; i < MAXTYPES; i++)
	for (j = 0; j < HASHSIZE; j++)
	    headerp->VolidHash[i][j] = ntohl(headerp->VolidHash[i][j]);

    if (listheader) {

	quiet_println("vldb header\n");
	quiet_println("   vldbversion      = %x\n",
	       headerp->vital_header.vldbversion);
	quiet_println("   headersize       = %u [actual=%d]\n",
	       headerp->vital_header.headersize, actual_size);
	quiet_println("   freePtr          = 0x%x\n", headerp->vital_header.freePtr);
	quiet_println("   eofPtr           = %u\n", headerp->vital_header.eofPtr);
	quiet_println("   allocblock calls = %10u\n", headerp->vital_header.allocs);
	quiet_println("   freeblock  calls = %10u\n", headerp->vital_header.frees);
	quiet_println("   MaxVolumeId      = %u\n",
	       headerp->vital_header.MaxVolumeId);
	quiet_println("   rw vol entries   = %u\n",
	       headerp->vital_header.totalEntries[0]);
	quiet_println("   ro vol entries   = %u\n",
	       headerp->vital_header.totalEntries[1]);
	quiet_println("   bk vol entries   = %u\n",
	       headerp->vital_header.totalEntries[2]);
	quiet_println("   multihome info   = 0x%x (%u)\n", headerp->SIT,
	       headerp->SIT);
	quiet_println("   server ip addr   table: size = %d entries\n",
	       MAXSERVERID + 1);
	if (!is_kv()) {
	    quiet_println("   volume name hash table: size = %d buckets\n", HASHSIZE);
	    quiet_println("   volume id   hash table: %d tables with %d buckets each\n",
		   MAXTYPES, HASHSIZE);
	}
    }

    /* Check the header size */
    if (headerp->vital_header.headersize != actual_size)
	log_error(VLDB_CHECK_WARNING,
		  "Header reports its size as %d (should be %d)\n",
		  headerp->vital_header.headersize, actual_size);
    return;
}

static void
writeheader(struct vlheader *headerp)
{
    int i, j;

    headerp->vital_header.vldbversion =
	htonl(headerp->vital_header.vldbversion);
    headerp->vital_header.headersize =
	htonl(headerp->vital_header.headersize);
    headerp->vital_header.freePtr = htonl(headerp->vital_header.freePtr);
    headerp->vital_header.eofPtr = htonl(headerp->vital_header.eofPtr);
    headerp->vital_header.allocs = htonl(headerp->vital_header.allocs);
    headerp->vital_header.frees = htonl(headerp->vital_header.frees);
    headerp->vital_header.MaxVolumeId =
	htonl(headerp->vital_header.MaxVolumeId);
    headerp->vital_header.totalEntries[0] =
	htonl(headerp->vital_header.totalEntries[0]);
    for (i = 0; i < MAXTYPES; i++)
	headerp->vital_header.totalEntries[i] =
	    htonl(headerp->vital_header.totalEntries[1]);

    headerp->SIT = htonl(headerp->SIT);
    for (i = 0; i <= MAXSERVERID; i++)
	headerp->IpMappedAddr[i] = htonl(headerp->IpMappedAddr[i]);
    for (i = 0; i < HASHSIZE; i++)
	headerp->VolnameHash[i] = htonl(headerp->VolnameHash[i]);
    for (i = 0; i < MAXTYPES; i++)
	for (j = 0; j < HASHSIZE; j++)
	    headerp->VolidHash[i][j] = htonl(headerp->VolidHash[i][j]);

    vldbwrite_cheader(headerp);
}

static void
readMH(afs_int64 addr, int block, struct extentaddr *mhblockP)
{
    int i, j;
    struct extentaddr *e;

    vldbread_exblock(addr, mhblockP, block);

    /* Every mh block has the VLCONTBLOCK flag set in the header to
     * indicate the entry is an 8192 byte extended block. The
     * VLCONTBLOCK flag is always clear in regular vl entries. The
     * vlserver depends on the VLCONTBLOCK flag to correctly traverse
     * the vldb. The flags field is in network byte order. */
    mhblockP->ex_hdrflags = ntohl(mhblockP->ex_hdrflags);

    if (block == 0) {
        /* These header fields are only used in the first mh block. */
        mhblockP->ex_count = ntohl(mhblockP->ex_count);
        for (i = 0; i < VL_MAX_ADDREXTBLKS; i++) {
	    mhblockP->ex_contaddrs[i] = ntohl(mhblockP->ex_contaddrs[i]);
        }
    }
    for (i = 1; i < VL_MHSRV_PERBLK; i++) {
	e = &(mhblockP[i]);

	/* won't convert hostuuid */
	e->ex_uniquifier = ntohl(e->ex_uniquifier);
	for (j = 0; j < VL_MAXIPADDRS_PERMH; j++)
	    e->ex_addrs[j] = ntohl(e->ex_addrs[j]);
    }
    return;
}

static void
vlkv_get(struct rx_opaque *keybuf, struct rx_opaque *valbuf, int *a_noent)
{
    struct rx_opaque_stringbuf opstr;
    int code = okv_get(kvtx, keybuf, valbuf, a_noent);
    if (code != 0) {
	log_error(VLDB_CHECK_FATAL, "error: okv_get failed for key %s: %d\n",
		  rx_opaque_stringify(keybuf, &opstr), code);
    }
}

static void
vlkv_put(struct rx_opaque *keybuf, struct rx_opaque *valbuf, int flags)
{
    struct rx_opaque_stringbuf opstr;
    int code = okv_put(kvtx, keybuf, valbuf, flags);
    if (code != 0) {
	log_error(VLDB_CHECK_FATAL, "error: okv_put failed for key %s: %d\n",
		  rx_opaque_stringify(keybuf, &opstr), code);
    }
}

static int
vldbread_vlentry(afs_uint64 addr, struct nvlentry *vlentryp)
{
    struct rx_opaque_stringbuf opstr;
    struct rx_opaque keybuf;
    int code;

    if (!is_kv()) {
	return vldbread(addr, vlentryp, sizeof(*vlentryp));
    }

    addr2key(&addr, &keybuf);

    code = okv_get_copy(kvtx, &keybuf, vlentryp, sizeof(*vlentryp), NULL);
    if (code != 0) {
	log_error(VLDB_CHECK_FATAL, "error: can't get vlentry for key %s: %d\n",
		  rx_opaque_stringify(&keybuf, &opstr), code);
	return -1;
    }

    return 0;
}

static int
vldbwrite_vlentry(afs_uint64 addr, struct nvlentry *vlentryp)
{
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;
    struct vl4kv_volnamekey nkey;
    struct vl4kv_volidkey idkey;
    int name_len;
    int type_i;
    afs_uint32 rwid = vlentryp->volumeId[0];

    if (!is_kv()) {
	return vldbwrite(addr, vlentryp, sizeof(*vlentryp));
    }

    /* Write out the vlentry itself. */

    addr2key(&addr, &keybuf);
    opaque_set(&valbuf, vlentryp, sizeof(*vlentryp));
    vlkv_put(&keybuf, &valbuf, OKV_PUT_REPLACE);

    /* Write out the volname ptr to the vlentry. */

    name_len = strnlen(vlentryp->name, sizeof(vlentryp->name));

    nkey.tag = htonl(VL4KV_KEY_VOLNAME);
    memcpy(&nkey.name, vlentryp->name, name_len);

    opaque_set(&keybuf, &nkey, sizeof(nkey.tag) + name_len);
    opaque_set(&valbuf, &rwid, sizeof(rwid));
    vlkv_put(&keybuf, &valbuf, 0);

    /* Write out the volid ptr to the vlentry, for each non-RW volid it has. */

    memset(&idkey, 0, sizeof(idkey));
    idkey.tag = htonl(VL4KV_KEY_VOLID);

    for (type_i = ROVOL; type_i < MAXTYPES; type_i++) {
	idkey.volid = vlentryp->volumeId[type_i];
	if (idkey.volid == 0) {
	    continue;
	}

	opaque_set(&keybuf, &idkey, sizeof(idkey));
	opaque_set(&valbuf, &rwid, sizeof(rwid));
	vlkv_put(&keybuf, &valbuf, 0);
    }

    return 0;
}

static void
convertentry(struct nvlentry *vlentryp)
{
    int i;
    for (i = 0; i < MAXTYPES; i++)
	vlentryp->volumeId[i] = ntohl(vlentryp->volumeId[i]);
    vlentryp->flags = ntohl(vlentryp->flags);
    vlentryp->LockAfsId = ntohl(vlentryp->LockAfsId);
    vlentryp->LockTimestamp = ntohl(vlentryp->LockTimestamp);
    vlentryp->cloneId = ntohl(vlentryp->cloneId);
    for (i = 0; i < MAXTYPES; i++)
	vlentryp->nextIdHash[i] = ntohl(vlentryp->nextIdHash[i]);
    vlentryp->nextNameHash = ntohl(vlentryp->nextNameHash);
    for (i = 0; i < NMAXNSERVERS; i++) {
	/* make sure not to ntohl these, as they're chars, not ints */
	vlentryp->serverNumber[i] = vlentryp->serverNumber[i];
	vlentryp->serverPartition[i] = vlentryp->serverPartition[i];
	vlentryp->serverFlags[i] = vlentryp->serverFlags[i];
    }
}

static void
printentry(afs_uint64 addr, struct nvlentry *vlentryp)
{
    int i;
    if (listentries) {
	quiet_println("address 0x%llx (offset 0x%llx): ", addr, OFFSET(addr));
	if (vlentryp->flags == VLCONTBLOCK) {
	    quiet_println("mh extension block\n");
	} else if (vlentryp->flags == VLFREE) {
	    quiet_println("free vlentry\n");
	} else {
	    quiet_println("vlentry %s\n", vlentryp->name);
	    quiet_println("   rw id = %u ; ro id = %u ; bk id = %u\n",
		   vlentryp->volumeId[0], vlentryp->volumeId[1],
		   vlentryp->volumeId[2]);
	    quiet_println("   flags         =");
	    if (vlentryp->flags & VLF_RWEXISTS)
		quiet_println(" rw");
	    if (vlentryp->flags & VLF_ROEXISTS)
		quiet_println(" ro");
	    if (vlentryp->flags & VLF_BACKEXISTS)
		quiet_println(" bk");
	    if (vlentryp->flags & VLOP_MOVE)
		quiet_println(" lock_move");
	    if (vlentryp->flags & VLOP_RELEASE)
		quiet_println(" lock_release");
	    if (vlentryp->flags & VLOP_BACKUP)
		quiet_println(" lock_backup");
	    if (vlentryp->flags & VLOP_DELETE)
		quiet_println(" lock_delete");
	    if (vlentryp->flags & VLOP_DUMP)
		quiet_println(" lock_dump");

	    /* all bits not covered by VLF_* and VLOP_* constants */
	    if (vlentryp->flags & 0xffff8e0f)
		quiet_println(" errorflag(0x%x)", vlentryp->flags);
	    quiet_println("\n");
	    quiet_println("   LockAfsId     = %d\n", vlentryp->LockAfsId);
	    quiet_println("   LockTimestamp = %d\n", vlentryp->LockTimestamp);
	    quiet_println("   cloneId       = %u\n", vlentryp->cloneId);
	    quiet_println
		("   next hash for rw = %u ; ro = %u ; bk = %u ; name = %u\n",
		 vlentryp->nextIdHash[0], vlentryp->nextIdHash[1],
		 vlentryp->nextIdHash[2], vlentryp->nextNameHash);
	    for (i = 0; i < NMAXNSERVERS; i++) {
		if (vlentryp->serverNumber[i] != 255) {
		    quiet_println("   server %d ; partition %d ; flags =",
			   vlentryp->serverNumber[i],
			   vlentryp->serverPartition[i]);
		    if (vlentryp->serverFlags[i] & VLSF_RWVOL)
			quiet_println(" rw");
		    if (vlentryp->serverFlags[i] & VLSF_ROVOL)
			quiet_println(" ro");
		    if (vlentryp->serverFlags[i] & VLSF_BACKVOL)
			quiet_println(" bk");
		    if (vlentryp->serverFlags[i] & VLSF_NEWREPSITE)
			quiet_println(" newro");
		    quiet_println("\n");
		}
	    }
	}
    }
}

static void
readentry(afs_uint64 addr, struct nvlentry *vlentryp, afs_int32 *type)
{
    vldbread_vlentry(addr, vlentryp);

    convertentry(vlentryp);

    if (vlentryp->flags == VLCONTBLOCK) {
	*type = MH;
    } else if (vlentryp->flags == VLFREE) {
	*type = FR;
    } else {
	*type = VL;
    }

    printentry(addr, vlentryp);

    return;
}

static void
writeMH(afs_int64 addr, int block, struct extentaddr *mhblockP)
{
    int i, j;
    struct extentaddr *e;

    if (verbose) {
	quiet_println("Writing back MH block %d at addr %llx\n", block, addr);
    }
    mhblockP->ex_hdrflags = htonl(mhblockP->ex_hdrflags);
    if (block == 0) {
	/*
	 * These header fields are only used in the first mh block, so were
	 * converted to host byte order only when the first mh block was read.
	 */
	mhblockP->ex_count = htonl(mhblockP->ex_count);
	for (i = 0; i < VL_MAX_ADDREXTBLKS; i++) {
	    mhblockP->ex_contaddrs[i] = htonl(mhblockP->ex_contaddrs[i]);
	}
    }
    for (i = 1; i < VL_MHSRV_PERBLK; i++) {
	e = &(mhblockP[i]);
	/* hostuuid was not converted */
	e->ex_uniquifier = htonl(e->ex_uniquifier);
	for (j = 0; j < VL_MAXIPADDRS_PERMH; j++) {
	    e->ex_addrs[j] = htonl(e->ex_addrs[j]);
	}
    }
    vldbwrite_exblock(addr, mhblockP, block);
}

static void
writeentry(afs_int64 addr, struct nvlentry *vlentryp)
{
    int i;

    if (verbose) quiet_println("Writing back entry at addr %llx\n", addr);
    for (i = 0; i < MAXTYPES; i++)
	vlentryp->volumeId[i] = htonl(vlentryp->volumeId[i]);
    vlentryp->flags = htonl(vlentryp->flags);
    vlentryp->LockAfsId = htonl(vlentryp->LockAfsId);
    vlentryp->LockTimestamp = htonl(vlentryp->LockTimestamp);
    vlentryp->cloneId = htonl(vlentryp->cloneId);
    for (i = 0; i < MAXTYPES; i++)
	vlentryp->nextIdHash[i] = htonl(vlentryp->nextIdHash[i]);
    vlentryp->nextNameHash = htonl(vlentryp->nextNameHash);
    for (i = 0; i < NMAXNSERVERS; i++) {
	/* make sure not to htonl these, as they're chars, not ints */
	vlentryp->serverNumber[i] =  vlentryp->serverNumber[i] ;
	vlentryp->serverPartition[i] = vlentryp->serverPartition[i] ;
	vlentryp->serverFlags[i] = vlentryp->serverFlags[i] ;
    }
    vldbwrite_vlentry(addr, vlentryp);
}

static void
check_volkey(char *name, int namelen, afs_uint32 key_volid, afs_uint32 rwid,
	     struct nvlentry *vlentry)
{
    struct vl4kv_volidkey idkey;
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;
    int noent = 0;

    memset(&valbuf, 0, sizeof(valbuf));
    memset(&idkey, 0, sizeof(idkey));
    idkey.tag = htonl(VL4KV_KEY_VOLID);
    idkey.volid = htonl(rwid);

    opaque_set(&keybuf, &idkey, sizeof(idkey));
    vlkv_get(&keybuf, &valbuf, &noent);

    if (noent) {
	if (name != NULL) {
	    log_error(VLDB_CHECK_ERROR, "name key %.*s refers to non-existent rw id %u\n",
		      namelen, name, rwid);
	} else {
	    log_error(VLDB_CHECK_ERROR, "id key %u refers to non-existent rw id %u\n",
		      key_volid, rwid);
	}
	return;
    }

    if (valbuf.len == sizeof(rwid)) {
	afs_uint32 ptr_volid;
	opaque_copy(&valbuf, &ptr_volid, sizeof(ptr_volid));
	ptr_volid = ntohl(ptr_volid);

	if (name != NULL) {
	    log_error(VLDB_CHECK_ERROR, "name key %.*s points to volid %u, "
		      "which is a non-RW volid (pointing to %u)\n",
		      namelen, name, rwid, ptr_volid);
	} else {
	    log_error(VLDB_CHECK_ERROR, "id key %u points to volid %u, "
		      "which is a non-RW volid (pointing to %u)\n",
		      key_volid, rwid, ptr_volid);
	}
	return;
    }

    if (valbuf.len != sizeof(*vlentry)) {
	/* Weirdly-sized value. Just ignore it here; 'nextentry' will flag it
	 * as an error. */
	return;
    }

    opaque_copy(&valbuf, vlentry, sizeof(*vlentry));
    convertentry(vlentry);

    if (name != NULL) {
	/* Check if 'name' matches volume name. */
	int entry_namelen = strnlen(vlentry->name, sizeof(vlentry->name));

	if (namelen != entry_namelen
	     || memcmp(name, vlentry->name, namelen) != 0) {
	    log_error(VLDB_CHECK_ERROR, "key for volume name %.*s points to rwid %u "
		      "with name %.*s\n",
		      namelen, name, rwid, entry_namelen, vlentry->name);
	}

    } else {
	int type_i;
	int found = 0;

	opr_Assert(key_volid != 0);

	/* Check if 'key_volid' matches any volume ids in the entry. */
	for (type_i = 0; type_i < MAXTYPES; type_i++) {
	    if (vlentry->volumeId[type_i] == key_volid) {
		found = 1;
		if (type_i == RWVOL) {
		    log_error(VLDB_CHECK_ERROR, "non-RW key volid %u points to "
			      "rwid %u, but vlentry has rwid %u\n",
			      key_volid, rwid, key_volid);
		}
		break;
	    }
	}

	if (!found) {
	    log_error(VLDB_CHECK_ERROR, "key volid %u points to unrelated rwid %u\n",
		      key_volid, rwid);
	}
    }
}

static void
check_vlentry_kv(afs_uint32 rwid, struct nvlentry *vlentry, afs_int32 *a_type)
{
    int type_i;
    int name_len;
    struct rx_opaque keybuf;
    struct rx_opaque valbuf;
    struct vl4kv_volnamekey namekey;
    afs_uint32 ptr_volid = 0;
    int noent = 0;

    memset(&keybuf, 0, sizeof(keybuf));
    memset(&valbuf, 0, sizeof(valbuf));
    memset(&namekey, 0, sizeof(namekey));

    if (vlentry->flags == VLFREE || vlentry->flags == VLCONTBLOCK) {
	log_error(VLDB_CHECK_ERROR, "vlentry %u has weird flags %x\n", rwid,
		  vlentry->flags);
    }

    if (rwid != vlentry->volumeId[RWVOL]) {
	log_error(VLDB_CHECK_ERROR, "RW volid key %u points to vlentry with RW volid %u\n",
		  rwid, vlentry->volumeId[RWVOL]);
    } else {
	*a_type |= RWH;
	*a_type |= REFRW;
    }

    /* Check if the volname key for the volume's name points back to this
     * volume. */

    name_len = strnlen(vlentry->name, sizeof(vlentry->name));

    namekey.tag = htonl(VL4KV_KEY_VOLNAME);
    memcpy(namekey.name, vlentry->name, name_len);

    opaque_set(&keybuf, &namekey, sizeof(namekey.tag)+name_len);

    vlkv_get(&keybuf, &valbuf, &noent);
    if (noent) {
	/* volname key doesn't exist. */
    } else if (valbuf.len != sizeof(ptr_volid)) {
	/* Weird value for the volname key. Ignore it, 'nextentry' will report
	 * it as an error. */
    } else {
	opaque_copy(&valbuf, &ptr_volid, sizeof(ptr_volid));
	ptr_volid = ntohl(ptr_volid);
	if (ptr_volid != rwid) {
	    log_error(VLDB_CHECK_ERROR, "rw volume %u has name %.*s, which "
		      "points to different volume rw id %u\n", rwid, name_len,
		      vlentry->name, ptr_volid);
	} else {
	    *a_type |= NH;
	    *a_type |= REFN;
	}
    }

    /* For each non-RW volid in the vlentry, check to see if we have a
     * corresponding volid key pointing to this volume. */

    for (type_i = ROVOL; type_i < MAXTYPES; type_i++) {
	struct vl4kv_volidkey idkey;
	afs_uint32 volid = vlentry->volumeId[type_i];

	memset(&keybuf, 0, sizeof(keybuf));

	idkey.tag = htonl(VL4KV_KEY_VOLID);
	idkey.volid = htonl(volid);

	if (volid == 0) {
	    continue;
	}

	opaque_set(&keybuf, &idkey, sizeof(idkey));
	vlkv_get(&keybuf, &valbuf, &noent);
	if (noent) {
	    /* ptr entry for this volid doesn't exist. */
	    continue;
	}

	if (valbuf.len == sizeof(struct nvlentry)) {
	    log_error(VLDB_CHECK_ERROR, "rw volume %u has volid %u, which "
		      "claims to be an rw volid\n", rwid, volid);
	    continue;
	}

	if (valbuf.len != sizeof(volid)) {
	    /* The volid key contains some weird value. Just ignore it here;
	     * 'nextentry' will report it as an error. */
	    continue;
	}

	opaque_copy(&valbuf, &ptr_volid, sizeof(ptr_volid));
	ptr_volid = ntohl(ptr_volid);
	if (ptr_volid != rwid) {
	    log_error(VLDB_CHECK_ERROR, "rw volume %u has volid %u, which "
		      "points to different volume rw id %u\n",
		      rwid, volid, ptr_volid);
	    continue;
	}

	/* Flag that this volume is on the volid "hash" for RO/BK if our RO/BK
	 * volid has a corresponding volid key that points to this vlentry. */
	if (type_i == ROVOL) {
	    *a_type |= ROH;
	    *a_type |= REFRO;
	} else {
	    opr_Assert(type_i == BACKVOL);
	    *a_type |= BKH;
	    *a_type |= REFBK;
	}
    }
}

/* Iterate through the database, and get the next nvlentry. */
static int
nextentry(struct vlheader *header, struct nextinfo *ni,
	  struct nvlentry *vlentry)
{
    struct rx_opaque_stringbuf opstr, opstr2;
    afs_int64 addr = ni->addr;
    int first = 0;
    int code;

    if (ni->valid == 0) {
	first = 1;
	ni->valid = 1;
    }

    if (!is_kv()) {
	afs_int32 type = ni->type;

	if (first) {
	    addr = header->vital_header.headersize;
	} else if (type == MH) {
	    addr += VL_ADDREXTBLK_SIZE;
	} else {
	    addr += sizeof(*vlentry);
	}

	if (addr >= header->vital_header.eofPtr) {
	    goto eof;
	}

	ni->addr = addr;
	ni->rindex = addr / sizeof(*vlentry);
	readentry(addr, vlentry, &ni->type);
	goto success;
    }

    for (;;) {
	struct rx_opaque keybuf;
	struct rx_opaque valbuf;
	int is_eof = 0;
	afs_uint32 tag = 0;

	keybuf = ni->keybuf;
	memset(&valbuf, 0, sizeof(valbuf));

	code = ukv_next(kvtx, &keybuf, &valbuf, &is_eof);
	rx_opaque_freeContents(&ni->keybuf);

	if (code != 0) {
	    log_error(VLDB_CHECK_FATAL, "error: ukv_next failed: %d\n", code);
	    goto error;
	}
	if (is_eof) {
	    goto eof;
	}

	/* The contents of 'keybuf' may become invalid if we make changes to
	 * the db, so make a copy of it for the next iteration of the loop. */
	opr_Verify(rx_opaque_copy(&ni->keybuf, &keybuf) == 0);

	if (!first) {
	    /* For KV, say rindex "n" refers to the nth key-value pair in the
	     * database, starting at 0. */
	    ni->rindex++;
	}

	/*
	 * Our "addr" is the volidkey or exkey for the corresponding record.
	 * Our keys are 64-bits in size, so we can stuff them into the same
	 * 'addr' field as we use for the flatfile db.
	 */
	ni->addr = 0;

	if (ni->keybuf.len >= sizeof(tag)) {
	    opaque_copy(&ni->keybuf, &tag, sizeof(tag));
	    tag = ntohl(tag);
	}

	if (tag == VL4KV_KEY_CHEADERKV && ni->keybuf.len == sizeof(tag)
	     && valbuf.len == sizeof(struct vlheader_kv)) {
	    /* Normal header KV item. */
	    continue;
	}

	if (tag == VL4KV_KEY_VOLNAME
	     && ni->keybuf.len > sizeof(tag)
	     && ni->keybuf.len <= sizeof(struct vl4kv_volnamekey)
	     && valbuf.len == sizeof(afs_uint32)) {
	    struct vl4kv_volnamekey namekey;
	    afs_uint32 rwid;
	    int namelen;

	    /* Normal volname KV item. Lookup the vlentry that it's pointing
	     * to, and check if the name matches. */

	    opr_Assert(ni->keybuf.len <= sizeof(namekey));
	    opaque_copy(&ni->keybuf, &namekey, ni->keybuf.len);
	    namelen = ni->keybuf.len - sizeof(namekey.tag);

	    opaque_copy(&valbuf, &rwid, sizeof(rwid));
	    rwid = ntohl(rwid);

	    check_volkey(namekey.name, namelen, 0, rwid, vlentry);

	    continue;
	}

	if (tag == VL4KV_KEY_VOLID
	     && ni->keybuf.len == sizeof(struct vl4kv_volidkey)) {
	    struct vl4kv_volidkey idkey;

	    opaque_copy(&ni->keybuf, &idkey, sizeof(idkey));
	    idkey.volid = ntohl(idkey.volid);

	    if (idkey.volid == 0) {
		/*
		 * This is a volid key for volid 0; that's not right. Fall
		 * through to the code below that handles unrecognized keys.
		 */

	    } else if (valbuf.len == sizeof(*vlentry)) {
		/* A KV item for an actual vlentry. */
		key2addr(&ni->keybuf, &ni->addr);
		opaque_copy(&valbuf, vlentry, sizeof(*vlentry));
		convertentry(vlentry);

		printentry(ni->addr, vlentry);
		ni->type = VL;

		check_vlentry_kv(idkey.volid, vlentry, &ni->type);
		goto success;

	    } else if (valbuf.len == sizeof(afs_uint32)) {
		/* A KV item for a non-RW volid pointing to the associated
		 * vlentry. Check if the vlentry actually contains that volid. */
		afs_uint32 rwid;
		opaque_copy(&valbuf, &rwid, sizeof(rwid));
		rwid = ntohl(rwid);

		check_volkey(NULL, 0, idkey.volid, rwid, vlentry);
		continue;
	    }
	}

	if (tag == VL4KV_KEY_EXBLOCK
	     && ni->keybuf.len == sizeof(struct vl4kv_exkey)
	     && valbuf.len == VL_ADDREXTBLK_SIZE) {
	    struct vl4kv_exkey exkey;
	    afs_uint32 base;

	    key2addr(&ni->keybuf, &ni->addr);
	    opaque_copy(&ni->keybuf, &exkey, sizeof(exkey));
	    base = ntohl(exkey.base);
	    ni->type = MH;

	    if (base >= VL_MAX_ADDREXTBLKS) {
		log_error(VLDB_CHECK_ERROR, "exblock with key %s has invalid "
			  "base %u\n",
			  rx_opaque_stringify(&ni->keybuf, &opstr),
			  base);
	    } else {
		ni->type |= MHC;
		mhinfo[base].addr = ni->addr;
	    }
	    goto success;
	}

	log_error(VLDB_CHECK_WARNING, "warning: unrecognized KV item: key %s "
		  "value %s\n",
		  rx_opaque_stringify(&ni->keybuf, &opstr),
		  rx_opaque_stringify(&valbuf, &opstr2));
    }

 error:
    opr_abort();

 success:
    opr_Assert(ni->addr != 0);
    return 0;

 eof:
    return 1;
}

/*
 * Read each entry in the database:
 * Record what type of entry it is and its address in the record array.
 * Remember what the maximum volume id we found is and check against the header.
 */
static void
ReadAllEntries(struct vlheader *header)
{
    afs_int32 i, j, e;
    int freecount = 0, mhcount = 0, vlcount = 0;
    int rwcount = 0, rocount = 0, bkcount = 0;
    struct nvlentry vlentry;
    struct nextinfo ni;
    afs_uint32 maxvolid = 0;

    memset(&vlentry, 0, sizeof(vlentry));
    memset(&ni, 0, sizeof(ni));

    if (verbose) quiet_println("Read each entry in the database\n");
    for (;;) {
	int eof;
	afs_int64 addr;
	afs_int32 type;
	afs_int32 rindex;

	eof = nextentry(header, &ni, &vlentry);
	if (eof) {
	    break;
	}

	addr = ni.addr;
	type = ni.type;
	rindex = ni.rindex;

	if ((type & VL) != 0) {

	    for (i = 0; i < MAXTYPES; i++)
		if (maxvolid < vlentry.volumeId[i])
		    maxvolid = vlentry.volumeId[i];

	    e = 1;
	    for (j = 0; j < NMAXNSERVERS; j++) {
		if (vlentry.serverNumber[j] == 255)
		    continue;
		if (vlentry.serverFlags[j] & (VLSF_ROVOL | VLSF_NEWREPSITE)) {
		    rocount++;
		    continue;
		}
		if (vlentry.serverFlags[j] & VLSF_RWVOL) {
		    rwcount++;
		    if (vlentry.flags & VLF_BACKEXISTS)
			bkcount++;
		    continue;
		}
		if (!vlentry.serverFlags[j]) {
		    /*e = 0;*/
		    continue;
 		}
		if (e) {
		   log_error(VLDB_CHECK_ERROR,
			     "address 0x%llx (offset 0x%llx): VLDB entry '%s' "
			     "contains an unknown RW/RO index serverFlag\n",
			     addr, OFFSET(addr), vlentry.name);
		    e = 0;
		}
		quiet_println("   index %d : serverNumber %d : serverPartition %d : serverFlag %d\n",
			      j, vlentry.serverNumber[j],
			      vlentry.serverPartition[j],
			      vlentry.serverFlags[j]);
	    }
	}

	if (record[rindex].type) {
	    log_error(VLDB_CHECK_ERROR, "INTERNAL VLDB_CHECK_ERROR: record holder %d already in use\n",
		      rindex);
	    return;
	}
	record[rindex].addr = addr;
	record[rindex].type = type;

	/* Keep count */
	if ((type & VL) != 0) {
	    vlcount++;
	} else if ((type & FR) != 0) {
	    freecount++;
	} else if ((type & MH) != 0) {
	    mhcount++;
	} else {
	    log_error(VLDB_CHECK_ERROR, "address 0x%llx (offset 0x%llx): Unknown entry. Aborting\n",
		      addr, OFFSET(addr));
	    break;
	}
    }
    if (verbose) {
	quiet_println("Found %d entries, %d free entries, %d multihomed blocks\n",
		      vlcount, freecount, mhcount);
	quiet_println("Found %d RW volumes, %d BK volumes, %d RO volumes\n",
		      rwcount, bkcount, rocount);
    }

    /* Check the maxmimum volume id in the header */
    if (maxvolid != header->vital_header.MaxVolumeId - 1)
	quiet_println("Header's maximum volume id is %u and largest id found in VLDB is %u\n",
		      header->vital_header.MaxVolumeId, maxvolid);
}

/*
 * Follow each Name hash bucket marking it as read in the record array.
 * Record we found it in the name hash within the record array.
 * Check that the name is hashed correctly.
 */
static void
FollowNameHash(struct vlheader *header)
{
    int count = 0, longest = 0, shortest = -1, chainlength;
    struct nvlentry vlentry;
    afs_int64 addr;
    afs_int32 i, type, rindex;

    /* Now follow the Name Hash Table */
    if (verbose) quiet_println("Check Volume Name Hash\n");
    for (i = 0; i < HASHSIZE; i++) {
	chainlength = 0;

	if (!validVolumeAddr(header->VolnameHash[i])) {
	    log_error(VLDB_CHECK_ERROR, "Name Hash index %d is out of range: %u\n",
		      i, header->VolnameHash[i]);
	    continue;
	}

	for (addr = header->VolnameHash[i]; addr; addr = vlentry.nextNameHash) {
	    readentry(addr, &vlentry, &type);
	    if (type != VL) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Name Hash %d: Not a vlentry\n",
			  addr, OFFSET(addr), i);
		continue;
	    }

	    rindex = ADDR(addr);

	    /*
	     * we know that the address is valid because we
	     * checked it either above or below
	     */
	    if (record[rindex].addr != addr && record[rindex].addr) {
		log_error(VLDB_CHECK_ERROR,
			  "INTERNAL VLDB_CHECK_ERROR: addresses %llx and %llx use same record slot %d\n",
			  record[rindex].addr, addr, rindex);
	    }
	    if (record[rindex].type & NH) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Name Hash %d: volume "
			  "name '%s' is already in the name hash\n",
			  addr, OFFSET(addr), i, vlentry.name);
		record[rindex].type |= MULTN;
		break;
	    }

	    if (!validVolumeAddr(vlentry.nextNameHash)) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Name Hash forward link "
			  "of '%s' is out of range\n",
			  addr, OFFSET(addr), vlentry.name);
		record[rindex].type |= MULTN;
		break;
	    }

	    record[rindex].type |= NH;
	    record[rindex].type |= REFN;

	    chainlength++;
	    count++;

	    /* Hash the name and check if in correct hash table */
	    if (NameHash(vlentry.name) != i) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Name Hash %d: volume "
			  "name '%s': Incorrect name hash chain (should be in %d)\n",
			  addr, OFFSET(addr), i, vlentry.name,
			  NameHash(vlentry.name));
		record[rindex].type |= MULTN;
	    }
	}
	if (chainlength > longest)
	    longest = chainlength;
	if ((shortest == -1) || (chainlength < shortest))
	    shortest = chainlength;
    }
    if (verbose) {
	quiet_println("%d entries in name hash, longest is %d, shortest is %d, "
		      "average length is %f\n",
		      count, longest, shortest,
		      ((float)count / (float)HASHSIZE));
    }
    return;
}

/*
 * Follow the ID hash chains for the RW, RO, and BK hash tables.
 * Record we found it in the id hash within the record array.
 * Check that the ID is hashed correctly.
 */
static void
FollowIdHash(struct vlheader *header)
{
    int count = 0, longest = 0, shortest = -1, chainlength;
    struct nvlentry vlentry;
    afs_int64 addr;
    afs_int32 i, j, hash, type, rindex, ref, badref, badhash;

    /* Now follow the RW, RO, and BK Hash Tables */
    if (verbose) quiet_println("Check RW, RO, and BK id Hashes\n");
    for (i = 0; i < MAXTYPES; i++) {
	hash = ((i == 0) ? RWH : ((i == 1) ? ROH : BKH));
	ref = ((i == 0) ? REFRW : ((i == 1) ? REFRO : REFBK));
	badref = ((i == 0) ? MULTRW : ((i == 1) ? MULTRO : MULTBK));
	badhash = ((i == 0) ? MULTRW : ((i == 1) ? MULTRO : MULTBK));
	count = longest = 0;
	shortest = -1;

	for (j = 0; j < HASHSIZE; j++) {
	    chainlength = 0;
	    if (!validVolumeAddr(header->VolidHash[i][j])) {
		log_error(VLDB_CHECK_ERROR, "%s Hash index %d is out of range: %u\n",
			  vtype(i), j, header->VolidHash[i][j]);
		continue;
	    }

	    for (addr = header->VolidHash[i][j]; addr;
		 addr = vlentry.nextIdHash[i]) {
		readentry(addr, &vlentry, &type);
		if (type != VL) {
		    log_error(VLDB_CHECK_ERROR,
			      "address 0x%llx (offset 0x%llx): %s Id Hash %d: Not a vlentry\n",
			      addr, OFFSET(addr), vtype(i), j);
		    continue;
		}

		rindex = ADDR(addr);
		if (record[rindex].addr != addr && record[rindex].addr) {
		    log_error(VLDB_CHECK_ERROR,
			      "INTERNAL VLDB_CHECK_ERROR: addresses %llx and %llx "
			      "use same record slot %d\n",
			      record[rindex].addr, addr, rindex);
		}
		if (record[rindex].type & hash) {
		    log_error(VLDB_CHECK_ERROR,
			      "address 0x%llx (offset 0x%llx): %s Id Hash %d: "
			      "volume name '%s': Already in the hash table\n",
			      addr, OFFSET(addr), vtype(i), j, vlentry.name);
		    record[rindex].type |= badref;
		    break;
		}

		if (!validVolumeAddr(vlentry.nextIdHash[i])) {
		    log_error(VLDB_CHECK_ERROR,
			      "address 0x%llx (offset 0x%llx): %s Id Hash forward "
			      "link of '%s' is out of range\n",
			      addr, OFFSET(addr), vtype(i), vlentry.name);
		    record[rindex].type |= badref;
		    break;
		}

		record[rindex].type |= hash;
		record[rindex].type |= ref;

		chainlength++;
		count++;

		/* Hash the id and check if in correct hash table */
		if (IdHash(vlentry.volumeId[i]) != j) {
		   log_error(VLDB_CHECK_ERROR,
			     "address 0x%llx (offset 0x%llx): %s Id Hash %d: "
			     "volume name '%s': Incorrect Id hash chain (should "
			     "be in %d)\n",
			     addr, OFFSET(addr), vtype(i), j, vlentry.name,
			     IdHash(vlentry.volumeId[i]));
		    record[rindex].type |= badhash;
		}
	    }

	    if (chainlength > longest)
		longest = chainlength;
	    if ((shortest == -1) || (chainlength < shortest))
		shortest = chainlength;
	}
	if (verbose) {
	    quiet_println("%d entries in %s hash, longest is %d, shortest is "
			  "%d, average length is %f\n",
			  count, vtype(i), longest, shortest,
			  ((float)count / (float)HASHSIZE));
	}
    }
    return;
}

/*
 * Follow the free chain.
 * Record we found it in the free chain within the record array.
 */
static void
FollowFreeChain(struct vlheader *header)
{
    afs_int32 count = 0;
    struct nvlentry vlentry;
    afs_int64 addr;
    afs_int32 type, rindex;

    /* Now follow the Free Chain */
    if (verbose) quiet_println("Check Volume Free Chain\n");
    for (addr = header->vital_header.freePtr; addr;
	 addr = vlentry.nextIdHash[0]) {
	readentry(addr, &vlentry, &type);
	if (type != FR) {
	   log_error(VLDB_CHECK_ERROR,
		     "address 0x%llx (offset 0%llx): Free Chain %d: Not a free "
		     "vlentry (0x%x)\n",
		     addr, OFFSET(addr), count, type);
	    continue;
	}

	rindex = addr / sizeof(vlentry);
	if (record[rindex].addr != addr && record[rindex].addr) {
	    log_error(VLDB_CHECK_ERROR,
		      "INTERNAL VLDB_CHECK_ERROR: addresses %llx (0x%llx) and "
		      "%llx (0x%llx) use same record slot %d\n",
		      record[rindex].addr, OFFSET(record[rindex].addr), addr,
		      OFFSET(addr), rindex);
	}
	if (record[rindex].type & FRC) {
	    log_error(VLDB_CHECK_ERROR,
		      "address 0x%llx (offset 0x%llx): Free Chain: Already in the "
		      "free chain\n",
		      addr, OFFSET(addr));
	    break;
	}
	record[rindex].type |= FRC;

	count++;
    }
    if (verbose)
	quiet_println("%d entries on free chain\n", count);
    return;
}

/*
 * Read each multihomed block and mark it as found in the record.
 * Read each entry in each multihomed block and mark the serveraddrs
 * array with the number of ip addresses found for this entry.
 *
 * Then read the IpMappedAddr array in the header.
 * Verify that multihomed entries base and index are valid and points to
 * a good multhomed entry.
 * Mark the serveraddrs array with 1 ip address for regular entries.
 *
 * By the end, the severaddrs array will have a 0 if the entry has no
 * IP addresses in it or the count of the number of IP addresses.
 *
 * The code does not verify if there are duplicate IP addresses in the
 * list. The vlserver does this when a fileserver registeres itself.
 */
static void
CheckIpAddrs(struct vlheader *header)
{
    int mhblocks = 0;
    afs_int32 i, j, m, rindex;
    afs_int32 mhentries, regentries;
    char mhblock[VL_ADDREXTBLK_SIZE];
    struct extentaddr *MHblock = (struct extentaddr *)mhblock;
    struct extentaddr *e;
    int ipindex, ipaddrs;
    afsUUID nulluuid;

    memset(&nulluuid, 0, sizeof(nulluuid));

    if (verbose)
	quiet_println("Check Multihomed blocks\n");

    if (header->SIT) {
	/* Read the first MH block and from it, gather the
	 * addresses of all the mh blocks.
	 */
	readMH(header->SIT, 0, MHblock);
	if (MHblock->ex_hdrflags != VLCONTBLOCK) {
	    log_error(VLDB_CHECK_ERROR,
		      "address 0x%x (offset 0x%x): Multihomed Block 0: Not a "
		      "multihomed block\n",
		      header->SIT, OFFSET(header->SIT));
	}

	if (!is_kv()) {
	    for (i = 0; i < VL_MAX_ADDREXTBLKS; i++) {
		mhinfo[i].addr = MHblock->ex_contaddrs[i];
	    }

	    if (header->SIT != mhinfo[0].addr) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%x (offset 0x%x): MH block does not point to "
			  "self in header, %llx in block\n",
			  header->SIT, OFFSET(header->SIT), mhinfo[0].addr);
	    }
	}

	/* Now read each MH block and record it in the record array */
	for (i = 0; i < VL_MAX_ADDREXTBLKS; i++) {
	    if (!mhinfo[i].addr)
		continue;

	    readMH(mhinfo[i].addr, i, MHblock);
	    if (MHblock->ex_hdrflags != VLCONTBLOCK) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Multihomed Block %d: "
			  "Not a multihomed block\n",
			  mhinfo[i].addr, OFFSET(mhinfo[i].addr), i);
	    }

	    if (!is_kv()) {
		rindex = mhinfo[i].addr / sizeof(vlentry);
		if (record[rindex].addr != mhinfo[i].addr && record[rindex].addr) {
		    log_error(VLDB_CHECK_ERROR,
			      "INTERNAL VLDB_CHECK_ERROR: addresses %llx (0x%llx) "
			      "and %llx (0x%llx) use same record slot %d\n",
			      record[rindex].addr, OFFSET(record[rindex].addr),
			      mhinfo[i].addr, OFFSET(mhinfo[i].addr), rindex);
		}
		if (record[rindex].type & MHC) {
		    log_error(VLDB_CHECK_ERROR,
			      "address 0x%llx (offset 0x%llx): MH Blocks Chain %d: "
			      "Already a MH block\n",
			      record[rindex].addr, OFFSET(record[rindex].addr), i);
		    break;
		}
		record[rindex].type |= MHC;
	    }

	    mhblocks++;

	    /* Read each entry in a multihomed block.
	     * Find the pointer to the entry in the IpMappedAddr array and
	     * verify that the entry is good (has IP addresses in it).
	     */
	    mhentries = 0;
	    for (j = 1; j < VL_MHSRV_PERBLK; j++) {
		int first_ipindex = -1;
		e = (struct extentaddr *)&(MHblock[j]);

		/* Search the IpMappedAddr array for all the references to this entry. */
		/* Use the first reference for checking the ip addresses of this entry. */
		for (ipindex = 0; ipindex <= MAXSERVERID; ipindex++) {
		    if (((header->IpMappedAddr[ipindex] & 0xff000000) == 0xff000000)
			&& (((header-> IpMappedAddr[ipindex] & 0x00ff0000) >> 16) == i)
			&& ((header->IpMappedAddr[ipindex] & 0x0000ffff) == j)) {
			if (first_ipindex == -1) {
			    first_ipindex = ipindex;
			} else {
			    serverxref[ipindex] = first_ipindex;
			}
		    }
		}
		ipindex = first_ipindex;
		if (ipindex != -1)
		    serveraddrs[ipindex] = -1;

		if (memcmp(&e->ex_hostuuid, &nulluuid, sizeof(afsUUID)) == 0) {
		    if (ipindex != -1) {
			log_error(VLDB_CHECK_ERROR,
				  "Server Addrs index %d references null MH "
				  "block %d, index %d\n",
				  ipindex, i, j);
			serveraddrs[ipindex] = 0;	/* avoids printing 2nd error below */
		    }
		    continue;
		}

		/* Step through each ip address and count the good addresses */
		ipaddrs = 0;
		for (m = 0; m < VL_MAXIPADDRS_PERMH; m++) {
		    if (e->ex_addrs[m])
			ipaddrs++;
		}

		/* If we found any good ip addresses, mark it in the
		 * serveraddrs record */
		if (ipaddrs) {
		    mhentries++;
		    if (ipindex == -1) {
		        mhinfo[i].orphan[j] = 1;
			log_error(VLDB_CHECK_ERROR,
				  "MH block %d, index %d: Not referenced by "
				  "server addrs\n",
				  i, j);
		    } else {
			serveraddrs[ipindex] = ipaddrs;	/* It is good */
		    }
		}

		if (listservers && ipaddrs) {
		    quiet_println("MH block %d, index %d:", i, j);
		    for (m = 0; m < VL_MAXIPADDRS_PERMH; m++) {
			if (!e->ex_addrs[m])
			    continue;
			quiet_println(" %d.%d.%d.%d",
			       (e->ex_addrs[m] & 0xff000000) >> 24,
			       (e->ex_addrs[m] & 0x00ff0000) >> 16,
			       (e->ex_addrs[m] & 0x0000ff00) >> 8,
			       (e->ex_addrs[m] & 0x000000ff));
		    }
		    quiet_println("\n");
		}
	    }
	}
    }
    if (verbose)
	quiet_println("%d multihomed blocks\n", mhblocks);

    /* Check the server addresses */
    if (verbose)
	quiet_println("Check server addresses\n");
    mhentries = regentries = 0;
    for (i = 0; i <= MAXSERVERID; i++) {
	if (header->IpMappedAddr[i]) {
	    if ((header->IpMappedAddr[i] & 0xff000000) == 0xff000000) {
		mhentries++;
		if (((header->IpMappedAddr[i] & 0x00ff0000) >> 16) >
		    VL_MAX_ADDREXTBLKS)
		   log_error(VLDB_CHECK_ERROR,
			     "IP Addr for entry %d: Multihome block is bad (%d)\n",
			     i, ((header->IpMappedAddr[i] & 0x00ff0000) >> 16));

		if (mhinfo[(header->IpMappedAddr[i] & 0x00ff0000) >> 16].addr == 0)
		    log_error(VLDB_CHECK_ERROR,
			      "IP Addr for entry %d: No such multihome block (%d)\n",
			      i, ((header->IpMappedAddr[i] & 0x00ff0000) >> 16));

		if (((header->IpMappedAddr[i] & 0x0000ffff) > VL_MHSRV_PERBLK)
		    || ((header->IpMappedAddr[i] & 0x0000ffff) < 1))
		    log_error(VLDB_CHECK_ERROR,
			      "IP Addr for entry %d: Multihome index is bad (%d)\n",
			      i, (header->IpMappedAddr[i] & 0x0000ffff));

		if (serveraddrs[i] == -1) {
		    log_error(VLDB_CHECK_WARNING,
			      "warning: IP Addr for entry %d: Multihome entry "
			      "has no ip addresses\n", i);
		    serveraddrs[i] = 0;
		}
		if (serverxref[i] != BADSERVERID) {
		    log_error(VLDB_CHECK_WARNING,
			      "warning: MH block %d, index %d is cross-linked "
			      "by server numbers %d and %d.\n",
			      (header->IpMappedAddr[i] & 0x00ff0000) >> 16,
			      (header->IpMappedAddr[i] & 0x0000ffff),
			      i, serverxref[i]);
		    /* set addresses found/not found for this server number,
		     * using the first index to the mh we found above. */
		    serveraddrs[i] = serveraddrs[serverxref[i]];
		}
		if (listservers) {
		    quiet_println("   Server ip addr %d = MH block %d, index %d\n",
				  i,
				  (header->IpMappedAddr[i] & 0x00ff0000) >> 16,
				  (header->IpMappedAddr[i] & 0x0000ffff));
		}
	    } else {
		regentries++;
		serveraddrs[i] = 1;	/* It is good */
		if (listservers) {
		    quiet_println("   Server ip addr %d = %d.%d.%d.%d\n", i,
			   (header->IpMappedAddr[i] & 0xff000000) >> 24,
			   (header->IpMappedAddr[i] & 0x00ff0000) >> 16,
			   (header->IpMappedAddr[i] & 0x0000ff00) >> 8,
			   (header->IpMappedAddr[i] & 0x000000ff));
		}
	    }
	}
    }
    if (verbose) {
	quiet_println("%d simple entries, %d multihomed entries, Total = %d\n",
		      regentries, mhentries, mhentries + regentries);
    }
    return;
}

static char *
nameForAddr(afs_int64 addr, int hashtype, afs_uint32 *hash, char *buffer)
{
    /*
     * We need to simplify the reporting, while retaining
     * legible messages.  This is a helper function.  The return address
     * is either a fixed char or the provided buffer - so don't use the
     * name after the valid lifetime of the buffer.
     */
    afs_int32 type;
    struct nvlentry entry;
    if (!addr) {
	/* Distinguished, invalid, hash */
	*hash = 0xFFFFFFFF;
	return "empty";
    } else if (!validVolumeAddr(addr)) {
	/* Different, invalid, hash */
	*hash = 0XFFFFFFFE;
	return "invalid";
    }
    readentry(addr, &entry, &type);
    if (VL != type) {
	*hash = 0XFFFFFFFE;
	return "invalid";
    }
    if (hashtype >= MAXTYPES) {
	*hash = NameHash(entry.name);
    } else {
	*hash = IdHash(entry.volumeId[hashtype]);
    }
    sprintf(buffer, "for '%s'", entry.name);
    return buffer;
}

static void
reportHashChanges(struct vlheader *header, afs_uint32 oldnamehash[HASHSIZE], afs_uint32 oldidhash[MAXTYPES][HASHSIZE])
{
    int i, j;
    afs_uint32 oldhash, newhash;
    char oldNameBuffer[10 + VL_MAXNAMELEN];
    char newNameBuffer[10 + VL_MAXNAMELEN];
    char *oldname, *newname;
    /*
     * report hash changes
     */

    for (i = 0; i < HASHSIZE; i++) {
	if (oldnamehash[i] != header->VolnameHash[i]) {

	    oldname = nameForAddr(oldnamehash[i], MAXTYPES, &oldhash, oldNameBuffer);
	    newname = nameForAddr(header->VolnameHash[i], MAXTYPES, &newhash, newNameBuffer);
	    if (verbose || (oldhash != newhash)) {
		quiet_println("FIX: Name hash header at %d was %s, is now %s\n",
			      i, oldname, newname);
	    }
	}
	for (j = 0; j < MAXTYPES; j++) {
	    if (oldidhash[j][i] != header->VolidHash[j][i]) {

		oldname = nameForAddr(oldidhash[j][i], j, &oldhash, oldNameBuffer);
		newname = nameForAddr(header->VolidHash[j][i], j, &newhash, newNameBuffer);
		if (verbose || (oldhash != newhash)) {
		    quiet_println("FIX: %s hash header at %d was %s, is now %s\n",
				  vtype(j), i, oldname, newname);
		}
	    }
	}
    }
}

/**
 * Remove unreferenced, duplicate multi-home address indices.
 *
 * Removes entries from IpMappedAddr which where found to be
 * duplicates. Only entries which are not referenced by vl entries
 * are removed on this pass.
 *
 * @param[inout] header the vldb header to be updated.
 */
static void
removeCrossLinkedAddresses(struct vlheader *header)
{
    int i;

    for (i = 0; i <= MAXSERVERID; i++) {
	if (serverref[i] == 0
	    && (header->IpMappedAddr[i] & 0xff000000) == 0xff000000
	    && serverxref[i] != BADSERVERID) {
	    if (serverxref[i] == i) {
		log_error(VLDB_CHECK_ERROR,
			  "INTERNAL VLDB_CHECK_ERROR: serverxref points to self; index %d\n",
			  i);
	    } else if (header->IpMappedAddr[serverxref[i]] == 0) {
		log_error(VLDB_CHECK_ERROR,
			  "INTERNAL VLDB_CHECK_ERROR: serverxref points to empty addr; index %d, value %d\n",
			  i, serverxref[i]);
	    } else if (header->IpMappedAddr[serverxref[i]] != header->IpMappedAddr[i]) {
		log_error(VLDB_CHECK_ERROR,
			  "INTERNAL VLDB_CHECK_ERROR: invalid serverxref; index %d, value %d\n",
			  i, serverxref[i]);
	    } else {
		quiet_println("FIX: Removing unreferenced address index %d, "
			      "which cross-links MH block %d, index %d\n",
			      i,
			      (header->IpMappedAddr[i] & 0x00ff0000) >> 16,
			      (header->IpMappedAddr[i] & 0x0000ffff));
		header->IpMappedAddr[i] = 0;
	    }
	}
    }
}

static int
opendb(char *dbfile)
{
    int code;
    struct stat st;

    code = stat(dbfile, &st);
    if (code < 0) {
	log_error(VLDB_CHECK_FATAL, "can't access file '%s'. error = %d\n",
		  dbfile, errno);
	goto error;
    }

    fd = -1;

    if (S_ISDIR(st.st_mode)) {
	int flags;

	code = ukv_open(dbfile, &dbh, NULL);
	if (code != 0) {
	    log_error(VLDB_CHECK_FATAL, "can't open KV dbase '%s'. error = %d\n",
		      dbfile, code);
	    goto error;
	}

	if (fix > 0) {
	    flags = OKV_BEGIN_RW;
	} else {
	    flags = OKV_BEGIN_RO;
	}

	code = okv_begin(dbh, flags, &kvtx);
	if (code != 0) {
	    okv_close(&dbh);
	    log_error(VLDB_CHECK_FATAL, "can't start tx for KV dbase '%s'. error = %d\n",
		      dbfile, code);
	    goto error;
	}

    } else {
	fd = open(dbfile, (fix > 0)?O_RDWR:O_RDONLY, 0);
	if (fd < 0) {
	    log_error(VLDB_CHECK_FATAL,"can't open file '%s'. error = %d\n",
		      dbfile, errno);
	    goto error;
	}
    }

    return 0;

 error:
    return -1;
}

static afs_uint64
maxentries_kv(void)
{
    int code;
    struct okv_statinfo stat;

    code = okv_stat(kvtx, &stat);
    if (code != 0) {
	log_error(VLDB_CHECK_FATAL, "error: can't stat KV dbase: %d\n", code);
	goto error;
    }

    if (stat.os_entries == NULL) {
	log_error(VLDB_CHECK_FATAL, "error: KV dbase entry count not "
				    "supported");
	goto error;
    }

    return *stat.os_entries;

 error:
    return 0;
}

/* Delete all of the indirect vlentry kv entries in the db (so we can recreate
 * them). That is, all of the volname keys and non-RW volid keys. */
static void
clear_kvhash(void)
{
    int code;
    struct rx_opaque keybuf;
    struct rx_opaque_stringbuf opstr;

    memset(&keybuf, 0, sizeof(keybuf));

    for (;;) {
	afs_uint32 tag = 0;
	int eof = 0;
	int delitem = 0;
	struct rx_opaque raw_keybuf;
	struct rx_opaque valbuf;

	raw_keybuf = keybuf;
	memset(&valbuf, 0, sizeof(valbuf));

	code = ukv_next(kvtx, &raw_keybuf, &valbuf, &eof);
	rx_opaque_freeContents(&keybuf);

	if (code != 0) {
	    log_error(VLDB_CHECK_FATAL, "error: ukv_next failed: %d\n", code);
	    return;
	}
	if (eof) {
	    return;
	}

	/* The contents of raw_keybuf may become invalid if we delete keys, so
	 * make a copy for the next iteration of the loop. */
	opr_Verify(rx_opaque_copy(&keybuf, &raw_keybuf) == 0);

	if (keybuf.len >= sizeof(tag)) {
	    opaque_copy(&keybuf, &tag, sizeof(tag));
	    tag = ntohl(tag);
	}

	switch (tag) {
	case VL4KV_KEY_VOLNAME:
	    if (keybuf.len > sizeof(tag)
		 && keybuf.len <= sizeof(struct vl4kv_volnamekey)
		 && valbuf.len == sizeof(afs_uint32)) {
		delitem = 1;
	    }
	    break;

	case VL4KV_KEY_VOLID:
	    if (keybuf.len == sizeof(struct vl4kv_volidkey)
		 && valbuf.len == sizeof(afs_uint32)) {
		delitem = 1;
	    }
	    break;
	}

	if (delitem) {
	    code = okv_del(kvtx, &keybuf, NULL);
	    if (code != 0) {
		log_error(VLDB_CHECK_FATAL, "error: okv_del failed for key %s: %d\n",
			  rx_opaque_stringify(&keybuf, &opstr), code);
	    }
	}
    }
}

static int
WorkerBee(struct cmd_syndesc *as, void *arock)
{
    char *dbfile;
    afs_int32 type;
    struct vlheader header;
    struct nvlentry vlentry, vlentry2;
    int i, j, k;
    int code;
    afs_uint32 oldnamehash[HASHSIZE];
    afs_uint32 oldidhash[MAXTYPES][HASHSIZE];

    error_level = 0;  /*  start clean with no error status */
    dbfile = as->parms[0].items->data;	/* -database */
    listuheader = (as->parms[1].items ? 1 : 0);	/* -uheader  */
    listheader = (as->parms[2].items ? 1 : 0);	/* -vheader  */
    listservers = (as->parms[3].items ? 1 : 0);	/* -servers  */
    listentries = (as->parms[4].items ? 1 : 0);	/* -entries  */
    verbose = (as->parms[5].items ? 1 : 0);	/* -verbose  */
    quiet = (as->parms[6].items ? 1 : 0);  /* -quiet */
    fix = (as->parms[7].items ? 1 : 0);    /* -fix  */

    /* sanity check */
    if (quiet && (verbose || listuheader || listheader ||listservers \
                || listentries)) {
	log_error(VLDB_CHECK_FATAL,
		  " -quiet cannot be used other display flags\n");
        return VLDB_CHECK_FATAL;
    }

    code = opendb(dbfile);
    if (code != 0) {
	return 0;
    }

    /* read the ubik header and the vldb database header */
    readUbikHeader();
    readheader(&header);
    if (header.vital_header.vldbversion < 3) {
	log_error(VLDB_CHECK_FATAL,
		  "does not support vldb with version less than 3\n");
	return VLDB_CHECK_FATAL;
    }

    if (!is_kv()) {
	maxentries = (header.vital_header.eofPtr / sizeof(vlentry)) + 1;
    } else {
	maxentries = maxentries_kv();
    }

    record = calloc(maxentries, sizeof(struct er));
    memset(serveraddrs, 0, sizeof(serveraddrs));
    memset(mhinfo, 0, sizeof(mhinfo));
    memset(serverref, 0, sizeof(serverref));
    for (i = 0; i <= MAXSERVERID; i++) {
	serverxref[i] = BADSERVERID;
    }

    /* Will fill in the record array of entries it found */
    ReadAllEntries(&header);
    listentries = 0;		/* Listed all the entries */

    /* Check the multihomed blocks for valid entries as well as
     * the IpMappedAddrs array in the header for valid entries.
     */
    CheckIpAddrs(&header);

    if (!is_kv()) {
	/* Follow the hash tables */
	FollowNameHash(&header);
	FollowIdHash(&header);

	/* Follow the chain of free entries */
	FollowFreeChain(&header);
    }

    /* Now check the record we have been keeping for inconsistencies
     * For valid vlentries, also check that the server we point to is
     * valid (the serveraddrs array).
     */
    if (verbose)
	quiet_println("Verify each volume entry\n");
    for (i = 0; i < maxentries; i++) {
	int hash = 0;
        int nexthash = 0;
	char *which = NULL;

	if (record[i].type == 0)
	    continue;

	/* If a vlentry, verify that its name is valid, its name and ids are
	 * on the hash chains, and its server numbers are good.
	 */
	if (record[i].type & VL) {
	    int foundbad = 0;
	    int foundbroken = 0;
	    char volidbuf[256];

	    readentry(record[i].addr, &vlentry, &type);

	    if (!(vlentry.flags & VLF_RWEXISTS))
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Volume '%s' (%u) has no RW volume\n",
			  record[i].addr, OFFSET(record[i].addr), vlentry.name,
			  vlentry.volumeId[0]);

	    if (InvalidVolname(vlentry.name))
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Volume '%s' (%u) has an invalid name\n",
			  record[i].addr, OFFSET(record[i].addr), vlentry.name,
			  vlentry.volumeId[0]);

	    if (vlentry.volumeId[0] == 0)
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Volume '%s' (%u) has an invalid volume id\n",
			  record[i].addr, OFFSET(record[i].addr), vlentry.name,
			  vlentry.volumeId[0]);

	    if (!is_kv()) {
		if (!(record[i].type & NH)) {
		    hash = NameHash(vlentry.name);
		    which = "name";
		    volidbuf[0]='\0';
		    foundbad = 1;
		}

		if (vlentry.volumeId[0] && !(record[i].type & RWH)) {
		    hash = IdHash(vlentry.volumeId[0]);
		    which = "RW";
		    sprintf(volidbuf, "id %u ", vlentry.volumeId[0]);
		    foundbad = 1;
		}

		if (vlentry.volumeId[1] && !(record[i].type & ROH)) {
		    hash = IdHash(vlentry.volumeId[1]);
		    which = "RO";
		    sprintf(volidbuf, "id %u ", vlentry.volumeId[1]);
		    foundbad = 1;
		}

		if (vlentry.volumeId[2] && !(record[i].type & BKH)) {
		    hash = IdHash(vlentry.volumeId[2]);
		    which = "BK";
		    sprintf(volidbuf, "id %u ", vlentry.volumeId[2]);
		    foundbad = 1;
		}

		if (!validVolumeAddr(vlentry.nextNameHash) ||
		    record[ADDR(vlentry.nextNameHash)].type & MULTN) {
		    hash = NameHash(vlentry.name);
		    which = "name";
		    volidbuf[0]='\0';
		    if (validVolumeAddr(vlentry.nextNameHash)) {
			readentry(vlentry.nextNameHash, &vlentry2, &type);
			nexthash = NameHash(vlentry2.name);
		    } else {
			nexthash = 0xFFFFFFFF;
		    }
		    if (hash != nexthash)
			foundbroken = 1;
		}

		if (!validVolumeAddr(vlentry.nextIdHash[0]) ||
		    record[ADDR(vlentry.nextIdHash[0])].type & MULTRW) {
		    hash = IdHash(vlentry.volumeId[0]);
		    which = "RW";
		    sprintf(volidbuf, "id %u ", vlentry.volumeId[0]);
		    if (validVolumeAddr(vlentry.nextIdHash[0])) {
			readentry(vlentry.nextIdHash[0], &vlentry2, &type);
			nexthash = IdHash(vlentry2.volumeId[0]);
		    } else {
			nexthash = 0xFFFFFFFF;
		    }
		    if (hash != nexthash)
			foundbroken = 1;
		}

		if (!validVolumeAddr(vlentry.nextIdHash[1]) ||
		    record[ADDR(vlentry.nextIdHash[1])].type & MULTRO) {
		    hash = IdHash(vlentry.volumeId[1]);
		    which = "RO";
		    sprintf(volidbuf, "id %u ", vlentry.volumeId[1]);
		    if (validVolumeAddr(vlentry.nextIdHash[1])) {
			readentry(vlentry.nextIdHash[1], &vlentry2, &type);
			nexthash = IdHash(vlentry2.volumeId[1]);
		    } else {
			nexthash = 0xFFFFFFFF;
		    }
		    if (hash != nexthash)
			foundbroken = 1;
		}

		if (!validVolumeAddr(vlentry.nextIdHash[2]) ||
		    record[ADDR(vlentry.nextIdHash[2])].type & MULTBK) {
		    hash = IdHash(vlentry.volumeId[2]);
		    which = "BK";
		    sprintf(volidbuf, "id %u ", vlentry.volumeId[2]);
		    if (validVolumeAddr(vlentry.nextIdHash[2])) {
			readentry(vlentry.nextIdHash[2], &vlentry2, &type);
			nexthash = IdHash(vlentry2.volumeId[2]);
		    } else {
			nexthash = 0xFFFFFFFF;
		    }
		    if (hash != nexthash)
			foundbroken = 1;
		}

		if (foundbroken) {
		    log_error(VLDB_CHECK_ERROR,
			      "address 0x%llx (offset 0x%llx): Volume '%s' %s forward "
			      "link in %s hash chain is broken (hash %d != %d)\n",
			      record[i].addr, OFFSET(record[i].addr), vlentry.name,
			      volidbuf, which, hash, nexthash);

		} else if (foundbad) {
		    log_error(VLDB_CHECK_ERROR,
			      "address 0x%llx (offset 0x%llx): Volume '%s' %snot "
			      "found in %s hash %d\n",
			      record[i].addr, OFFSET(record[i].addr), vlentry.name,
			      volidbuf, which, hash);
		}
	    }

	    for (j = 0; j < NMAXNSERVERS; j++) {
		if (vlentry.serverNumber[j] != BADSERVERID) {
		    serverref[vlentry.serverNumber[j]] = 1;
		    if (serveraddrs[vlentry.serverNumber[j]] == 0) {
			log_error(VLDB_CHECK_ERROR,
				  "address 0x%llx (offset 0x%llx): Volume '%s', "
				  "index %d points to empty server entry %d\n",
				  record[i].addr, OFFSET(record[i].addr),
				  vlentry.name, j, vlentry.serverNumber[j]);

		    } else if (serverxref[vlentry.serverNumber[j]] != BADSERVERID) {
			    log_error(VLDB_CHECK_ERROR,
				      "address 0x%llx (offset 0x%llx): Volume '%s', "
				      "index %d points to server entry %d, "
				      "which is cross-linked by %d\n",
				      record[i].addr, OFFSET(record[i].addr),
				      vlentry.name, j, vlentry.serverNumber[j],
				      serverxref[vlentry.serverNumber[j]]);
		    }
		}
	    }

	    if (!is_kv()) {
		if (record[i].type & 0xffff0f00)
		    log_error(VLDB_CHECK_ERROR,
			      "address 0x%llx (offset 0x%llx): Volume '%s' id %u also "
			      "found on other chains (0x%x)\n",
			      record[i].addr, OFFSET(record[i].addr), vlentry.name, vlentry.volumeId[0], record[i].type);
	    }

	    /* A free entry */
	} else if (record[i].type & FR) {
	    if (!(record[i].type & FRC))
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Free vlentry not on free chain\n",
			  record[i].addr, OFFSET(record[i].addr));

	    if (record[i].type & 0xfffffdf0)
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Free vlentry also "
			  "found on other chains (0x%x)\n",
			  record[i].addr, OFFSET(record[i].addr),
			  record[i].type);

	    /* A multihomed entry */
	} else if (record[i].type & MH) {
	    if (!(record[i].type & MHC))
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Multihomed block is orphaned\n",
			  record[i].addr, OFFSET(record[i].addr));

	    if (record[i].type & 0xfffffef0)
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Multihomed block also "
			  "found on other chains (0x%x)\n",
			  record[i].addr, OFFSET(record[i].addr), record[i].type);

	} else {
	    log_error(VLDB_CHECK_ERROR,
		      "address 0x%llx (offset 0x%llx): Unknown entry type 0x%x\n",
		      record[i].addr, OFFSET(record[i].addr), record[i].type);
	}
    }

    if (fix) {
	/*
	 * If we are fixing we will rebuild the free and hash lists from the ground up.
	 */
	header.vital_header.freePtr = 0;
	memcpy(oldnamehash, header.VolnameHash, sizeof(oldnamehash));
	memset(header.VolnameHash, 0, sizeof(header.VolnameHash));

	memcpy(oldidhash, header.VolidHash, sizeof(oldidhash));
	memset(header.VolidHash, 0, sizeof(header.VolidHash));

	if (is_kv()) {
	    clear_kvhash();
	}

	quiet_println("Rebuilding %llu entries\n", maxentries);
    } else {
	quiet_println("Scanning %llu entries for possible repairs\n",
		      maxentries);
    }
    for (i = 0; i < maxentries; i++) {
	afs_uint32 hash;
	if (record[i].type & VL) {
	    readentry(record[i].addr, &vlentry, &type);
	    if (!(record[i].type & REFN)) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Record is not in a "
			  "name chain (type 0x%0x)\n",
			  record[i].addr, OFFSET(record[i].addr),
			  record[i].type);
	    }
	    if (vlentry.volumeId[0] && !(record[i].type & REFRW)) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Record not in a RW "
			  "chain (type 0x%0x)\n",
			  record[i].addr, OFFSET(record[i].addr),
			  record[i].type);
	    }
	    if (vlentry.volumeId[1] && !(record[i].type & REFRO)) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Record not in a RO "
			  "chain (type 0x%0x)\n",
			  record[i].addr, OFFSET(record[i].addr),
			  record[i].type);
	    }
	    if (vlentry.volumeId[2] && !(record[i].type & REFBK)) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Record not in a BK "
			  "chain (type 0x%0x)\n",
			  record[i].addr, OFFSET(record[i].addr),
			  record[i].type);
	    }
	    if ((vlentry.LockTimestamp == 0 && (vlentry.flags & VLOP_ALLOPERS) != 0) ||
		(vlentry.LockTimestamp != 0 && (vlentry.flags & VLOP_ALLOPERS) == 0)) {
		log_error(VLDB_CHECK_ERROR,
			  "address 0x%llx (offset 0x%llx): Lock inconsistency "
			  "in volume '%s'; timestamp %d, lock flags 0x%0x\n",
			  record[i].addr, OFFSET(record[i].addr), vlentry.name,
			  vlentry.LockTimestamp,
			  (vlentry.flags & VLOP_ALLOPERS));
	    }

	    if (fix) {
		afs_uint32 oldhash, newhash;
		char oldNameBuffer[10 + VL_MAXNAMELEN];
		char newNameBuffer[10 + VL_MAXNAMELEN];
		char *oldname, *newname;

		/* Fix broken names and numbers so entries can be inspected and deleted. */
		if (InvalidVolname(vlentry.name)) {
		    char bogus[VL_MAXNAMELEN];
		    memset(bogus, 0, sizeof(bogus));

		    /*
		     * Note that we only use the lower 32 bits of the "address"
		     * for our bogus volume name. For KV, our address is
		     * actually a 64-bit key (translated from net-byte-order).
		     * We don't use the full 64-bit key in our volume name
		     * here, because that would be really big. But if we take
		     * just the lower 32 bits, we'll get the RW id of the volid
		     * key, which is a good number to use.
		     *
		     * For non-KV, we can't use addresses above the 32-bit
		     * limit anyway, so just using the lower 32 bits doesn't
		     * matter.
		     */
		    snprintf(bogus, sizeof(bogus)-1, ".bogus.%u", (afs_uint32)record[i].addr);
		    strcpy(vlentry.name, bogus);
		    quiet_println("FIX: Record %llx invalid volume name set to '%s'\n",
				  record[i].addr, bogus);
		}
		if (vlentry.volumeId[0] == 0) {
		    afs_uint32 next_volid = header.vital_header.MaxVolumeId++;
		    vlentry.volumeId[0] = next_volid;
		    quiet_println("FIX: Record %llx invalid volume id set to %u. "
				  "New max volid is %u\n",
				  record[i].addr, next_volid,
				  header.vital_header.MaxVolumeId);
		}

		if (!is_kv()) {
		    /*
		     * Put the current hash table contexts into our 'next'
		     * and our address into the hash table.
		     */
		    hash = NameHash(vlentry.name);

		    if (vlentry.nextNameHash != header.VolnameHash[hash]) {
			oldname = nameForAddr(vlentry.nextNameHash, MAXTYPES, &oldhash, oldNameBuffer);
			newname = nameForAddr(header.VolnameHash[hash], MAXTYPES, &newhash, newNameBuffer);
			if (verbose || ((oldhash != newhash) &&
					(0 != vlentry.nextNameHash) &&
					(0 != header.VolnameHash[hash]))) {
			    /*
			     * That is, only report if we are verbose
			     * or the hash is changing (and one side wasn't NULL
			     */
			    quiet_println("FIX: Name hash link for '%s' was %s, is now %s\n",
				  vlentry.name, oldname, newname);
			}
		    }

		    vlentry.nextNameHash = header.VolnameHash[hash];
		    header.VolnameHash[hash] = record[i].addr;

		    for (j = 0; j < MAXTYPES; j++) {

			if (0 == vlentry.volumeId[j]) {
			    /*
			     * No volume of that type.  Continue
			     */
			    continue;
			}
			hash = IdHash(vlentry.volumeId[j]);

			if (vlentry.nextIdHash[j] != header.VolidHash[j][hash]) {
			    oldname = nameForAddr(vlentry.nextIdHash[j], j, &oldhash, oldNameBuffer);
			    newname = nameForAddr(header.VolidHash[j][hash], j, &newhash, newNameBuffer);
			    if (verbose || ((oldhash != newhash) &&
					    (0 != vlentry.nextIdHash[j]) &&
					    (0 != header.VolidHash[j][hash]))) {
				quiet_println("FIX: %s hash link for '%s' was %s, is now %s\n",
					      vtype(j), vlentry.name, oldname, newname);
			    }
			}

			/* Consolidate server numbers which point to the same mh entry.
			 * The serverref flags are not reset here, since we want to make
			 * sure the data is actually written before the server number is
			 * considered unreferenced. */
			for (k = 0; k < NMAXNSERVERS; k++) {
			    if (vlentry.serverNumber[k] != BADSERVERID
				&& serverxref[vlentry.serverNumber[k]] != BADSERVERID) {
				u_char oldsn = vlentry.serverNumber[k];
				u_char newsn = serverxref[oldsn];
				if (newsn == oldsn) {
				    log_error(VLDB_CHECK_ERROR,
					      "INTERNAL VLDB_CHECK_ERROR: "
					      "serverxref points to self; index %d\n",
					      oldsn);
				} else if (header.IpMappedAddr[oldsn] == 0) {
				    log_error(VLDB_CHECK_ERROR,
					      "INTERNAL VLDB_CHECK_ERROR: "
					      "serverxref; points to empty address; "
					      "index %d, value %d\n",
					      oldsn, newsn);
				} else if (header.IpMappedAddr[newsn] != header.IpMappedAddr[oldsn]) {
				    log_error(VLDB_CHECK_ERROR,
					      "INTERNAL VLDB_CHECK_ERROR: invalid "
					      "serverxref; index %d\n",
					      oldsn);
				} else {
				    quiet_println("FIX: Volume '%s', index %d, "
						  "server number was %d, is now %d\n",
						  vlentry.name, k, oldsn, newsn);
				    vlentry.serverNumber[k] = newsn;
				}
			    }
			}

			vlentry.nextIdHash[j] = header.VolidHash[j][hash];
			header.VolidHash[j][hash] = record[i].addr;
		    }
		}

		/*
		 * Fix lock timestamp and flag inconsistencies.
		 */
		if (vlentry.LockTimestamp == 0 && (vlentry.flags & VLOP_ALLOPERS) != 0) {
		    quiet_println("FIX: Record %llx: Inconsistent lock in "
				  "volume %s; setting timestamp.\n",
				  record[i].addr, vlentry.name);
		    vlentry.LockTimestamp = time(NULL);
		}
		if (vlentry.LockTimestamp != 0 && (vlentry.flags & VLOP_ALLOPERS) == 0) {
		    quiet_println("FIX: Record %llx: Inconsistent lock in "
				  "volume %s; setting misc lock flag.\n",
				  record[i].addr, vlentry.name);
		    vlentry.flags |= VLOP_DELETE; /* Shown by vos as "delete/misc". */
		}

		writeentry(record[i].addr, &vlentry);
	    }
	}
	else if (record[i].type & MH) {
	    int block, index;
	    char mhblock[VL_ADDREXTBLK_SIZE];
	    struct extentaddr *MHblock = (struct extentaddr *)mhblock;

	    if (fix) {
		for (block = 0; block < VL_MAX_ADDREXTBLKS; block++) {
		    if (mhinfo[block].addr == record[i].addr)
			break;
		}
		if (block == VL_MAX_ADDREXTBLKS) {
		    continue;  /* skip orphaned extent block */
		}
		readMH(record[i].addr, block, MHblock);
		for (index = 0; index < VL_MHSRV_PERBLK; index++) {
		    if (mhinfo[block].orphan[index]) {
			quiet_println("FIX: Removing unreferenced mh entry; "
				      "block %d, index %d\n",
				      block, index);
			memset(&(MHblock[index]), 0, sizeof(struct extentaddr));
		    }
		}
		writeMH(record[i].addr, block, MHblock);
	    }
	} else if (record[i].type & FR) {
	    if (fix) {
		readentry(record[i].addr, &vlentry, &type);
		vlentry.nextIdHash[0] = header.vital_header.freePtr;
		header.vital_header.freePtr = record[i].addr;
		if ((record[i].type & FRC) == 0) {
		    quiet_println("FIX: Putting free entry on the free chain: "
				  "addr=%llx (offset 0x%llx)\n",
				  record[i].addr, OFFSET(record[i].addr));
		}
		writeentry(record[i].addr, &vlentry);
	    }
	}
    }
    if (fix) {
	if (!is_kv()) {
	    reportHashChanges(&header, oldnamehash, oldidhash);
	}
	removeCrossLinkedAddresses(&header);
	writeheader(&header);
    }

    if (!is_kv()) {
	opr_Assert(fd >= 0);
	close(fd);
	fd = -1;
    } else {
	code = okv_commit(&kvtx);
	if (code != 0) {
	    log_error(VLDB_CHECK_FATAL, "error: okv_commit failed: %d\n", code);
	}
	okv_close(&dbh);
    }

    return error_level;
}

int
main(int argc, char **argv)
{
    struct cmd_syndesc *ts;

    setprogname(argv[0]);
    setlinebuf(stdout);

    ts = cmd_CreateSyntax(NULL, WorkerBee, NULL, 0, "vldb check");
    cmd_AddParm(ts, "-database", CMD_SINGLE, CMD_REQUIRED, "vldb_file");
    cmd_AddParm(ts, "-uheader", CMD_FLAG, CMD_OPTIONAL,
		"Display UBIK header");
    cmd_AddParm(ts, "-vheader", CMD_FLAG, CMD_OPTIONAL,
		"Display VLDB header");
    cmd_AddParm(ts, "-servers", CMD_FLAG, CMD_OPTIONAL,
		"Display server list");
    cmd_AddParm(ts, "-entries", CMD_FLAG, CMD_OPTIONAL, "Display entries");
    cmd_AddParm(ts, "-verbose", CMD_FLAG, CMD_OPTIONAL, "verbose");
    cmd_AddParm(ts, "-quiet", CMD_FLAG, CMD_OPTIONAL, "quiet");
    cmd_AddParm(ts, "-fix", CMD_FLAG, CMD_OPTIONAL, "attempt to patch the database (potentially dangerous)");

    return cmd_Dispatch(argc, argv);
}

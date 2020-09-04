/*
 * Copyright (c) 2020 Sine Nomine Associates
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

#include <afs/okv.h>

#include "ubik_internal.h"

#include "common.h"
#include "vltest.h"

#define KVBUF(str) { \
    .len = sizeof(str)-1, \
    .val = (str), \
}

/* Represents a modification/corruption to a vldb. */
struct vlmod {
    char *descr;

    /* key to modify */
    struct rx_opaque key;

    /* modify just a portion of the value */
    int offset;
    struct rx_opaque old_buf;
    struct rx_opaque new_buf;

    /* replace (or add or delete) the entire value */
    struct rx_opaque old_val;	/**< if NULL, add a new item */
    struct rx_opaque new_val;	/**< if NULL, delete the existing item */

    int check_code;	/**< Running vldb_check should result in this exit
			 *   code. */
    char *check_output;	/**< Running vldb_check should result in in this
			 *   output. */

    int skip_rerun;	/**< Don't run vldb_check again after running with
			 *   -fix. */
    int skip_fix;	/**< Just run the tests for vldb_check (without -fix);
			 *   don't run anything against the actual vlserver. */
    int only_corrupt;	/**< Don't run vldb_check -fix, but still run the
			 *   dbtest_corrupt tests. */

    int dbtest_corrupt_force;
			/**< Run the vlserver against the corrupted vldb, even
			 *   if dbtest_corrupt is empty. */

    /*
     * After corrupting the vldb, run these tests instead of the normal dataset
     * tests. We use this to verify that we've actually corrupted the vldb
     * correctly.
     */
    struct ubiktest_dbtest dbtest_corrupt[4];
    struct ubiktest_dbtest _dbtest_corrupt_nul;

    /*
     * After fixing the vldb with vldb_check -fix, run these extra tests (in
     * addition to the normal dataset tests) against the running vlserver.
     */
    struct ubiktest_dbtest dbtest_fix[4];
    struct ubiktest_dbtest _dbtest_fix_nul;
};

static void
rename_bogus_rootafs(char *dirname)
{
    vltest_renamevol(".bogus.536870912", "root.afs");
}

static struct vlmod vlsmall_mods[] = {
    {
	.descr = "normal db",
	.check_code = 0,
	.check_output = "",
    },
    {
	.descr = "bad vl header size",
	.key = KVBUF("\x04\x68\x64\x72"),
	.offset = 4,
	.old_buf = KVBUF("\x00\x00\x04\x28"),
	.new_buf = KVBUF("\x00\x00\x04\x27"),

	.check_code = 1,
	.check_output =
	    "Header reports its size as 1063 (should be 1064)\n",
	.skip_rerun = 1,
    },
    {
	.descr = "root.afs invalid serverFlag",
	.key = KVBUF("\x04\x56\x49\x44\x20\x00\x00\x00"),
	.offset = 0x87,
	.old_buf = KVBUF("\x04"),
	.new_buf = KVBUF("\x80"),

	.check_code = 2,
	.check_output =
	    "address 0x456494420000000 (offset 0x456494420000040): VLDB entry 'root.afs' contains an unknown RW/RO index serverFlag\n",

	/*
	 * Note that -fix doesn't fix this (we don't know what to set the
	 * serverFlags to). Running with the corrupted vldb will cause the
	 * output for root.afs to be wrong (since the relevant site doesn't
	 * appear as a normal RW site anymore), so just skip running the
	 * vlserver against this corrupted db.
	 */
	.skip_fix = 1,
    },
    {
	.descr = "IpMappedAddr specifies unused mh block",
	.key = KVBUF("\x04\x68\x64\x72"),
	.offset = 0x30,
	.old_buf = KVBUF("\0\0\0\0"),
	.new_buf = KVBUF("\xff\0\0\x03"),

	.check_code = 2,
	.check_output =
	    "Server Addrs index 2 references null MH block 0, index 3\n",
	.skip_fix = 1,

	/*
	 * The vlserver doesn't really notice this issue on its own (we'd
	 * probably have to have an entry referencing the IpMappedAddr entry;
	 * creating that corruption is more complex than we bother with right
	 * now).
	 */
    },
    {
	.descr = "mh entry has no ip addrs",
	.key = KVBUF("\x04\x45\x58\x62\x00\x00\x00\x00"),
	.offset = 0x114,
	.old_buf = KVBUF("\x0a\0\0\x02"),
	.new_buf = KVBUF("\0\0\0\0"),

	.check_code = 2,
	.check_output =
	    "warning: IP Addr for entry 1: Multihome entry has no ip addresses\n"
	    "address 0x456494420000006 (offset 0x456494420000046): Volume 'vol.691719c1', index 0 points to empty server entry 1\n"
	    "address 0x456494420002002 (offset 0x456494420002042): Volume 'vol.bigid', index 0 points to empty server entry 1\n",

	/*
	 * -fix doesn't fix this (it doesn't know what IP to put in the entry).
	 * But we can at least check that we are performing the corruption we
	 * think we are, by checking that the IP for a related volume looks weird.
	 */
	.only_corrupt = 1,
	.dbtest_corrupt = {
	    {
		.descr = "lookup vol.bigid shows 0.0.0.0",
		.cmd_args = "listvldb -name vol.bigid",
		.cmd_stdout =
		    "\n"
		    "vol.bigid \n"
		    "    RWrite: 536879106 \n"
		    "    number of sites -> 1\n"
		    "       server 0.0.0.0 partition /vicepa RW Site \n"
	    },
	},
    },
    {
	.descr = "orphan mh entry",
	.key = KVBUF("\x04\x45\x58\x62\x00\x00\x00\x00"),
	.offset = 0x180,
	.old_buf = KVBUF("\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"),
	.new_buf = KVBUF("\xce\x38\x7b\x42\x9e\x84\x45\x9f\x83\x81\x23\x07\x12\xed\x4c\xb6\x00\x00\x00\x01\x0a\x00\x00\x03"),

	.check_code = 2,
	.check_output =
	    "MH block 0, index 3: Not referenced by server addrs\n",

	/*
	 * No .dbtest_corrupt/.dbtest_fix checks for this; we can't really
	 * access the mh entry if it's not referenced in the IpMappedAddr
	 * array (the VL_* RPCs never iterate or do lookups for mh entries
	 * directly; we only lookup mh entries when an entry in IpMappedAddr
	 * references an mh entry).
	 */
    },
    {
	.descr = "IpMappedAddr specifies invalid mh block number",
	.key = KVBUF("\x04\x68\x64\x72"),
	.offset = 0x30,
	.old_buf = KVBUF("\0\0\0\0"),
	.new_buf = KVBUF("\xff\x09\0\x01"),

	.check_code = 2,
	.check_output =
	    "IP Addr for entry 2: Multihome block is bad (9)\n"
	    "IP Addr for entry 2: No such multihome block (9)\n",
	.only_corrupt = 1,

	/*
	 * vldb_check doesn't fix this, so we only check the corrupted db. To
	 * check that we are corrupting the db in the way that we expect, try
	 * to run a 'changeaddr -remove' against a nonexistent IP address. This
	 * will cause the vlserver to iterate over all known mh blocks, and
	 * it'll throw an error when it encounters the invalid mh block.
	 *
	 * We can't simply do a lookup for an IP address, since for those, the
	 * vlserver translates any error during mh block lookup into VL_NOENT,
	 * which is the normal error we'd expect for a nonexistent address.
	 */
	.dbtest_corrupt = {
	    {
		.descr = "changeaddr -remove 10.0.0.100 yields VL_IO",
		.cmd_args = "changeaddr -oldaddr 10.0.0.100 -remove",
		.cmd_auth = 1,
		.cmd_exitcode = 255,
		.cmd_stderr =
		    "Could not remove server 10.0.0.100 from the VLDB\n"
		    "VLDB: a read terminated too early\n",
	    },
	},
    },
    {
	.descr = "IpMappedAddr specifies nonexisting mh block",
	.key = KVBUF("\x04\x68\x64\x72"),
	.offset = 0x30,
	.old_buf = KVBUF("\0\0\0\0"),
	.new_buf = KVBUF("\xff\x02\0\x01"),

	.check_code = 2,
	.check_output =
	    "IP Addr for entry 2: No such multihome block (2)\n",
	.only_corrupt = 1,

	/* See the 'invalid mh block number' test above for details. */

	.dbtest_corrupt = {
	    {
		.descr = "changeaddr -remove 10.0.0.100 yields VL_IO",
		.cmd_args = "changeaddr -oldaddr 10.0.0.100 -remove",
		.cmd_auth = 1,
		.cmd_exitcode = 255,
		.cmd_stderr =
		    "Could not remove server 10.0.0.100 from the VLDB\n"
		    "VLDB: a read terminated too early\n",
	    },
	},
    },
    {
	.descr = "IpMappedAddr specifies invalid mh index",
	.key = KVBUF("\x04\x68\x64\x72"),
	.offset = 0x30,
	.old_buf = KVBUF("\0\0\0\0"),
	.new_buf = KVBUF("\xff\0\0\x70"),

	.check_code = 2,
	.check_output =
	    "IP Addr for entry 2: Multihome index is bad (112)\n",
	.only_corrupt = 1,

	/* See the 'invalid mh block number' test above for details. */

	.dbtest_corrupt = {
	    {
		.descr = "changeaddr -remove 10.0.0.100 yields VL_IO",
		.cmd_args = "changeaddr -oldaddr 10.0.0.100 -remove",
		.cmd_auth = 1,
		.cmd_exitcode = 255,
		.cmd_stderr =
		    "Could not remove server 10.0.0.100 from the VLDB\n"
		    "VLDB: a read terminated too early\n",
	    },
	},
    },
    {
	.descr = "mh index cross-linked",
	.key = KVBUF("\x04\x68\x64\x72"),
	.offset = 0x30,
	.old_buf = KVBUF("\0\0\0\0"),
	.new_buf = KVBUF("\xff\0\0\x01"),

	.check_code = 1,
	.check_output =
	    "warning: MH block 0, index 1 is cross-linked by server numbers 2 and 0.\n",

	.dbtest_corrupt = {
	    {
		.descr = "vos listaddrs shows duplicate 10.0.0.1 entry",
		.cmd_args = "listaddrs -printuuid",
		.cmd_stdout =
		    "UUID: 5dafb2eb-77d0-4b42-ac-cb-e54c3b6db53a\n"
		    "10.0.0.1\n"
		    "\n"
		    "UUID: ce387b42-9e84-459f-83-81-230712ed4cb5\n"
		    "10.0.0.2\n"
		    "\n"
		    "UUID: 5dafb2eb-77d0-4b42-ac-cb-e54c3b6db53a\n"
		    "10.0.0.1\n"
		    "\n"
	    },
	},
    },
    {
	.descr = "root.afs is missing VLF_RWEXISTS",
	.key = KVBUF("\x04\x56\x49\x44\x20\x00\x00\x00"),
	.offset = 0x0c,
	.old_buf = KVBUF("\0\0\x30\0"),
	.new_buf = KVBUF("\0\0\x20\0"),

	.check_code = 2,
	.check_output =
	    "address 0x456494420000000 (offset 0x456494420000040): Volume 'root.afs' (536870912) has no RW volume\n",
	.only_corrupt = 1,

	.dbtest_corrupt = {
	    {
		.descr = "root.afs lacks rw id",
		.cmd_args = "listvldb -name root.afs",
		.cmd_stdout =
		    "\n"
		    "root.afs \n"
		    "    ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n",
	    },
	},
    },
    {
	.descr = "root.afs changed to invalid name root+afs",
	.key = KVBUF("\x04\x56\x49\x44\x20\x00\x00\x00"),
	.offset = 0x2c,
	.old_buf = KVBUF("root.afs"),
	.new_buf = KVBUF("root+afs"),

	.check_code = 2,
	.check_output =
	    "key for volume name root.afs points to rwid 536870912 with name root+afs\n"
	    "address 0x456494420000000 (offset 0x456494420000040): Volume 'root+afs' (536870912) has an invalid name\n"
	    "address 0x456494420000000 (offset 0x456494420000040): Record is not in a name chain (type 0x3031)\n",

	.dbtest_corrupt = {
	    {
		.descr = "volid 536870912 shows up as root+afs",
		.cmd_args = "listvldb -name 536870912",
		.cmd_stdout =
		    "\n"
		    "root+afs \n"
		    "    RWrite: 536870912     ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "rename .bogus.536870912 back to root.afs",
		.func = rename_bogus_rootafs,
	    },
	},
    },
    {
	.descr = "root.afs set to invalid rw volid 0",
	.key = KVBUF("\x04\x56\x49\x44\x20\x00\x00\x00"),
	.offset = 0,
	.old_buf = KVBUF("\x20\0\0\0"),
	.new_buf = KVBUF("\0\0\0\0"),

	.check_code = 2,
	.check_output =
	    "RW volid key 536870912 points to vlentry with RW volid 0\n"
	    "address 0x456494420000000 (offset 0x456494420000040): Volume 'root.afs' (0) has an invalid volume id\n",

	.dbtest_corrupt = {
	    {
		.descr = "root.afs has rwid 0",
		.cmd_args = "listvldb -name root.afs",
		.cmd_stdout =
		    "\n"
		    "root.afs \n"
		    "    RWrite: 0             ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n",
	    },
	},
    },
    {
	.descr = "namekey root.afs points to RO volid",
	.key = KVBUF("\x04\x6e\x61\x6droot.afs"),
	.old_val = KVBUF("\x20\0\0\0"),
	.new_val = KVBUF("\x20\0\0\x01"),

	.check_code = 2,
	.check_output =
	    "rw volume 536870912 has name root.afs, which points to different volume rw id 536870913\n"
	    "name key root.afs points to volid 536870913, which is a non-RW volid (pointing to 536870912)\n"
	    "address 0x456494420000000 (offset 0x456494420000040): Record is not in a name chain (type 0x3031)\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup root.afs results in VL_IO",
		.cmd_args = "listvldb -name root.afs",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "VLDB: a read terminated too early\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup root.afs succeeds",
		.cmd_args = "listvldb -name root.afs",
		.cmd_exitcode = 0,
		.cmd_stdout =
		    "\n"
		    "root.afs \n"
		    "    RWrite: 536870912     ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n",

	    },
	},
    },
    {
	.descr = "namekey vol.foo points to nonexistent volume",
	.key = KVBUF("\x04\x6e\x61\x6dvol.foo"),
	.new_val = KVBUF("\x20\xff\0\0"),

	.check_code = 2,
	.check_output =
	    "name key vol.foo refers to non-existent rw id 553582592\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup vol.foo results in UIOERROR",
		.cmd_args = "listvldb -name vol.foo",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "u: I/O error writing dbase or log\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup vol.foo results in VL_NOENT",
		.cmd_args = "listvldb -name vol.foo",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "namekey vol.foo points to root.afs",
	.key = KVBUF("\x04\x6e\x61\x6dvol.foo"),
	.new_val = KVBUF("\x20\0\0\0"),

	.check_code = 2,
	.check_output =
	    "key for volume name vol.foo points to rwid 536870912 with name root.afs\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup vol.foo results in UIOERROR",
		.cmd_args = "listvldb -name vol.foo",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "VLDB: bad incoming vldb entry\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup vol.foo results in VL_NOENT",
		.cmd_args = "listvldb -name vol.foo",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "idkey 536870913 points to 536870913",
	.key = KVBUF("\x04\x56\x49\x44\x20\0\0\x01"),
	.old_val = KVBUF("\x20\0\0\0"),
	.new_val = KVBUF("\x20\0\0\x01"),

	.check_code = 2,
	.check_output =
	    "rw volume 536870912 has volid 536870913, which points to different volume rw id 536870913\n"
	    "id key 536870913 points to volid 536870913, which is a non-RW volid (pointing to 536870913)\n"
	    "address 0x456494420000000 (offset 0x456494420000040): Record not in a RO chain (type 0x9091)\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup 536870913 results in VL_IO",
		.cmd_args = "listvldb -name 536870913",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "VLDB: a read terminated too early\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup 536870913 succeeds",
		.cmd_args = "listvldb -name 536870913",
		.cmd_exitcode = 0,
		.cmd_stdout =
		    "\n"
		    "root.afs \n"
		    "    RWrite: 536870912     ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n",

	    },
	},
    },
    {
	.descr = "idkey 553582593 points to nonexistent volume",
	.key = KVBUF("\x04\x56\x49\x44\x20\xff\x00\x01"),
	.new_val = KVBUF("\x20\xff\0\0"),

	.check_code = 2,
	.check_output =
	    "id key 553582593 refers to non-existent rw id 553582592\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup 553582593 results in UIOERROR",
		.cmd_args = "listvldb -name 553582593",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "u: I/O error writing dbase or log\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup 553582593 results in VL_NOENT",
		.cmd_args = "listvldb -name 553582593",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "extra idkey 553582593 points to root.afs",
	.key = KVBUF("\x04\x56\x49\x44\x20\xff\x00\x01"),
	.new_val = KVBUF("\x20\0\0\0"),

	.check_code = 2,
	.check_output =
	    "key volid 553582593 points to unrelated rwid 536870912\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup 553582593 results in UIOERROR",
		.cmd_args = "listvldb -name 553582593",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "VLDB: bad incoming vldb entry\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup 553582593 results in VL_NOENT",
		.cmd_args = "listvldb -name 553582593",
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "root.afs LockTimestamp is set without any lock flag",

	/* Change vlentry.LockTimestamp for root.afs to 0x60665502 */
	.key = KVBUF("\x04\x56\x49\x44\x20\x00\x00\x00"),
	.offset = 0x14,
	.old_buf = KVBUF("\0\0\0\0"),
	.new_buf = KVBUF("\x60\x66\x55\x02"),

	.check_code = 2,
	.check_output =
	    "address 0x456494420000000 (offset 0x456494420000040): Lock inconsistency in volume 'root.afs'; timestamp 1617319170, lock flags 0x0\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup root.afs shows unlocked entry",
		.cmd_args = "listvldb -name root.afs",
		.cmd_stdout =
		    "\n"
		    "root.afs \n"
		    "    RWrite: 536870912     ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n",
	    },
	    {
		.descr = "locking root.afs fails",
		.cmd_args = "lock -id root.afs",
		.cmd_auth = 1,
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "Could not lock VLDB entry for volume root.afs\n"
		    "VLDB: vldb entry is already locked\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup root.afs shows locked entry",
		.cmd_args = "listvldb -name root.afs",
		.cmd_stdout =
		    "\n"
		    "root.afs \n"
		    "    RWrite: 536870912     ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n"
		    "    Volume is currently LOCKED  \n"
		    "    Volume is locked for a delete/misc operation\n",
	    },
	    {
		.descr = "locking root.afs fails",
		.cmd_args = "lock -id root.afs",
		.cmd_auth = 1,
		.cmd_exitcode = 1,
		.cmd_stderr =
		    "Could not lock VLDB entry for volume root.afs\n"
		    "VLDB: vldb entry is already locked\n",
	    },
	    {
		.descr = "unlocking root.afs succeeds",
		.cmd_args = "unlock -id root.afs",
		.cmd_auth = 1,
		.cmd_stdout =
		    "Released lock on vldb entry for volume root.afs\n",
	    },
	    {
		.descr = "lookup root.afs shows unlocked entry (again)",
		.cmd_args = "listvldb -name root.afs",
		.cmd_stdout =
		    "\n"
		    "root.afs \n"
		    "    RWrite: 536870912     ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n",
	    },
	}
    },
    {0}
};

static void
apply_mod(char *db_path, struct vlmod *mod)
{
    int code;
    int flags = 0;
    struct okv_dbhandle *dbh;
    struct okv_trans *tx;
    struct rx_opaque newval;
    struct rx_opaque free_val = RX_EMPTY_OPAQUE;
    struct rx_opaque_stringbuf bufstr, bufstr2;

    if (mod->key.val == NULL) {
	return;
    }

    code = ukv_open(db_path, &dbh, NULL);
    opr_Assert(code == 0);

    code = okv_begin(dbh, OKV_BEGIN_RW, &tx);
    opr_Assert(code == 0);

    /* If old_buf/old_val is set, we need to read in the existing value. */
    if (mod->old_buf.val != NULL || mod->old_val.val != NULL) {
	struct rx_opaque got;
	struct rx_opaque val;
	struct rx_opaque expected;

	code = okv_get(tx, &mod->key, &val, NULL);
	if (code != 0) {
	    bail("okv_get(%s) failed with %d",
		 rx_opaque_stringify(&mod->key, &bufstr), code);
	}

	expected = mod->old_val;
	got = val;

	if (mod->old_buf.val != NULL) {
	    /* "seek" into the existing value, by 'offset' bytes. */
	    expected = mod->old_buf;

	    if (got.len < mod->offset + mod->old_buf.len) {
		bail("value for key %s too small: %s (must be at least %d+%d)",
		     rx_opaque_stringify(&mod->key, &bufstr),
		     rx_opaque_stringify(&got, &bufstr2),
		     mod->offset, (int)mod->old_buf.len);

	    } else {
		got.val = (char*)got.val + mod->offset;
		got.len = mod->old_buf.len;
	    }
	}

	opr_Assert(rx_opaque_cmp(&got, &expected) == 0);

	if (mod->old_buf.val != NULL) {
	    /* If we're just changing part of the value, we need to make a copy
	     * of the value to modify it. */
	    char *buf;

	    opr_Assert(mod->new_buf.val != NULL);
	    opr_Assert(mod->new_buf.len == mod->new_buf.len);

	    opr_Verify(rx_opaque_copy(&free_val, &val) == 0);
	    newval = free_val;

	    buf = newval.val;
	    buf += mod->offset;
	    memcpy(buf, mod->new_buf.val, mod->new_buf.len);

	} else {
	    opr_Assert(mod->old_val.val != NULL);
	    newval = mod->new_val;
	}
	flags = OKV_PUT_REPLACE;

    } else {
	newval = mod->new_val;
    }

    opr_Assert(newval.val != NULL);
    if (newval.val == NULL) {
	code = okv_del(tx, &mod->key, NULL);
	if (code != 0) {
	    bail("okv_del(%s) failed with %d",
		 rx_opaque_stringify(&mod->key, &bufstr), code);
	}
    } else {
	code = okv_put(tx, &mod->key, &newval, flags);
	if (code != 0) {
	    bail("okv_put(%s) failed with %d",
		 rx_opaque_stringify(&mod->key, &bufstr), code);
	}
    }

    code = okv_commit(&tx);
    if (code != 0) {
	bail("okv_commit failed with %d", code);
    }

    okv_close(&dbh);

    rx_opaque_freeContents(&free_val);
}

static void
run_vldb_check(char *db_path, struct vlmod *mod, int fix)
{
    char *cmd_nofix;
    char *cmd_fix;
    char *vldb_check;
    struct afstest_cmdinfo cmdinfo;

    vldb_check = afstest_obj_path("src/tvlserver/vldb_check");

    cmd_nofix = afstest_asprintf("%s -quiet '%s'", vldb_check, db_path);
    cmd_fix = afstest_asprintf("%s -quiet -fix '%s'", vldb_check, db_path);

    memset(&cmdinfo, 0, sizeof(cmdinfo));
    cmdinfo.exit_code = mod->check_code;
    cmdinfo.output = mod->check_output;
    cmdinfo.fd = STDERR_FILENO;
    if (fix) {
	cmdinfo.command = cmd_fix;
    } else {
	cmdinfo.command = cmd_nofix;
    }

    is_command(&cmdinfo, "vldb_check%s results: %s",
	       (fix ? " -fix": ""),
	       mod->descr);

    if (fix && !mod->skip_rerun) {
	/* After running with -fix, run again without -fix and verify the
	 * issues have gone away. */
	memset(&cmdinfo, 0, sizeof(cmdinfo));
	cmdinfo.exit_code = 0;
	cmdinfo.output = "";
	cmdinfo.fd = STDERR_FILENO;
	cmdinfo.command = cmd_nofix;
	is_command(&cmdinfo, "vldb_check re-run: %s", mod->descr);
    }

    free(vldb_check);
    free(cmd_fix);
    free(cmd_nofix);
}

static void
run_fix(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct vlmod *mod = ops->rock;
    apply_mod(info->db_path, mod);
    run_vldb_check(info->db_path, mod, 1);
}

static void
corrupt_db(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct vlmod *mod = ops->rock;
    apply_mod(info->db_path, mod);
}

int
main(int argc, char **argv)
{
    char *src_db;
    char *dbname = "tests/vlserver/db.vlsmall/vldb4kv.lmdb";
    struct vlmod *mod;
    int code;

    vltest_init(argv);

    code = afstest_okv_dbpath(&src_db, dbname);
    if (code != 0) {
	afs_com_err(NULL, code, "while opening dbase %s", dbname);
	return 1;
    }
    if (src_db == NULL) {
	skip_all("okv dbase %s not available for this platform", dbname);
    }

    plan(418);

    /* Run the plain vldb_check tests (without -fix). */

    for (mod = vlsmall_mods; mod->descr != NULL; mod++) {
	char *dest_db = NULL;
	char *dirname = afstest_mkdtemp();
	opr_Assert(dirname != NULL);

	dest_db = afstest_asprintf("%s/vldb.DB0", dirname);
	opr_Verify(afstest_cp(src_db, dest_db) == 0);

	apply_mod(dest_db, mod);
	run_vldb_check(dest_db, mod, 0);

	free(dest_db);
	afstest_rmdtemp(dirname);
    }

    /* Test running vldb_check with -fix, and check the result against the
     * ubiktest tests. */

    for (mod = vlsmall_mods; mod->descr != NULL; mod++) {
	char *descr_corrupt = NULL;
	char *descr_fix = NULL;

	if (mod->skip_fix) {
	    continue;
	}

	descr_corrupt = afstest_asprintf("%s [corrupt]", mod->descr);
	descr_fix = afstest_asprintf("%s [-fix]", mod->descr);

	opr_Assert(mod->_dbtest_corrupt_nul.descr == NULL);
	opr_Assert(mod->_dbtest_fix_nul.descr == NULL);

	if (mod->dbtest_corrupt[0].descr != NULL || mod->dbtest_corrupt_force) {
	    struct ubiktest_ops ops = {
		.descr = descr_corrupt,
		.use_db = "vldb4-kv",
		.result_kv = 1,
		.pre_start = corrupt_db,
		.rock = mod,
		.override_dbtests = &mod->dbtest_corrupt[0],
	    };

	    ubiktest_runtest(&vlsmall, &ops);
	}

	if (!mod->only_corrupt) {
	    struct ubiktest_ops ops = {
		.descr = descr_fix,
		.use_db = "vldb4-kv",
		.result_kv = 1,
		.pre_start = run_fix,
		.rock = mod,
		.extra_dbtests = &mod->dbtest_fix[0],
	    };

	    ubiktest_runtest(&vlsmall, &ops);
	}
    }

    return 0;
}

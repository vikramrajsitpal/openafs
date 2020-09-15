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

#include "vltest.h"

/* Represents a modification/corruption to a vldb. */
struct vlmod {
    char *descr;

    off_t offset;
    size_t buf_size;
    char *old_buf;
    char *new_buf;

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
create_voltmp(char *dirname)
{
    struct vltest_voldef vol = {
	.name = "vol.tmp",
	.rwid = 536870924,
	.server = 0x0A000002,
    };
    vltest_createvol(&vol);
}

static void
rename_bogus_rootafs(char *dirname)
{
    vltest_renamevol(".bogus.140312", "root.afs");
}

static struct vlmod vlsmall_mods[] = {
    {
	.descr = "normal db",
	.check_code = 0,
	.check_output = "",
    },
    {
	.descr = "bad ubik header magic",
	.offset = 0,
	.buf_size = 4,
	.old_buf = "\0\x35\x45\x45",
	.new_buf = "\0\xff\x45\x45",

	/* Note that -fix doesn't actually fix this, but the vlserver doesn't
	 * care that it's wrong anyway. */
	.check_code = 2,
	.check_output =
	    "Ubik header magic is 0xff4545 (should be 0x354545)\n",
	.skip_rerun = 1,
    },
    {
	.descr = "bad ubik header size",
	.offset = 4,
	.buf_size = 4,
	.old_buf = "\0\0\0\x40",
	.new_buf = "\0\0\0\x41",

	/* Note that -fix doesn't actually fix this, but the vlserver doesn't
	 * care that it's wrong anyway. */
	.check_code = 1,
	.check_output =
	    "VLDB_CHECK_WARNING: Ubik header size is 65 (should be 64)\n",
	.skip_rerun = 1,
    },
    {
	.descr = "bad vl header size",
	.offset = 0x44,
	.buf_size = 4,
	.old_buf = "\0\x02\x04\x18",
	.new_buf = "\0\x02\x03\x84",

	.check_code = 2,
	/*
	 * Changing the header size also changes where vldb_check starts
	 * traversing entries. We reduce the header size by one nvlentry
	 * (0x94), so it finds an extra entry full of blanks.
	 */
	.check_output =
	    "Header reports its size as 131972 (should be 132120)\n"
	    "address 131972 (offset 0x203c4): VLDB entry '' contains an unknown RW/RO index serverFlag\n"
	    "address 131972 (offset 0x203c4): Volume '' (0) has no RW volume\n"
	    "address 131972 (offset 0x203c4): Volume '' (0) has an invalid name\n"
	    "address 131972 (offset 0x203c4): Volume '' (0) has an invalid volume id\n"
	    "address 131972 (offset 0x203c4): Volume '' not found in name hash 0\n"
	    "address 131972 (offset 0x203c4): Record is not in a name chain (type 0x1)\n",
	.skip_rerun = 1,
    },
    {
	.descr = "root.afs invalid serverFlag",
	.offset = 0x224df,
	.buf_size = 1,
	.old_buf = "\x04",
	.new_buf = "\x80",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): VLDB entry 'root.afs' contains an unknown RW/RO index serverFlag\n",

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
	.descr = "namehash header points to invalid addr",
	.offset = 0x464,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\xff\xff\xff",

	.check_code = 2,
	.check_output =
	    "Name Hash index 0 is out of range: 4294967295\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup vol.ad1 fails with VL_IO",
		.cmd_args = "listvldb -name vol.ad1",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: a read terminated too early\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup vol.ad1 fails with VL_NOENT",
		.cmd_args = "listvldb -name vol.ad1",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "namehash header points to exblock",
	.offset = 0x464,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x04\x18",

	.check_code = 2,
	.check_output =
	    "address 132120 (offset 0x20458): Name Hash 0: Not a vlentry\n",

	/*
	 * The vlserver doesn't really notice this corruption; we'll just try
	 * to traverse the namehash anyway as if we were pointed to a normal
	 * nvlentry (which typically means the name hash will just end there).
	 * So we don't define .dbtest_corrupt or .dbtest_fix checks in here,
	 * since the situation is pretty much the same as a prematurely ended
	 * namehash chain.
	 */
    },
    {
	.descr = "root.afs namehash ptr points to exblock",
	.offset = 0x22480,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x04\x18",

	.check_code = 2,
	.check_output =
	    "address 132120 (offset 0x20458): Name Hash 306: Not a vlentry\n",

	/*
	 * No .dbtest_corrupt or .dbtest_fix checks here, since the vlserver
	 * doesn't really notice a hash chain pointing to an exblock (see
	 * "namehash header points to exblock").
	 */
    },
    {
	.descr = "root.afs namehash ptr points to self",
	.offset = 0x22480,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x24\x18",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): Name Hash 306: volume name 'root.afs' is already in the name hash\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912 also found on other chains (0x8b0b1)\n",

	/*
	 * No .dbtest_corrupt check here, because running with this corrupted
	 * db causes the vlserver to go into an infinite loop when traversing
	 * the relevant hash chain.
	 */
	.dbtest_fix = {
	    {
		.descr = "lookup vol.d99 fails with VL_NOENT",
		.cmd_args = "listvldb -name vol.d99",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "root.afs namehash ptr invalid addr",
	.offset = 0x22480,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\xff\xff\xff",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): Name Hash forward link of 'root.afs' is out of range\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs'  forward link in name hash chain is broken (hash 306 != -1)\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912 also found on other chains (0x83031)\n"
	    "address 140312 (offset 0x22458): Record is not in a name chain (type 0x83031)\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup vol.d99 fails with VL_IO",
		.cmd_args = "listvldb -name vol.d99",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: a read terminated too early\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup vol.d99 fails with VL_NOENT",
		.cmd_args = "listvldb -name vol.d99",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "root.afs on wrong name chain",
	.offset = 0x928,
	.buf_size = 8,
	.old_buf = "\0\0\0\0\0\x02\x24\x18",
	.new_buf = "\0\x02\x24\x18\0\0\0\0",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): Name Hash 305: volume name 'root.afs': Incorrect name hash chain (should be in 306)\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912 also found on other chains (0x8b0b1)\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup root.afs fails with VL_NOENT",
		.cmd_args = "listvldb -name root.afs",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup root.afs by name",
		.cmd_args = "listvldb -name root.afs",
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
	.descr = "rwid hash header points to invalid addr",
	.offset = 0x8460,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\xff\xff\xff",

	.check_code = 2,
	.check_output =
	    "rw Hash index 0 is out of range: 4294967295\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup 8191 fails with VL_IO",
		.cmd_args = "listvldb -name 8191",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: a read terminated too early\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup 8191 fails with VL_NOENT",
		.cmd_args = "listvldb -name 8191",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "root.afs rwid hash ptr points to exblock",
	.offset = 0x22474,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x04\x18",

	.check_code = 2,
	.check_output =
	    "address 132120 (offset 0x20458): rw Id Hash 8: Not a vlentry\n",

	/*
	 * No .dbtest_corrupt or .dbtest_fix checks here, since the vlserver
	 * doesn't really notice a hash chain pointing to an exblock (see
	 * "namehash header points to exblock").
	 */
    },
    {
	.descr = "root.afs rwid hash ptr points to self",
	.offset = 0x22474,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x24\x18",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): rw Id Hash 8: volume name 'root.afs': Already in the hash table\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912 also found on other chains (0x1b0b1)\n",

	/*
	 * No .dbtest_corrupt check here, because running with this corrupted
	 * db causes the vlserver to go into an infinite loop when traversing
	 * the relevant hash chain. No .dbtest_fix check, since that would just
	 * be looking up root.afs, which the regular vltest checks already do.
	 */
    },
    {
	.descr = "root.afs rwid hash ptr invalid",
	.offset = 0x22474,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\xff\xff\xff",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): rw Id Hash forward link of 'root.afs' is out of range\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912  forward link in RW hash chain is broken (hash 8 != -1)\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912 also found on other chains (0x1a0a1)\n"
	    "address 140312 (offset 0x22458): Record not in a RW chain (type 0x1a0a1)\n",

	.dbtest_corrupt = {
	    {
		/* 536879103 is the rwid for root.afs (536870912) plus HASHSIZE (8191) */
		.descr = "lookup 536879103 fails with VL_IO",
		.cmd_args = "listvldb -name 536879103",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: a read terminated too early\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup 536879103 fails with VL_NOENT",
		.cmd_args = "listvldb -name 536879103",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "root.afs on wrong rwid chain",
	.offset = 0x847c,
	.buf_size = 8,
	.old_buf = "\0\0\0\0\0\x02\x24\x18",
	.new_buf = "\0\x02\x24\x18\0\0\0\0",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): rw Id Hash 7: volume name 'root.afs': Incorrect Id hash chain (should be in 8)\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912 also found on other chains (0x1b0b1)\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup 536870912 fails with VL_NOENT",
		.cmd_args = "listvldb -name 536870912",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup 536870912",
		.cmd_args = "listvldb -name 536870912",
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
	.descr = "point freechain to root.afs",
	.offset = 0x48,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x24\x18",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 022458): Free Chain 0: Not a free vlentry (0x1)\n",

	/*
	 * To check this corruption, create a new volume, and check if root.afs
	 * still exists. Creating the new volume will overwrite the entry for
	 * root.afs, since that's where the freechain is pointing.
	 */
	.dbtest_corrupt = {
	    {
		.descr = "create vol.tmp",
		.func = create_voltmp,
	    },
	    {
		.descr = "remove vol.tmp",
		.cmd_args = "delentry -id vol.tmp",
		.cmd_auth = 1,
		.cmd_stdout = "Deleted 1 VLDB entries\n",
	    },
	    {
		.descr = "lookup root.afs fails with VL_NOENT",
		.cmd_args = "listvldb -name root.afs",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "create vol.tmp",
		.func = create_voltmp,
	    },
	    {
		.descr = "remove vol.tmp",
		.cmd_args = "delentry -id vol.tmp",
		.cmd_auth = 1,
		.cmd_stdout = "Deleted 1 VLDB entries\n",
	    },
	    {
		.descr = "lookup root.afs",
		.cmd_args = "listvldb -name root.afs",
		.cmd_stdout =
		    "\n"
		    "root.afs \n"
		    "    RWrite: 536870912     ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n"
	    },
	    {
		.descr = "lookup vol.tmp.afs fails with VL_NOENT",
		.cmd_args = "listvldb -name vol.tmp",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
    },
    {
	.descr = "specify first exblock twice",
	.offset = 0x2046c,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x04\x18",

	.check_code = 2,
	.check_output =
	    "address 132120 (offset 0x20458): MH Blocks Chain 1: Already a MH block\n",
	.only_corrupt = 1,

	/*
	 * When the db is corrupted in this way, it causes vlserver to go
	 * through the rare 'extent_mod' code path for re-running readExtents.
	 * The db is then usable and appears fine, so we don't need any special
	 * tests here; just go through the normal vltest tests.
	 */
	.dbtest_corrupt_force = 1,
    },
    {
	.descr = "IpMappedAddr specifies unused mh block",
	.offset = 0x70,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\0\0\x03",

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
	.offset = 0x2056c,
	.buf_size = 4,
	.old_buf = "\x0a\0\0\x02",
	.new_buf = "\0\0\0\0",

	.check_code = 2,
	.check_output =
	    "warning: IP Addr for entry 1: Multihome entry has no ip addresses\n"
	    "address 140608 (offset 0x22580): Volume 'vol.691719c1', index 0 points to empty server entry 1\n"
	    "address 140756 (offset 0x22614): Volume 'vol.bigid', index 0 points to empty server entry 1\n",

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
	.offset = 0x205d8,
	.buf_size = 24,
	.old_buf = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
	.new_buf = "\xce\x38\x7b\x42\x9e\x84\x45\x9f\x83\x81\x23\x07\x12\xed\x4c\xb6\x00\x00\x00\x01\x0a\x00\x00\x03",

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
	.offset = 0x70,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\x09\0\x01",

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
	.offset = 0x70,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\x02\0\x01",

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
	.offset = 0x70,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\0\0\x70",

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
	.offset = 0x70,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\xff\0\0\x01",

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
	.offset = 0x22464,
	.buf_size = 4,
	.old_buf = "\0\0\x30\0",
	.new_buf = "\0\0\x20\0",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): Volume 'root.afs' (536870912) has no RW volume\n",
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
	.descr = "root.afs changed to invalid name root+83c",
	.offset = 0x22484,
	.buf_size = 8,

	/*
	 * Pick a name that has an invalid char, but is still on the same hash
	 * chain (just to keep the relevant scenario a bit simpler, and
	 * generate fewer errors).
	 */
	.old_buf = "root.afs",
	.new_buf = "root+83c",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): Volume 'root+83c' (536870912) has an invalid name\n",

	.dbtest_corrupt = {
	    {
		.descr = "volid 536870912 shows up as root+83c",
		.cmd_args = "listvldb -name 536870912",
		.cmd_stdout =
		    "\n"
		    "root+83c \n"
		    "    RWrite: 536870912     ROnly: 536870913 \n"
		    "    number of sites -> 2\n"
		    "       server 10.0.0.1 partition /vicepa RW Site \n"
		    "       server 10.0.0.1 partition /vicepa RO Site \n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "rename .bogus.140312 back to root.afs",
		.func = rename_bogus_rootafs,
	    },
	},
    },
    {
	.descr = "root.afs set to invalid rw volid 0",
	.offset = 0x22458,
	.buf_size = 4,
	.old_buf = "\x20\0\0\0",
	.new_buf = "\0\0\0\0",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): rw Id Hash 8: volume name 'root.afs': Incorrect Id hash chain (should be in 0)\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' (0) has an invalid volume id\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 0 also found on other chains (0x1b0b1)\n",

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
	.descr = "root.afs namehash ptr points to wrong entry root.cell",
	.offset = 0x22480,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x24\xac",

	.check_code = 2,
	.check_output =
	    "address 140460 (offset 0x224ec): Name Hash 306: volume name 'root.cell': Incorrect name hash chain (should be in 7485)\n"
	    "address 140460 (offset 0x224ec): Name Hash 7485: volume name 'root.cell' is already in the name hash\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs'  forward link in name hash chain is broken (hash 306 != 7485)\n"
	    "address 140460 (offset 0x224ec): Volume 'root.cell' id 536870915 also found on other chains (0x8b0b1)\n",

	/*
	 * We don't really notice this corruption via VL RPCs, since we're just
	 * pointing the namehash chain from root.afs to root.cell, where
	 * normally it would end at root.afs. So this doesn't cause any lookups
	 * to fail, so we don't have any .dbtest_corrupt or .dbtest_fix checks
	 * for this.
	 */
    },
    {
	.descr = "root.afs idhash ptr points to wrong entry root.cell",
	.offset = 0x22474,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\0\x02\x24\xac",

	.check_code = 2,
	.check_output =
	    "address 140460 (offset 0x224ec): rw Id Hash 8: volume name 'root.cell': Incorrect Id hash chain (should be in 11)\n"
	    "address 140460 (offset 0x224ec): rw Id Hash 11: volume name 'root.cell': Already in the hash table\n"
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912  forward link in RW hash chain is broken (hash 8 != 11)\n"
	    "address 140460 (offset 0x224ec): Volume 'root.cell' id 536870915 also found on other chains (0x1b0b1)\n",

	/*
	 * No .dbtest_corrupt or .dbtest_fix checks for this, since we don't
	 * really notice this particular corruption via VL RPCs.
	 */
    },
    {
	.descr = "root.afs not on namehash",
	.offset = 0x92c,
	.buf_size = 4,
	.old_buf = "\0\x02\x24\x18",
	.new_buf = "\0\0\0\0",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): Volume 'root.afs' not found in name hash 306\n"
	    "address 140312 (offset 0x22458): Record is not in a name chain (type 0x3031)\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup root.afs fails with VL_NOENT",
		.cmd_args = "listvldb -name root.afs",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup root.afs by name",
		.cmd_args = "listvldb -name root.afs",
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
	.descr = "root.afs not on rwid hash",
	.offset = 0x8480,
	.buf_size = 4,
	.old_buf = "\0\x02\x24\x18",
	.new_buf = "\0\0\0\0",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): Volume 'root.afs' id 536870912 not found in RW hash 8\n"
	    "address 140312 (offset 0x22458): Record not in a RW chain (type 0xa0a1)\n",

	.dbtest_corrupt = {
	    {
		.descr = "lookup 536870912 fails with VL_NOENT",
		.cmd_args = "listvldb -name 536870912",
		.cmd_exitcode = 1,
		.cmd_stderr = "VLDB: no such entry\n",
	    },
	},
	.dbtest_fix = {
	    {
		.descr = "lookup 536870912",
		.cmd_args = "listvldb -name 536870912",
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
	.descr = "root.afs LockTimestamp is set without any lock flag",

	/* Change vlentry.LockTimestamp for root.afs to 0x60665502 */
	.offset = 0x2246c,
	.buf_size = 4,
	.old_buf = "\0\0\0\0",
	.new_buf = "\x60\x66\x55\x02",

	.check_code = 2,
	.check_output =
	    "address 140312 (offset 0x22458): Lock inconsistency in volume 'root.afs'; timestamp 1617319170, lock flags 0x0\n",

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
    if (mod->old_buf != NULL) {
	int fd = -1;
	int nbytes;
	char *buf = calloc(1, mod->buf_size);
	opr_Assert(buf);

	fd = open(db_path, O_RDWR);
	opr_Assert(fd >= 0);

	/* Check that the existing db data is what we expect. */
	nbytes = pread(fd, buf, mod->buf_size, mod->offset);
	opr_Assert(nbytes == mod->buf_size);
	opr_Assert(memcmp(mod->old_buf, buf, mod->buf_size) == 0);

	nbytes = pwrite(fd, mod->new_buf, mod->buf_size, mod->offset);
	opr_Assert(nbytes == mod->buf_size);

	opr_Verify(close(fd) == 0);
	fd = -1;
	free(buf);
    }
}

static void
run_vldb_check(char *db_path, struct vlmod *mod, int fix)
{
    char *cmd_nofix;
    char *cmd_fix;
    char *vldb_check;
    struct afstest_cmdinfo cmdinfo;

    vldb_check = afstest_obj_path("src/vlserver/vldb_check");

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
run_check(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct vlmod *mod = ops->rock;
    run_vldb_check(info->db_path, mod, 0);
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
    struct vlmod *mod;

    vltest_init(argv);

    plan(792);

    src_db = afstest_src_path("tests/vlserver/db.vlsmall/vldb4.DB0");

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

    {
	/* Run vldb_check against a freshly-created db, and check that it
	 * doesn't report any issues. */
	struct vlmod mod = {
	    .descr = "created db",
	    .check_code = 0,
	    .check_output = "",
	};
	struct ubiktest_ops ops = {
	    .descr = "created db",
	    .create_db = 1,
	    .post_stop = run_check,
	    .rock = &mod,
	};
	ubiktest_runtest(&vlsmall, &ops);
    }

    /* Run vldb_check with -fix, and then again without -fix. Then give the
     * result to the vlserver, and run the normal dataset tests. */

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
		.use_db = "vldb4",
		.pre_start = corrupt_db,
		.rock = mod,
		.override_dbtests = &mod->dbtest_corrupt[0],
	    };

	    ubiktest_runtest(&vlsmall, &ops);
	}

	if (!mod->only_corrupt) {
	    struct ubiktest_ops ops = {
		.descr = descr_fix,
		.use_db = "vldb4",
		.pre_start = run_fix,
		.rock = mod,
		.extra_dbtests = &mod->dbtest_fix[0],
	    };

	    ubiktest_runtest(&vlsmall, &ops);
	}

	free(descr_corrupt);
	free(descr_fix);
    }

    return 0;
}

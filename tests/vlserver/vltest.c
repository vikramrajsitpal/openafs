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

static char *vos;
static struct ubik_client *uclient;

void
vltest_init(char **argv)
{
    ubiktest_init("afs3-vlserver", argv);
    vos = afstest_obj_path("src/volser/vos");
}

static void
vltest_dbtest(char *dirname, struct ubiktest_dbtest *test)
{
    char *cmd = NULL;
    const char *auth;
    struct afstest_cmdinfo cmdinfo;

    auth = "-noauth";
    if (test->cmd_auth) {
	auth = "-localauth";
    }

    opr_Assert(vos != NULL);
    cmd = afstest_asprintf("%s %s -noresolve -config %s %s",
			   vos, test->cmd_args, dirname, auth);

    memset(&cmdinfo, 0, sizeof(cmdinfo));
    cmdinfo.command = cmd;
    cmdinfo.exit_code = test->cmd_exitcode;

    if (test->cmd_stdout != 0) {
	cmdinfo.output = test->cmd_stdout;
	cmdinfo.fd = STDOUT_FILENO;

    } else {
	opr_Assert(test->cmd_stderr != NULL);
	cmdinfo.output = test->cmd_stderr;
	cmdinfo.fd = STDERR_FILENO;
    }

    is_command(&cmdinfo, "%s", test->descr);

    free(cmd);
}

static void
do_addsite_rootafs(char *dirname)
{
    struct nvldbentry entry;
    afs_uint32 rwid;
    int code;

    memset(&entry, 0, sizeof(entry));

    /* Emulate 'vos addsite', but skip checking the volserver partitions. Also
     * don't bother with locking. */

    code = ubik_VL_GetEntryByNameN(uclient, 0, "root.afs", &entry);
    if (code != 0) {
	diag("%d: %s", code, afs_error_message(code));
	is_int(0, code, "addsite root.afs: VL_GetEntryByNameN(root.afs)");
	return;
    }

    opr_Assert(entry.nServers < NMAXNSERVERS);

    /* 10.0.0.2 */
    entry.serverNumber[entry.nServers] = 0x0A000002;
    entry.serverPartition[entry.nServers] = 0;
    entry.serverFlags[entry.nServers] = VLSF_ROVOL | VLSF_DONTUSE;

    entry.nServers++;

    rwid = entry.volumeId[RWVOL];
    code = ubik_VL_ReplaceEntryN(uclient, 0, rwid, RWVOL, &entry, 0);
    if (code != 0) {
	diag("%d: %s", code, afs_error_message(code));
	is_int(0, code, "addsite root.afs: VL_ReplaceEntryN(root.afs)");
	return;
    }

    ok(1, "addsite root.afs");
}

static void
create_newvol(char *dirname)
{
    struct vltest_voldef vol = {
	.name = "vol.newvol",
	.rwid = 536870921,
	.server = 0x0A000002,
    };
    vltest_createvol(&vol);
}

/*
 * Our info to check the "vlsmall" reference set of data. Just a pretty normal,
 * if small, vldb.
 */
static struct ubiktest_dbtest vlsmall_tests[] = {
    {
	.descr = "list all server entries",
	.cmd_args = "listaddrs -printuuid",
	.cmd_stdout =
	    "UUID: 5dafb2eb-77d0-4b42-ac-cb-e54c3b6db53a\n"
	    "10.0.0.1\n"
	    "\n"
	    "UUID: ce387b42-9e84-459f-83-81-230712ed4cb5\n"
	    "10.0.0.2\n"
	    "\n"
    },
    {
	.descr = "list all volume entries",
	.cmd_args = "listvldb",
	.cmd_stdout =
	    "VLDB entries for all servers \n"
	    "\n"
	    "root.afs \n"
	    "    RWrite: 536870912     ROnly: 536870913 \n"
	    "    number of sites -> 2\n"
	    "       server 10.0.0.1 partition /vicepa RW Site \n"
	    "       server 10.0.0.1 partition /vicepa RO Site \n"
	    "\n"
	    "root.cell \n"
	    "    RWrite: 536870915     ROnly: 536870916 \n"
	    "    number of sites -> 2\n"
	    "       server 10.0.0.1 partition /vicepa RW Site \n"
	    "       server 10.0.0.1 partition /vicepa RO Site \n"
	    "\n"
	    "vol.691719c1 \n"
	    "    RWrite: 536870918 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.2 partition /vicepa RW Site \n"
	    "\n"
	    "vol.bigid \n"
	    "    RWrite: 536879106 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.2 partition /vicepa RW Site \n"
	    "\n"
	    "Total entries: 4\n"
    },

    /*
     * vol.691719c1's name was chosen because the vldb4 name hash value of it
     * should be the same as "root.cell" (7485). Make sure that looking it up
     * by name results in the correct entry.
     */
    {
	.descr = "lookup root.cell by name",
	.cmd_args = "listvldb -name root.cell",
	.cmd_stdout =
	    "\n"
	    "root.cell \n"
	    "    RWrite: 536870915     ROnly: 536870916 \n"
	    "    number of sites -> 2\n"
	    "       server 10.0.0.1 partition /vicepa RW Site \n"
	    "       server 10.0.0.1 partition /vicepa RO Site \n"
    },
    {
	.descr = "lookup vol.691719c1 by name",
	.cmd_args = "listvldb -name vol.691719c1",
	.cmd_stdout =
	    "\n"
	    "vol.691719c1 \n"
	    "    RWrite: 536870918 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.2 partition /vicepa RW Site \n"
    },

    /*
     * vol.bigid has an RW volid that has the same hash value as root.cell's
     * (11). Ensure that looking them up by id still results in the correct
     * entry.
     */
    {
	.descr = "lookup root.cell by RW id",
	.cmd_args = "listvldb -name 536870915",
	.cmd_stdout =
	    "\n"
	    "root.cell \n"
	    "    RWrite: 536870915     ROnly: 536870916 \n"
	    "    number of sites -> 2\n"
	    "       server 10.0.0.1 partition /vicepa RW Site \n"
	    "       server 10.0.0.1 partition /vicepa RO Site \n"
    },
    {
	.descr = "lookup root.cell by RO id",
	.cmd_args = "listvldb -name 536870916",
	.cmd_stdout =
	    "\n"
	    "root.cell \n"
	    "    RWrite: 536870915     ROnly: 536870916 \n"
	    "    number of sites -> 2\n"
	    "       server 10.0.0.1 partition /vicepa RW Site \n"
	    "       server 10.0.0.1 partition /vicepa RO Site \n"
    },
    {
	.descr = "lookup vol.bigid by RW id",
	.cmd_args = "listvldb -name 536879106",
	.cmd_stdout =
	    "\n"
	    "vol.bigid \n"
	    "    RWrite: 536879106 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.2 partition /vicepa RW Site \n"
    },

    {
	.descr = "create vol.newvol",
	.func = create_newvol,
    },
    {
	.descr = "check vol.newvol",
	.cmd_args = "listvldb -name vol.newvol",
	.cmd_stdout =
	    "\n"
	    "vol.newvol \n"
	    "    RWrite: 536870921 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.2 partition /vicepa RW Site \n"
    },

    {
	/*
	 * Effectively 'vos addsite -server 10.0.0.2 -partition vicepa -id root.afs',
	 * but skips talking to the volserver to check the given partition.
	 */
	.descr = "add RO site to root.afs",
	.func = do_addsite_rootafs,
    },
    {
	.descr = "check modified root.afs",
	.cmd_args = "listvldb -name root.afs",
	.cmd_stdout =
	    "\n"
	    "root.afs \n"
	    "    RWrite: 536870912     ROnly: 536870913 \n"
	    "    number of sites -> 3\n"
	    "       server 10.0.0.1 partition /vicepa RW Site \n"
	    "       server 10.0.0.1 partition /vicepa RO Site \n"
	    "       server 10.0.0.2 partition /vicepa RO Site  -- Not released\n"
    },

    {
	.descr = "remove RO site from root.cell",
	.cmd_args = "remsite -server 10.0.0.1 -partition vicepa -id root.cell",
	.cmd_auth = 1,
	.cmd_stdout = "Deleting the replication site for volume 536870915 ... done\n"
		      "Removed replication site 10.0.0.1 /vicepa for volume root.cell\n",
    },
    {
	.descr = "check modified root.cell",
	.cmd_args = "listvldb -name root.cell",
	.cmd_stdout =
	    "\n"
	    "root.cell \n"
	    "    RWrite: 536870915 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.1 partition /vicepa RW Site \n"
    },

    {
	.descr = "remove vol.bigid",
	.cmd_args = "delentry -id vol.bigid",
	.cmd_auth = 1,
	.cmd_stdout = "Deleted 1 VLDB entries\n",
    },

    {
	.descr = "vol.bigid no longer exists",
	.cmd_args = "listvldb -name vol.bigid",
	.cmd_exitcode = 1,
	.cmd_stderr = "VLDB: no such entry\n",
    },

    {
	.descr = "change fileserver addrs",
	.cmd_args = "setaddrs -uuid 5dafb2eb-77d0-4b42-ac-cb-e54c3b6db53a "
		    "-host 10.0.0.3",
	.cmd_auth = 1,
	.cmd_stdout = "",
    },
    {
	.descr = "list all server entries (again)",
	.cmd_args = "listaddrs -printuuid",
	.cmd_stdout =
	    "UUID: 5dafb2eb-77d0-4b42-ac-cb-e54c3b6db53a\n"
	    "10.0.0.3\n"
	    "\n"
	    "UUID: ce387b42-9e84-459f-83-81-230712ed4cb5\n"
	    "10.0.0.2\n"
	    "\n"
    },

    {
	.descr = "list all volume entries (again)",
	.cmd_args = "listvldb",
	.cmd_stdout =
	    "VLDB entries for all servers \n"
	    "\n"
	    "root.afs \n"
	    "    RWrite: 536870912     ROnly: 536870913 \n"
	    "    number of sites -> 3\n"
	    "       server 10.0.0.3 partition /vicepa RW Site \n"
	    "       server 10.0.0.3 partition /vicepa RO Site \n"
	    "       server 10.0.0.2 partition /vicepa RO Site  -- Not released\n"
	    "\n"
	    "root.cell \n"
	    "    RWrite: 536870915 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.3 partition /vicepa RW Site \n"
	    "\n"
	    "vol.691719c1 \n"
	    "    RWrite: 536870918 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.2 partition /vicepa RW Site \n"
	    "\n"
	    "vol.newvol \n"
	    "    RWrite: 536870921 \n"
	    "    number of sites -> 1\n"
	    "       server 10.0.0.2 partition /vicepa RW Site \n"
	    "\n"
	    "Total entries: 4\n"
    },

    {0}
};

static void
vlsmall_create(char *dirname)
{
    struct ubiktest_dbtest *cmd;

    static struct ubiktest_dbtest fs_cmds[] = {
	{
	    .descr = "Create 10.0.0.1 server",
	    .cmd_args = "setaddrs -uuid 5dafb2eb-77d0-4b42-ac-cb-e54c3b6db53a "
			"-host 10.0.0.1",
	    .cmd_auth = 1,
	    .cmd_stdout = "",
	},
	{
	    .descr = "Create 10.0.0.2 server",
	    .cmd_args = "setaddrs -uuid ce387b42-9e84-459f-83-81-230712ed4cb5 "
			"-host 10.0.0.2",
	    .cmd_auth = 1,
	    .cmd_stdout = "",
	},
	{0}
    };

    static struct vltest_voldef vol_defs[] = {
	{
	    .name = "root.afs",
	    .rwid = 536870912,
	    .roid = 536870913,
	    .server = 0x0A000001,
	},
	{
	    .name = "root.cell",
	    .rwid = 536870915,
	    .roid = 536870916,
	    .server = 0x0A000001,
	},
	{
	    .name = "vol.691719c1",
	    .rwid = 536870918,
	    .server = 0x0A000002,
	},
	{
	    .name = "vol.bigid",
	    .rwid = 536879106,
	    .server = 0x0A000002,
	},
	{0}
    };

    for (cmd = fs_cmds; cmd->descr != NULL; cmd++) {
	vltest_dbtest(dirname, cmd);
    }

    vltest_createvol_list(vol_defs);
}

struct ubiktest_dataset vlsmall = {
    .descr = "vlsmall",
    .server_type = &afstest_server_vl,
    .uclientp = &uclient,

    .dbtest_func = vltest_dbtest,
    .tests = vlsmall_tests,
    .create_func = vlsmall_create,
    .existing_dbs = {
	{
	    .name = "vldb4",
	    .flat_path = "tests/vlserver/db.vlsmall/vldb4.DB0",
	},
    },
};

void
vltest_createvol(struct vltest_voldef *vol)
{
    int code;
    struct nvldbentry entry;

    memset(&entry, 0, sizeof(entry));

    opr_Assert(strlen(vol->name) < sizeof(entry.name) - 1);
    strcpy(entry.name, vol->name);
    entry.flags = VLF_RWEXISTS;
    entry.volumeId[RWVOL] = vol->rwid;

    entry.serverNumber[entry.nServers] = vol->server;
    entry.serverPartition[entry.nServers] = 0;
    entry.serverFlags[entry.nServers] = VLSF_RWVOL;
    entry.nServers++;

    if (vol->roid != 0) {
	entry.volumeId[ROVOL] = vol->roid;
	entry.flags |= VLF_ROEXISTS;

	entry.serverNumber[entry.nServers] = vol->server;
	entry.serverPartition[entry.nServers] = 0;
	entry.serverFlags[entry.nServers] = VLSF_ROVOL;
	entry.nServers++;
    }

    code = ubik_VL_CreateEntryN(uclient, 0, &entry);
    is_int(0, code, "VL_CreateEntryN for %s returns success", vol->name);
}

void
vltest_createvol_list(struct vltest_voldef *voldef)
{
    for (; voldef->name != NULL; voldef++) {
	vltest_createvol(voldef);
    }
}

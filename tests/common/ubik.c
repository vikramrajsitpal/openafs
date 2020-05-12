/*
 * Copyright (c) 2012 Your File System Inc. All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>
#include <sys/mman.h>

#include <afs/cellconfig.h>
#include <afs/okv.h>
#include <ubik_internal.h>

#include "common.h"

struct ubiktest_serverinfo {
    char *dirname;
    struct afsconf_dir *dir;

    char *db_path;
    char *ctl_sock;

    pid_t pid;
    int stop_code;

    afs_uint32 addr;
    char host_str[16];
};

int
afstest_GetUbikClient(struct afsconf_dir *dir, char *service,
		      int serviceId,
		      struct rx_securityClass *secClass, int secIndex,
		      struct ubik_client **ubikClient)
{
    int code, i;
    struct afsconf_cell info;
    struct rx_connection *serverconns[MAXSERVERS];

    code = afsconf_GetCellInfo(dir, NULL, service, &info);
    if (code)
	return code;

    for (i = 0; i < info.numServers; i++) {
	serverconns[i] = rx_NewConnection(info.hostAddr[i].sin_addr.s_addr,
					  info.hostAddr[i].sin_port,
					  serviceId,
					  secClass, secIndex);
    }

    serverconns[i] = NULL;

    *ubikClient = NULL;

    return ubik_ClientInit(serverconns, ubikClient);

}

void
ubiktest_init(char *service, char **argv)
{
    int code;

    /* Skip all tests if the current hostname can't be resolved */
    afstest_SkipTestsIfBadHostname();
    /* Skip all tests if the current hostname is on the loopback network */
    afstest_SkipTestsIfLoopbackNetIsDefault();
    /* Skip all tests if a server is already running on this system. */
    afstest_SkipTestsIfServerRunning(service);

    setprogname(argv[0]);

    code = rx_Init(0);
    if (code != 0) {
	bail("rx_Init failed with %d", code);
    }
}

/*
 * Is the given path a valid .DB0 file? Return 1 if so, 0 if not.
 */
static int
valid_flatfile_db(char *path)
{
    FILE *fh = fopen(path, "r");
    struct ubik_hdr hdr;
    int isvalid = 0;

    memset(&hdr, 0, sizeof(hdr));

    if (fh == NULL) {
	diag("cannot open %s: errno %d", path, errno);
	goto bad;
    }

    if (fread(&hdr, sizeof(hdr), 1, fh) != 1) {
	diag("cannot read header from %s", path);
	goto bad;
    }

    hdr.version.epoch = ntohl(hdr.version.epoch);
    hdr.version.counter = ntohl(hdr.version.counter);
    hdr.magic = ntohl(hdr.magic);
    hdr.size = ntohs(hdr.size);

    if (hdr.magic != UBIK_MAGIC) {
	diag("db %s, bad magic 0x%x != 0x%x", path, hdr.magic, UBIK_MAGIC);
	goto bad;
    }

    if (hdr.size != HDRSIZE) {
	diag("db %s, bad hdrsize %d != %d", path, hdr.size, HDRSIZE);
	goto bad;
    }

    if (hdr.version.epoch == 0) {
	diag("db %s, bad version %d.%d", path, hdr.version.epoch,
	     hdr.version.counter);
	goto bad;
    }

    isvalid = 1;

 bad:
    if (fh != NULL) {
	fclose(fh);
    }
    return isvalid;
}

static void
run_testlist(struct ubiktest_dataset *ds, struct ubiktest_dbtest *testlist,
	     struct ubiktest_serverinfo *server_info, int n_servers)
{
    struct ubiktest_dbtest *test;
    for (test = testlist; test->descr != NULL; test++) {
	int server_i;
	int max_server;
	char *dirname = server_info[0].dirname;

	if (test->func != NULL) {
	    (*test->func)(dirname);
	    continue;
	}

	opr_Assert(ds->dbtest_func != NULL);
	if (n_servers == 1) {
	    /* Only 1 site; don't specify a specific server. */
	    (*ds->dbtest_func)(dirname, NULL, test);
	    continue;
	}

	max_server = n_servers;
	if (test->cmd_sync) {
	    /* Only run against the sync site. */
	    max_server = 1;
	}
	for (server_i = 0; server_i < max_server; server_i++) {
	    (*ds->dbtest_func)(dirname, server_info[server_i].host_str,
			       test);
	}
    }
}

/* Return the path to the database specified in 'dbdef'. Caller must free the
 * returned string. */
static char *
get_dbpath_optional(struct ubiktest_dbdef *dbdef, int *a_present,
		    char **a_skipreason)
{
    const char *progname = getprogname();
    int code;
    char *path;

    *a_present = 1;

    if (dbdef->flat_path != NULL) {
	path = afstest_src_path(dbdef->flat_path);
	opr_Assert(path != NULL);

    } else if (dbdef->kv_path != NULL) {
	if (*a_skipreason == NULL) {
	    code = afstest_okv_dbpath(&path, dbdef->kv_path);
	    if (code != 0) {
		afs_com_err(progname, code,
			    "while opening okv dbase %s", dbdef->kv_path);
		return NULL;
	    }
	    if (path == NULL) {
		*a_skipreason = afstest_asprintf("okv dbase %s does not exist "
						 "on this platform",
						 dbdef->kv_path);
	    }
	}

    } else {
	path = NULL;
	*a_present = 0;
    }
    return path;
}

static char *
get_dbpath(struct ubiktest_dbdef *dbdef, char **a_skipreason)
{
    int present;
    return get_dbpath_optional(dbdef, &present, a_skipreason);
}

/*
 * Find the dbase in 'ds' with the name 'use_db', and return the dbdef for it.
 * A return of NULL means "none" was specified, meaning don't use any existing
 * database.
 */
static struct ubiktest_dbdef *
find_dbdef(struct ubiktest_dataset *ds, char *use_db)
{
    struct ubiktest_dbdef *dbdef;

    if (strcmp(use_db, "none") == 0) {
	return NULL;
    }

    for (dbdef = ds->existing_dbs; dbdef->name != NULL; dbdef++) {
	if (strcmp(use_db, dbdef->name) == 0) {
	    break;
	}
    }
    if (dbdef->name == NULL) {
	bail("invalid db name %s", use_db);
    }

    return dbdef;
}

static void
init_serverinfo(struct ubiktest_serverinfo **a_serverinfo, int n_servers,
		struct ubiktest_dataset *ds, char *src_db)
{
    struct afstest_server_type *server = ds->server_type;
    struct ubiktest_serverinfo *server_info;
    struct sockaddr_storage *ips;
    int server_i;

    server_info = bcalloc(n_servers, sizeof(server_info[0]));
    ips = bcalloc(n_servers, sizeof(ips[0]));

    for (server_i = 0; server_i < n_servers; server_i++) {
	struct sockaddr_in *sin = (struct sockaddr_in *)&ips[server_i];
	struct ubiktest_serverinfo *sinfo = &server_info[server_i];

	sin->sin_family = AF_INET;

	if (n_servers == 1) {
	    /* If we only have 1 server, use the real machine's IP. */
	    sinfo->addr = afstest_MyHostAddr();
	} else {
	    /* Otherwise, use some loopback IPs. 127.1.0.10, 127.1.0.11, etc. */
	    sinfo->addr = htonl(0x7f01000A + server_i);
	}
	sin->sin_addr.s_addr = sinfo->addr;
    }

    for (server_i = 0; server_i < n_servers; server_i++) {
	struct afstest_configinfo config;
	struct sockaddr *sa = (struct sockaddr *)&ips[server_i];
	socklen_t salen = sizeof(ips[server_i]);
	struct ubiktest_serverinfo *sinfo = &server_info[server_i];
	int code;

	code = getnameinfo(sa, salen, sinfo->host_str, sizeof(sinfo->host_str),
			   NULL, 0, NI_NUMERICHOST);
	if (code != 0) {
	    bail("getnameinfo returned %d", code);
	}

	memset(&config, 0, sizeof(config));

	config.dbserver_addrs = ips;
	config.dbserver_addrs_len = n_servers;
	if (n_servers > 1) {
	    config.force_host = sinfo->host_str;
	}

	sinfo->dirname = afstest_BuildTestConfig(&config);
	diag("  dir[%d]: %s", server_i, sinfo->dirname);

	sinfo->dir = afsconf_Open(sinfo->dirname);
	opr_Assert(sinfo->dir != NULL);
	sinfo->db_path = afstest_asprintf("%s/%s.DB0", sinfo->dirname,
					  server->db_name);

	if (server->ctl_sock != NULL) {
	    sinfo->ctl_sock = afstest_asprintf("%s/%s", sinfo->dirname,
					       server->ctl_sock);
	    opr_Assert(sinfo->ctl_sock != NULL);
	}

	if (src_db != NULL) {
	    /* Copy the sample db into place (if any). */
	    code = afstest_cp(src_db, sinfo->db_path);
	    if (code != 0) {
		bail("error %d while copying %s -> %s", code, src_db,
		     sinfo->db_path);
	    }
	}
    }

    *a_serverinfo = server_info;
    server_info = NULL;

    free(server_info);
    free(ips);
}

static int
calc_ntests(struct ubiktest_dataset *ds, struct ubiktest_ops *ops,
	    int n_servers)
{
    int ntests = 0;
    int dbtests_nonsync = ds->n_dbtests - ds->n_dbtests_sync;
    opr_Assert(dbtests_nonsync >= 0);

    /* For all non-syncsite-only tests, we run the test against each server. */
    ntests += dbtests_nonsync * n_servers;

    /* For syncsite-only tests, of course we just run them once. */
    ntests += ds->n_dbtests_sync;

    /* The scenario ops may also have its own tests, which are run just once. */
    ntests += ops->n_tests;

    return ntests;
}

/**
 * Run db tests for the given dataset, for the given scenario ops.
 *
 * @param[in] db    The database dataset to run against (e.g. vlsmall).
 * @param[in] ops   The scenario to run.
 */
void
ubiktest_runtest(struct ubiktest_dataset *ds, struct ubiktest_ops *ops)
{
    const char *progname = getprogname();
    struct afstest_server_type *server = ds->server_type;
    int code;
    struct rx_securityClass *secClass = NULL;
    int secIndex = 0;
    char *src_db = NULL;
    char *db_copy = NULL;
    char *skip_reason = NULL;
    char *use_db = ops->use_db;
    struct ubiktest_dbdef *dbdef;
    struct ubiktest_dbtest *testlist;
    struct stat st;
    struct ubiktest_cbinfo cbinfo;
    struct ubiktest_serverinfo *server_info = NULL;
    struct ubiktest_serverinfo *syncsite = NULL;
    int server_i;
    int n_servers;
    struct afstest_server_opts *srv_opts = NULL;
    unsigned long testnum_start = testnum;
    int n_tests;

    memset(&cbinfo, 0, sizeof(cbinfo));

    opr_Assert(progname != NULL);

    n_servers = ops->n_servers;
    if (n_servers == 0) {
	/* Default to 1 server process. */
	n_servers = 1;
    }

    n_tests = 4 + calc_ntests(ds, ops, n_servers);

    diag("ubiktest dataset: %s, ops: %s, tests: %d", ds->descr, ops->descr,
	 n_tests);

    if (ops->skip_reason != NULL) {
	skip_block(n_tests, "%s", ops->skip_reason);
	code = 0;
	goto done;
    }

    /* Get the path to the sample db we're using. */

    if (use_db == NULL && ops->create_db) {
	use_db = "none";
    }

    opr_Assert(use_db != NULL);
    dbdef = find_dbdef(ds, use_db);
    if (dbdef != NULL) {
	src_db = get_dbpath(dbdef, &skip_reason);
    }
    cbinfo.src_dbdef = dbdef;

    if (skip_reason != NULL) {
	skip_block(n_tests, "%s", skip_reason);
	code = 0;
	goto done;
    }

    /* Setup config and other data for server procs. */

    init_serverinfo(&server_info, n_servers, ds, src_db);
    syncsite = &server_info[0];

    srv_opts = bcalloc(n_servers, sizeof(srv_opts[0]));

    cbinfo.confdir = syncsite->dirname;
    cbinfo.db_path = syncsite->db_path;
    cbinfo.ctl_sock = syncsite->ctl_sock;

    if (ops->pre_start != NULL) {
	(*ops->pre_start)(&cbinfo, ops);
    }

    for (server_i = 0; server_i < n_servers; server_i++) {
	struct ubiktest_serverinfo *sinfo = &server_info[server_i];
	struct afstest_server_opts *opts = &srv_opts[server_i];

	opts->server = server;
	opts->addr = sinfo->addr;
	opts->dirname = sinfo->dirname;
	opts->serverPid = &sinfo->pid;
	opts->extra_argv = ops->server_argv;
	if (n_servers > 1) {
	    opts->rxbind = 1;
	    opts->multi_server = 1;
	    opts->nowait = 1;
	}

	code = afstest_StartServerOpts(opts);
	if (code != 0) {
	    afs_com_err(progname, code, "while starting server");
	    goto error;
	}
    }

    if (n_servers > 1) {
	/* Wait for sync-site server pid to finish starting up. */
	struct afstest_server_opts *opts = &srv_opts[0];
	opts->nowait = 0;
	code = afstest_StartServerOpts(opts);
	if (code != 0) {
	    afs_com_err(progname, code, "while waiting for server startup");
	    goto error;
	}
    }

    code = afsconf_ClientAuthSecure(syncsite->dir, &secClass, &secIndex);
    if (code != 0) {
	afs_com_err(progname, code, "while building security class");
	goto error;
    }

    cbinfo.disk_conn = rx_NewConnection(syncsite->addr,
					htons(ds->server_type->port),
					DISK_SERVICE_ID, secClass, secIndex);
    opr_Assert(cbinfo.disk_conn != NULL);

    if (ds->uclientp != NULL) {
	code = afstest_GetUbikClient(syncsite->dir, server->service_name,
				     USER_SERVICE_ID, secClass, secIndex,
				     ds->uclientp);
	if (code != 0) {
	    afs_com_err(progname, code, "while building ubik client");
	    goto error;
	}
    }

    if (ops->create_db) {
	if (ds->create_func == NULL) {
	    bail("create_db set, but we have no create_func");
	}

	(*ds->create_func)(syncsite->dirname);

	db_copy = afstest_asprintf("%s.copy", syncsite->db_path);

	code = afstest_cp(syncsite->db_path, db_copy);
	if (code != 0) {
	    afs_com_err(progname, code, "while copying %s -> %s",
			syncsite->db_path, db_copy);
	    goto error;
	}

	diag("Copy of created db saved in %s", db_copy);
    }

    if (ops->post_start != NULL) {
	(*ops->post_start)(&cbinfo, ops);
    }

    if (ops->extra_dbtests != NULL) {
	run_testlist(ds, ops->extra_dbtests, server_info, n_servers);
    }
    testlist = ds->tests;
    if (ops->override_dbtests != NULL && ops->override_dbtests[0].descr != NULL) {
	testlist = ops->override_dbtests;
    }
    run_testlist(ds, testlist, server_info, n_servers);

    for (server_i = 0; server_i < n_servers; server_i++) {
	struct ubiktest_serverinfo *sinfo = &server_info[server_i];
	sinfo->stop_code = afstest_StopServer(sinfo->pid);
	sinfo->pid = 0;
	if (sinfo->stop_code != 0) {
	    diag("server %d exit code %d", server_i, sinfo->stop_code);
	}
    }

    code = 0;
    for (server_i = 0; server_i < n_servers; server_i++) {
	struct ubiktest_serverinfo *sinfo = &server_info[server_i];
	if (sinfo->stop_code != 0) {
	    code = sinfo->stop_code;
	    break;
	}
    }

    is_int(0, code, "servers exited cleanly");
    if (code != 0) {
	afs_com_err(progname, code, "while stopping servers");
	goto error;
    }

    /* After everything is done, make sure the db is the proper type
     * (flatfile vs KV). */

    code = lstat(syncsite->db_path, &st);
    if (code != 0) {
	sysdiag("lstat(%s)", syncsite->db_path);
    }
    is_int(0, code, ".DB0 file exists");
    if (code != 0) {
	goto error;
    }

    if (ops->result_kv) {
	struct okv_dbhandle *dbh = NULL;

	ok(S_ISLNK(st.st_mode),
	   "db %s is a symlink (mode 0x%x)", syncsite->db_path,
	   (unsigned)st.st_mode);

	code = okv_open(syncsite->db_path, &dbh);
	is_int(0, code, "db %s is a valid okv dbase", syncsite->db_path);
	okv_close(&dbh);

    } else {
	ok(S_ISREG(st.st_mode),
	   "db %s is a regular file (mode 0x%x)", syncsite->db_path,
	   (unsigned)st.st_mode);

	ok(valid_flatfile_db(syncsite->db_path),
	   "db %s is a valid flatfile db", syncsite->db_path);
    }

    if (ops->post_stop != NULL) {
	(*ops->post_stop)(&cbinfo, ops);
    }

    code = 0;

 done:
    if (ds->uclientp != NULL && *ds->uclientp != NULL) {
	ubik_ClientDestroy(*ds->uclientp);
	*ds->uclientp = NULL;
    }
    if (cbinfo.disk_conn != NULL) {
	rx_DestroyConnection(cbinfo.disk_conn);
	cbinfo.disk_conn = NULL;
    }

    if (server_info != NULL) {
	for (server_i = 0; server_i < n_servers; server_i++) {
	    struct ubiktest_serverinfo *sinfo = &server_info[server_i];
	    if (sinfo->pid != 0) {
		int stop_code = afstest_StopServer(sinfo->pid);
		if (stop_code != 0) {
		    afs_com_err(progname, stop_code, "while stopping server");
		    if (code == 0) {
			code = 1;
		    }
		}
		sinfo->pid = 0;
	    }

	    if (sinfo->dir != NULL) {
		afsconf_Close(sinfo->dir);
		sinfo->dir = NULL;
	    }

	    if (sinfo->dirname != NULL) {
		afstest_rmdtemp(sinfo->dirname);
		free(sinfo->dirname);
	    }
	    free(sinfo->db_path);
	}
    }

    if (code != 0) {
	bail("encountered unexpected error");
    }
    if (ops->n_tests != 0 && testnum - testnum_start != n_tests) {
	/* If ops->n_tests is set, make sure it's set correctly, in case we
	 * ever need to skip the tests inside. */
	bail("bad n_tests: %lu - %lu == %ld != %d",
	     testnum, testnum_start, testnum - testnum_start, n_tests);
    }

    free(src_db);
    free(db_copy);
    free(skip_reason);
    if (secClass != NULL) {
	rxs_Release(secClass);
    }

    return;

 error:
    code = 1;
    goto done;
}

/**
 * Run db tests for the given dataset, for each scenario in the given ops list.
 *
 * @param[in] db    The database dataset to run against (e.g. vlsmall).
 * @param[in] ops   An array of ops, specifying scenarios to run. The array is
 *		    terminated by an ops struct filled with zeroes.
 */
void
ubiktest_runtest_list(struct ubiktest_dataset *ds, struct ubiktest_ops *ops)
{
    for (; ops->descr != NULL; ops++) {
	ubiktest_runtest(ds, ops);
    }
}

static void
rx_recv_file(struct rx_call *rxcall, char *path)
{
    static char buf[1024];
    FILE *fh;
    int nbytes;

    fh = fopen(path, "wx");
    if (fh == NULL) {
	sysbail("fopen(%s)", path);
    }

    for (;;) {
	nbytes = rx_Read(rxcall, buf, sizeof(buf));
	if (nbytes == 0) {
	    break;
	}

	opr_Verify(fwrite(buf, nbytes, 1, fh) == 1);
    }

    opr_Verify(fclose(fh) == 0);
}

static int
rx_send_file(struct rx_call *rxcall, char *path, off_t start_off, off_t length)
{
    char *buf;
    int code = 0;
    int fd;
    int nbytes;
    size_t map_len = start_off + length;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
	sysbail("open(%s)", path);
    }

    buf = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == NULL) {
	sysbail("mmap");
    }

    nbytes = rx_Write(rxcall, &buf[start_off], length);
    if (nbytes != length) {
	diag("rx_send_file: wrote %d/%d bytes (call error %d)", nbytes,
	     (int)length, rx_Error(rxcall));
	code = RX_PROTOCOL_ERROR;
    }

    opr_Verify(munmap(buf, map_len) == 0);
    close(fd);

    return code;
}

static void
run_getfile(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct rx_call *rxcall = rx_NewCall(info->disk_conn);
    char *tmp_path = NULL;
    char *expected_path = NULL;
    int code;

    tmp_path = afstest_asprintf("%s/getfile.dmp", info->confdir);
    expected_path = afstest_src_path(info->src_dbdef->getfile_path);

    code = StartDISK_GetFile(rxcall, 0);
    opr_Assert(code == 0);

    rx_recv_file(rxcall, tmp_path);

    code = rx_EndCall(rxcall, 0);
    is_int(0, code, "DISK_GetFile call succeeded");

    ok(afstest_file_equal(expected_path, tmp_path, 0),
       "DISK_GetFile returns expected contents");

    free(tmp_path);
    free(expected_path);
}

static void
run_sendfile(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    char *db_path = ops->rock;
    struct ubik_stat ustat;
    struct ubik_version version;
    int code;
    struct rx_call *rxcall = rx_NewCall(info->disk_conn);

    memset(&version, 0, sizeof(version));
    memset(&ustat, 0, sizeof(ustat));

    code = uphys_stat_path(db_path, &ustat);
    opr_Assert(code == 0);
    opr_Assert(ustat.size > 0);

    code = uphys_getlabel_path(db_path, &version);
    opr_Assert(code == 0);

    code = StartDISK_SendFile(rxcall, 0, ustat.size, &version);
    opr_Assert(code == 0);

    code = rx_send_file(rxcall, db_path, HDRSIZE, ustat.size);

    code = rx_EndCall(rxcall, code);
    is_int(0, code, "DISK_SendFile call succeeded");
}

void
urectest_runtests(struct ubiktest_dataset *ds, char *use_db)
{
    struct ubiktest_ops utest;
    struct ubiktest_dbdef *dbdef;
    char *db_path = NULL;
    char *skip_reason = NULL;

    dbdef = find_dbdef(ds, use_db);
    opr_Assert(dbdef != NULL);
    db_path = get_dbpath(dbdef, &skip_reason);

    memset(&utest, 0, sizeof(utest));

    {
	utest.rock = db_path;
	utest.descr = afstest_asprintf("run DISK_GetFile for %s", use_db);
	utest.skip_reason = skip_reason;
	utest.use_db = use_db;
	utest.post_start = run_getfile;
	utest.n_tests = 2;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
    {
	utest.rock = db_path;
	utest.descr = afstest_asprintf("run DISK_SendFile for %s", use_db);
	utest.skip_reason = skip_reason;
	utest.use_db = "none";
	utest.post_start = run_sendfile;
	utest.n_tests = 1;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
}

struct freeze_test {
    struct frztest_ops *ops;
    int backup_db;	/**< frztest_restore: if set, test backing up the
			 *   original db when restoring. */

    int revert_exit;	/**< frztest_revert: get the freeze to fail by exiting
			 *   with an error. */
    int revert_abort;	/**< frztest_revert: get the freeze to fail by running
			 *   vldb-freeze-abort. */
    int revert_abort_force;
			/**< frztest_revert: get the freeze to fail by running
			 *   vldb-freeze-abort -force. */
    int revert_abort_id;/**< frztest_revert: get the freeze to fail by running
			 *   vldb-freeze-abort -freezeid. */
    int revert_timeout;	/**< frztest_revert: get the freeze to fail by timeout. */

    char *db_path;	/**< path to the ops->use_db dbase */
    char *blankdb_path;	/**< path to the ops->blankdb dbase */
    char *baddb_path;	/**< path to the ops->baddb dbase */
};

static char *ctl_path;
static char *ctl_sockname;

/* Generate a openafs-ctl command string to run */
static char *
ctl_cmd_v(char *confdir, char *suite, char *subcmd, char *fmt, va_list ap)
{
    char *args = NULL;
    char *cmdline;

    opr_Assert(ctl_path != NULL);
    opr_Assert(ctl_sockname != NULL);

    args = afstest_vasprintf(fmt, ap);
    cmdline = afstest_asprintf("%s %s%s -quiet -ctl-socket %s/%s %s",
			       ctl_path, suite, subcmd, confdir, ctl_sockname,
			       args);
    free(args);

    return cmdline;
}

static char *
AFS_ATTRIBUTE_FORMAT(__printf__, 4, 5)
ctl_cmd(char *confdir, char *suite, char *subcmd, char *fmt, ...)
{
    va_list ap;
    char *cmdline;
    va_start(ap, fmt);
    cmdline = ctl_cmd_v(confdir, suite, subcmd, fmt, ap);
    va_end(ap);
    return cmdline;
}

/* Run an openafs-ctl command */
static void
AFS_ATTRIBUTE_FORMAT(__printf__, 4, 5)
ctl_run(char *confdir, char *suite, char *subcmd, char *fmt, ...)
{
    va_list ap;
    char *cmdline;
    struct afstest_cmdinfo cmdinfo;

    memset(&cmdinfo, 0, sizeof(cmdinfo));

    va_start(ap, fmt);
    cmdline = ctl_cmd_v(confdir, suite, subcmd, fmt, ap);
    va_end(ap);

    cmdinfo.output = "";
    cmdinfo.fd = STDOUT_FILENO;
    cmdinfo.command = cmdline;

    is_command(&cmdinfo, "openafs-ctl %s%s %s runs successfully",
	       suite, subcmd, fmt);

    free(cmdline);
}

static int
opaque_cmp(struct rx_opaque *buf_a, struct rx_opaque *buf_b)
{
    if (buf_a->len < buf_b->len) {
	return -1;
    } else if (buf_a->len > buf_b->len) {
	return 1;
    }
    return memcmp(buf_a->val, buf_b->val, buf_a->len);
}

static int
db_equal_kv(char *path_a, char *path_b)
{
    int code;
    struct okv_dbhandle *dbh_a = NULL;
    struct okv_dbhandle *dbh_b = NULL;
    struct okv_trans *tx_a = NULL;
    struct okv_trans *tx_b = NULL;
    struct rx_opaque key_a;
    struct rx_opaque key_b;
    struct rx_opaque val_a;
    struct rx_opaque val_b;
    struct rx_opaque_stringbuf sbuf_a;
    struct rx_opaque_stringbuf sbuf_b;
    int equal = 0;

    memset(&key_a, 0, sizeof(key_a));
    memset(&key_b, 0, sizeof(key_b));
    memset(&val_a, 0, sizeof(val_a));
    memset(&val_b, 0, sizeof(val_b));
    memset(&sbuf_a, 0, sizeof(sbuf_a));
    memset(&sbuf_b, 0, sizeof(sbuf_b));

    code = ukv_open(path_a, &dbh_a, NULL);
    if (code != 0) {
	diag("failed to open %s, code %d", path_a, code);
	goto done;
    }

    code = ukv_open(path_b, &dbh_b, NULL);
    if (code != 0) {
	diag("failed to open %s, code %d", path_b, code);
	goto done;
    }

    code = okv_begin(dbh_a, OKV_BEGIN_RO, &tx_a);
    if (code != 0) {
	diag("okv_begin failed for %s, code %d", path_a, code);
	goto done;
    }

    code = okv_begin(dbh_b, OKV_BEGIN_RO, &tx_b);
    if (code != 0) {
	diag("okv_begin failed for %s, code %d", path_b, code);
	goto done;
    }

    for (;;) {
	int eof_a = 0;
	int eof_b = 0;

	/*
	 * Note that we use ukv_next() here, instead of okv_next(). This means
	 * we skip ubik-private keys, so if the given databases just differ in
	 * the ubik version, we'll still report that the databases are equal.
	 */

	code = ukv_next(tx_a, &key_a, &val_a, &eof_a);
	if (code != 0) {
	    diag("okv_next failed for %s, code %d", path_a, code);
	    goto done;
	}

	code = ukv_next(tx_b, &key_b, &val_b, &eof_b);
	if (code != 0) {
	    diag("okv_next failed for %s, code %d", path_b, code);
	    goto done;
	}

	if (eof_a && eof_b) {
	    break;
	}

	if (eof_a) {
	    diag("hit eof for %s, while %s has key %s", path_a, path_b,
		 rx_opaque_stringify(&key_b, &sbuf_b));
	    goto done;
	}
	if (eof_b) {
	    diag("hit eof for %s, while %s has key %s", path_b, path_a,
		 rx_opaque_stringify(&key_a, &sbuf_a));
	    goto done;
	}

	if (opaque_cmp(&key_a, &key_b) != 0) {
	    diag("%s key %s != %s key %s",
		 path_a, rx_opaque_stringify(&key_a, &sbuf_a),
		 path_b, rx_opaque_stringify(&key_b, &sbuf_b));
	    goto done;
	}

	if (opaque_cmp(&val_a, &val_b) != 0) {
	    diag("%s key %s val != %s key %s val",
		 path_a, rx_opaque_stringify(&key_a, &sbuf_a),
		 path_b, rx_opaque_stringify(&key_b, &sbuf_b));
	    goto done;
	}
    }

    equal = 1;

 done:
    okv_abort(&tx_a);
    okv_abort(&tx_b);
    okv_close(&dbh_a);
    okv_close(&dbh_b);
    return equal;
}

static int
db_equal(char *path_a, char *path_b)
{
    int isdir_a = 0;
    int isdir_b = 0;
    int exists_a = 0;
    int exists_b = 0;

    opr_Verify(udb_dbinfo(path_a, &exists_a, &isdir_a, NULL) == 0);
    opr_Verify(udb_dbinfo(path_b, &exists_b, &isdir_b, NULL) == 0);

    if (!exists_a) {
	diag("%s doesn't exist", path_a);
	return 0;
    }
    if (!exists_b) {
	diag("%s doesn't exist", path_b);
	return 0;
    }

    if (isdir_a != isdir_b) {
	diag("%d != %d, isdir(%s) != isdir(%s)", isdir_a, isdir_b, path_a, path_b);
	return 0;
    }

    if (isdir_a) {
	return db_equal_kv(path_a, path_b);
    } else {
	/*
	 * For flat-file ubik dbases, skip the ubik header (HDRSIZE), and just
	 * do a byte-for-byte comparison of all of the remaining bytes in the
	 * file. We skip the ubik header, so the dbases still appear "equal" if
	 * just the ubik version differs.
	 */
	return afstest_file_equal(path_a, path_b, HDRSIZE);
    }
}

static void
frztest_dump(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    char *dump_path = NULL;
    struct freeze_test *test = ops->rock;
    struct frztest_ops *frzops = test->ops;

    /* Dump the db from the running server, and verify that the dumped db
     * matches the original db. */

    dump_path = afstest_asprintf("%s/db.dump", info->confdir);

    ctl_run(info->confdir, frzops->suite, "db-dump", "-output %s", dump_path);

    ok(db_equal(test->db_path, dump_path), "dumped db matches");

    opr_Verify(udb_delpath(dump_path) == 0);

    free(dump_path);
}

static void
frztest_restore(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct freeze_test *test = ops->rock;
    struct frztest_ops *frzops = test->ops;
    char *blankdb_path = NULL;
    char *bak_path = NULL;
    char *args;

    /*
     * Start the server with a blank db (that is, tell ubiktest to not use an
     * existing db). Then restore the normal db to the vlserver, and verify
     * that .DB0 matches the normal db.
     *
     * If ->backup_db is set, dump the existing (blank) db before restoring.
     * When we restore, tell the server to backup the existing (blank) vldb,
     * and afterwards check that the backup matches the dumped blank db.
     */

    if (test->backup_db) {
	blankdb_path = afstest_asprintf("%s/blank.DB0", info->confdir);
	bak_path = afstest_asprintf("%s.BAK", info->db_path);

	ctl_run(info->confdir, frzops->suite, "db-dump", "-output %s", blankdb_path);

	args = "-backup-suffix .BAK";

    } else {
	args = "-no-backup";
    }

    ctl_run(info->confdir, frzops->suite, "db-restore",
	    "-input %s %s", test->db_path, args);

    ok(db_equal(test->db_path, info->db_path), "restored db matches");

    if (test->backup_db) {
	ok(db_equal(blankdb_path, bak_path), "db backup matches");
    }

    free(blankdb_path);
    free(bak_path);
}

static void
frztest_restore_invalid(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct freeze_test *test = ops->rock;
    struct frztest_ops *frzops = test->ops;
    char *cmd;
    struct afstest_cmdinfo cmdinfo;

    memset(&cmdinfo, 0, sizeof(cmdinfo));

    /*
     * Start the server with a normal db, then try to restore an invalid db.
     * Verify that the restore failed, and afterwards ubiktest will check that
     * the db contents are still as normal.
     */

    cmd = ctl_cmd(info->confdir, frzops->suite,
		  "db-restore", "-input %s -no-backup", test->baddb_path);

    cmdinfo.output = "\nopenafs-ctl: Failed to install db: I/O error writing dbase or log\n";
    cmdinfo.fd = STDERR_FILENO;
    cmdinfo.exit_code = 255;
    cmdinfo.command = cmd;

    is_command(&cmdinfo, "openafs-ctl %sdb-restore with invalid db fails",
	       frzops->suite);

    free(cmd);
}

static void
frztest_install(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct freeze_test *test = ops->rock;
    struct frztest_ops *frzops = test->ops;
    char *new_path;
    int code;
    int errno_save;

    /*
     * Start the server with a blank db. Then copy the src db to a tmp path,
     * install it, and then verify the installed db matches.
     */

    new_path = afstest_asprintf("%s.NEW", info->db_path);
    opr_Verify(ubik_CopyDB(test->db_path, new_path) == 0);

    ctl_run(info->confdir, frzops->suite, "db-install",
	    "-input %s -no-backup", new_path);

    ok(db_equal(test->db_path, info->db_path), "restored db matches");

    code = access(new_path, F_OK);
    errno_save = errno;

    is_int(-1, code, "post-install access(%s) fails", new_path);
    is_int(ENOENT, errno_save, "post-install access(%s) fails with ENOENT", new_path);

    free(new_path);
}

static void
frztest_revert(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct freeze_test *test = ops->rock;
    struct frztest_ops *frzops = test->ops;
    struct afstest_cmdinfo cmdinfo;
    struct afstest_cmdinfo cmdinfo_abort;
    char *fifo_start = NULL;
    char *fifo_end = NULL;
    char *db_restore = NULL;
    char *frz_abort = NULL;
    char *abort_cmd = NULL;
    char *cmd = NULL;

    memset(&cmdinfo, 0, sizeof(cmdinfo));
    memset(&cmdinfo_abort, 0, sizeof(cmdinfo_abort));

    opr_Assert(test->blankdb_path != NULL);
    opr_Assert(frzops->blank_cmd != NULL);
    opr_Assert(frzops->blank_cmd_stdout != NULL);

    /*
     * server is started with populated db. Inside a 'db-freeze-run', we
     * restore a blank db, then fail. The db should then revert back to the
     * original, populated db.
     */

    /* Restore the blank db to the running server */
    db_restore = ctl_cmd(info->confdir, frzops->suite, "db-restore",
			 "-input %s -no-backup -dist skip", test->blankdb_path);

    /*
     * ... but do the restore inside a db-freeze-run that fails, so the blank
     * db gets reverted. But before it fails, run e.g. 'vos listvldb' to make
     * sure the blank db is actually loaded and running on the server
     */

    if (test->revert_abort) {
	/* Cause the freeze to fail by running db-freeze-abort. */
	cmdinfo.exit_code = 255;
	frz_abort = ctl_cmd(info->confdir, frzops->suite, "db-freeze-abort", " ");
	cmd = ctl_cmd(info->confdir, frzops->suite,
		      "db-freeze-run", "-rw -cmd -- sh -c '"
			"%s && "
			"%s -config %s && "
			"%s"
		       "'",
		      db_restore, frzops->blank_cmd, info->confdir, frz_abort);

    } else if (test->revert_abort_force || test->revert_abort_id) {
	/*
	 * Cause the freeze to fail by running 'db-freeze-abort -force' (or
	 * 'db-freeze-abort -freezeid'). This is like the regular revert_abort
	 * case above, but we run 'db-freeze-abort' with those extra arguments.
	 * To make sure we're not getting the freeze information from the
	 * environment, we run 'db-freeze-abort' from outside the
	 * 'db-freeze-run' context.
	 *
	 * To run the abort outside of the freeze context, we have a couple of
	 * FIFOs (fifo_start, fifo_end) so we can start the abort (and wait for
	 * it to finish) from inside db-freeze-run.
	 *
	 * This looks a bit complex, so here's some pseudocode to try to help
	 * illustrate what we're doing here:
	 *
	 * cmdinfo_abort runs this in the background:
	 * read < abort_start.fifo && openafs-ctl vl db-freeze-abort -force ; e_code=$? ; echo > abort_end.fifo ; exit $e_code
	 *
	 * And cmdinfo runs this:
	 * openafs-ctl vl db-freeze-run -- \
	 *	sh -c 'openafs-ctl vl db-restore &&
	 *	       vos listvldb &&
	 *	       echo > abort_start.fifo &&
	 *	       read < abort_end.fifo'
	 */

	fifo_start = afstest_asprintf("%s/abort_start.fifo", info->confdir);
	fifo_end = afstest_asprintf("%s/abort_end.fifo", info->confdir);
	opr_Verify(mkfifo(fifo_start, 0700) == 0);
	opr_Verify(mkfifo(fifo_end, 0700) == 0);

	if (test->revert_abort_force) {
	    frz_abort = ctl_cmd(info->confdir, frzops->suite,
				"db-freeze-abort", "-force");
	} else {
	    opr_Assert(test->revert_abort_id);
	    frz_abort = ctl_cmd(info->confdir, frzops->suite,
				"db-freeze-abort", "-freezeid $freezeid");
	}
	abort_cmd = afstest_asprintf("read freezeid < %s && "
				     "%s ; e_code=$? ; "
				     "echo > %s ; exit $e_code",
				     fifo_start, frz_abort, fifo_end);

	cmdinfo_abort.output = "";
	cmdinfo_abort.fd = STDOUT_FILENO;
	cmdinfo_abort.command = abort_cmd;
	afstest_command_start(&cmdinfo_abort);

	cmdinfo.exit_code = 255;
	cmd = ctl_cmd(info->confdir, frzops->suite,
		      "db-freeze-run", "-rw -cmd -- sh -c ' "
			"%s && "
			"%s -config %s && "
			"echo $OPENAFS_%s_FREEZE_ID > %s && read line < %s"
		       " ' ",
		      db_restore, frzops->blank_cmd, info->confdir,
		      frzops->freeze_envname, fifo_start, fifo_end);

    } else if (test->revert_timeout) {
	/*
	 * Cause the freeze to fail by timeout. Set the free to timeout after
	 * 200ms, and sleep inside the freeze for 210ms.
	 *
	 * Use perl for sleeping, since sub-second sleep(1) is not portable.
	 */
	cmdinfo.exit_code = 255;

	cmd = ctl_cmd(info->confdir, frzops->suite,
		      "db-freeze-run", "-rw -timeout-ms 200 -cmd -- sh -c ' "
			"%s && "
			"%s -config %s && "
			"perl -MTime::HiRes=usleep -e \"usleep(210000)\""
		       " ' ",
		      db_restore, frzops->blank_cmd, info->confdir);

    } else if (test->revert_exit) {
	/* Cause the freeze to fail because the underlying command exits with a
	 * non-zero exit code. */
	cmdinfo.exit_code = 1;
	cmd = ctl_cmd(info->confdir, frzops->suite,
		      "db-freeze-run", "-rw -cmd -- sh -c ' "
			"%s && "
			"%s -config %s && "
			"exit 1"
		       " ' ",
		      db_restore, frzops->blank_cmd, info->confdir);
    } else {
	opr_Assert(0);
    }

    cmdinfo.output = frzops->blank_cmd_stdout;
    cmdinfo.fd = STDOUT_FILENO;
    cmdinfo.command = cmd;

    if (!is_command(&cmdinfo, "nested db-restore fails correctly")) {
	if (cmdinfo_abort.child != 0) {
	    /* Kill out cmdinfo_abort command, since it may be hanging waiting
	     * on a fifo. */
	    kill(cmdinfo_abort.child, SIGTERM);
	}
    }

    if (cmdinfo_abort.command != NULL) {
	afstest_command_end(&cmdinfo_abort,
			    "separate db-freeze-abort command runs successfully");
    }

    ok(db_equal(test->db_path, info->db_path), "reverted db matches");

    free(fifo_start);
    free(fifo_end);
    free(db_restore);
    free(frz_abort);
    free(abort_cmd);
    free(cmd);
}

static void
frztest_dist(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    struct freeze_test *test = ops->rock;
    struct frztest_ops *frzops = test->ops;
    char *db_restore = NULL;
    char *frz_dist = NULL;
    char *cmd = NULL;
    struct afstest_cmdinfo cmdinfo;

    memset(&cmdinfo, 0, sizeof(cmdinfo));

    /*
     * server is started without a db. We install a db with a nested
     * 'db-restore -dist skip' that eventually fails like in frztest_revert,
     * but we also run db-freeze-dist, so the db never gets reverted.
     */

    db_restore = ctl_cmd(info->confdir, frzops->suite, "db-restore",
			 "-input %s -no-backup -dist skip", test->db_path);
    frz_dist = ctl_cmd(info->confdir, frzops->suite, "db-freeze-dist", " ");

    cmd = ctl_cmd(info->confdir, frzops->suite,
		  "db-freeze-run", "-rw -cmd -- sh -c '"
		    "%s && %s && exit 1"
		   "'",
		  db_restore, frz_dist);

    cmdinfo.output = "";
    cmdinfo.fd = STDOUT_FILENO;
    cmdinfo.command = cmd;
    cmdinfo.exit_code = 1;

    is_command(&cmdinfo, "nested db-restore/dist fails correctly");

    ok(db_equal(test->db_path, info->db_path), "db matches");

    free(db_restore);
    free(frz_dist);
    free(cmd);
}

void
frztest_runtests(struct ubiktest_dataset *ds, struct frztest_ops *ops)
{
    char *src_db = NULL;
    char *blankdb_path = NULL;
    char *baddb_path = NULL;
    struct ubiktest_dbdef *dbdef;
    char *skip_reason = NULL;
    struct ubiktest_ops utest;
    struct freeze_test frztest;
    int have_baddb = 0;
    int db_kv = 0;
    int pass;

    memset(&utest, 0, sizeof(utest));
    memset(&frztest, 0, sizeof(frztest));

    ctl_path = afstest_obj_path("src/ctl/openafs-ctl");
    ctl_sockname = ds->server_type->ctl_sock;

    opr_Assert(ctl_sockname != NULL);

    dbdef = find_dbdef(ds, ops->use_db);
    if (dbdef != NULL) {
	src_db = get_dbpath(dbdef, &skip_reason);
	if (dbdef->kv_path != NULL) {
	    db_kv = 1;
	}
    }
    blankdb_path = get_dbpath(&ops->blankdb, &skip_reason);
    baddb_path = get_dbpath_optional(&ops->baddb, &have_baddb, &skip_reason);

    frztest.ops = ops;
    frztest.db_path = src_db;
    frztest.blankdb_path = blankdb_path;
    frztest.baddb_path = baddb_path;

    {
	utest.rock = &frztest;
	utest.descr = afstest_asprintf("dump %s", ops->use_db);
	utest.skip_reason = skip_reason;
	utest.use_db = ops->use_db;
	utest.post_start = frztest_dump;
	utest.server_argv = ops->server_argv;
	utest.result_kv = db_kv;
	utest.n_tests = 2;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
    for (pass = 1; pass <= 2; pass++) {
	utest.rock = &frztest;
	utest.skip_reason = skip_reason;
	utest.use_db = "none";
	utest.post_start = frztest_restore;
	utest.server_argv = ops->server_argv;
	utest.result_kv = db_kv;

	if (pass == 1) {
	    utest.n_tests = 2;
	    utest.descr = afstest_asprintf("restore %s", ops->use_db);
	} else {
	    frztest.backup_db = 1;
	    utest.n_tests = 4;
	    utest.descr = afstest_asprintf("restore %s with backup", ops->use_db);
	}
	ubiktest_runtest(ds, &utest);

	frztest.backup_db = 0;
	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
    if (have_baddb) {
	utest.rock = &frztest;
	utest.descr = afstest_asprintf("restore invalid db over %s", ops->use_db);
	utest.use_db = ops->use_db;
	utest.post_start = frztest_restore_invalid;
	utest.server_argv = ops->server_argv;
	utest.result_kv = db_kv;
	utest.n_tests = 1;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
    {
	utest.rock = &frztest;
	utest.descr = afstest_asprintf("install %s", ops->use_db);
	utest.skip_reason = skip_reason;
	utest.use_db = "none";
	utest.post_start = frztest_install;
	utest.server_argv = ops->server_argv;
	utest.result_kv = db_kv;
	utest.n_tests = 4;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
    for (pass = 1; pass <= 5; pass++) {
	utest.rock = &frztest;
	if (pass == 1) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-exit %s",
					   ops->use_db);
	    utest.n_tests = 2;
	    frztest.revert_exit = 1;
	} else if (pass == 2) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-abort %s",
					   ops->use_db);
	    utest.n_tests = 2;
	    frztest.revert_abort = 1;
	} else if (pass == 3) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-abort-force %s",
					   ops->use_db);
	    utest.n_tests = 3;
	    frztest.revert_abort_force = 1;
	} else if (pass == 4) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-abort-id %s",
					   ops->use_db);
	    utest.n_tests = 3;
	    frztest.revert_abort_id = 1;
	} else if (pass == 5) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-timeout %s",
					   ops->use_db);
	    utest.n_tests = 2;
	    frztest.revert_timeout = 1;
	} else {
	    opr_Assert(0);
	}

	utest.skip_reason = skip_reason;
	utest.use_db = ops->use_db;
	utest.post_start = frztest_revert;
	utest.server_argv = ops->server_argv;
	utest.result_kv = db_kv;

	ubiktest_runtest(ds, &utest);

	frztest.revert_exit = 0;
	frztest.revert_abort = 0;
	frztest.revert_abort_force = 0;
	frztest.revert_abort_id = 0;
	frztest.revert_timeout = 0;
	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
    {
	utest.rock = &frztest;
	utest.descr = afstest_asprintf("db-freeze-dist %s", ops->use_db);
	utest.skip_reason = skip_reason;
	utest.use_db = "none",
	utest.post_start = frztest_dist;
	utest.server_argv = ops->server_argv;
	utest.result_kv = db_kv;
	utest.n_tests = 2;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }

    free(src_db);
    free(blankdb_path);
    free(baddb_path);
    free(skip_reason);

    free(ctl_path);
    ctl_path = NULL;
    ctl_sockname = NULL;
}

/*
 * Run our 'freeze tests' for the given scenario. This is different from
 * frztest_runtests() because here, we do things like run with an existing
 * flatfile database, and restore a KV database over it. We're not actually
 * "converting" a database between different formats; we're just testing going
 * from one format to another (which is why we need two 'frztest_ops'
 * arguments).
 */
void
frztest_runtests_convert(struct ubiktest_dataset *ds, struct frztest_ops *ops_a,
			 struct frztest_ops *ops_b)
{
    char *db_path_a = NULL;
    char *db_path_b = NULL;
    char *blank_b = NULL;
    char *skip_reason = NULL;
    int iskv_a = 0;
    int iskv_b = 0;
    struct ubiktest_dbdef *dbdef;
    struct ubiktest_ops utest;
    struct freeze_test frztest;
    int pass;

    memset(&utest, 0, sizeof(utest));
    memset(&frztest, 0, sizeof(frztest));

    ctl_path = afstest_obj_path("src/ctl/openafs-ctl");
    ctl_sockname = ds->server_type->ctl_sock;

    opr_Assert(ctl_sockname != NULL);

    dbdef = find_dbdef(ds, ops_a->use_db);
    if (dbdef != NULL) {
	db_path_a = get_dbpath(dbdef, &skip_reason);
	if (dbdef->kv_path != NULL) {
	    iskv_a = 1;
	}
    }

    dbdef = find_dbdef(ds, ops_b->use_db);
    if (dbdef != NULL) {
	db_path_b = get_dbpath(dbdef, &skip_reason);
	if (dbdef->kv_path != NULL) {
	    iskv_b = 1;
	}
    }

    blank_b = get_dbpath(&ops_b->blankdb, &skip_reason);

    frztest.ops = ops_a;

    /* restore ops_a -> ops_b, with and without backup */
    for (pass = 1; pass <= 2; pass++) {
	utest.rock = &frztest;
	utest.skip_reason = skip_reason;
	utest.use_db = "none";
	utest.post_start = frztest_restore;
	utest.server_argv = ops_a->server_argv;
	utest.result_kv = iskv_b;

	frztest.db_path = db_path_b;

	if (pass == 1) {
	    utest.n_tests = 2;
	    utest.descr = afstest_asprintf("restore %s -> %s",
					   ops_a->use_db, ops_b->use_db);
	} else {
	    frztest.backup_db = 1;
	    utest.n_tests = 4;
	    utest.descr = afstest_asprintf("restore %s -> %s with backup",
					   ops_a->use_db, ops_b->use_db);
	}
	ubiktest_runtest(ds, &utest);

	frztest.backup_db = 0;
	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }

    /* install ops_a -> ops_b */
    {
	utest.rock = &frztest;
	utest.descr = afstest_asprintf("install %s -> %s",
				       ops_a->use_db, ops_b->use_db);
	utest.skip_reason = skip_reason;
	utest.use_db = "none";
	utest.post_start = frztest_install;
	utest.server_argv = ops_a->server_argv;
	utest.result_kv = iskv_b;
	utest.n_tests = 4;

	frztest.db_path = db_path_b;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }

    /* revert an install from ops_a -> ops_b, via
     * exit/abort/abort-force/abort-id/timeout */
    for (pass = 1; pass <= 5; pass++) {
	utest.rock = &frztest;
	if (pass == 1) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-exit %s -> %s",
					   ops_a->use_db, ops_b->use_db);
	    utest.n_tests = 2;
	    frztest.revert_exit = 1;
	} else if (pass == 2) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-abort %s -> %s",
					   ops_a->use_db, ops_b->use_db);
	    utest.n_tests = 2;
	    frztest.revert_abort = 1;
	} else if (pass == 3) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-abort-force %s -> %s",
					   ops_a->use_db, ops_b->use_db);
	    utest.n_tests = 3;
	    frztest.revert_abort_force = 1;
	} else if (pass == 4) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-abort-id %s -> %s",
					   ops_a->use_db, ops_b->use_db);
	    utest.n_tests = 3;
	    frztest.revert_abort_id = 1;
	} else if (pass == 5) {
	    utest.descr = afstest_asprintf("db-freeze-run revert-timeout %s -> %s",
					   ops_a->use_db, ops_b->use_db);
	    utest.n_tests = 2;
	    frztest.revert_timeout = 1;
	} else {
	    opr_Assert(0);
	}

	utest.skip_reason = skip_reason;
	utest.use_db = ops_a->use_db;
	utest.post_start = frztest_revert;
	utest.result_kv = iskv_a;

	frztest.db_path = db_path_a;
	frztest.blankdb_path = blank_b;

	ubiktest_runtest(ds, &utest);

	frztest.revert_exit = 0;
	frztest.revert_abort = 0;
	frztest.revert_abort_force = 0;
	frztest.revert_abort_id = 0;
	frztest.revert_timeout = 0;
	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }

    /* dist a restore from ops_a -> ops_b */
    {
	utest.rock = &frztest;
	utest.descr = afstest_asprintf("db-freeze-dist %s -> %s",
				       ops_a->use_db, ops_b->use_db);
	utest.skip_reason = skip_reason;
	utest.use_db = "none",
	utest.post_start = frztest_dist;
	utest.server_argv = ops_a->server_argv;
	utest.result_kv = iskv_b;
	utest.n_tests = 2;

	frztest.db_path = db_path_b;
	frztest.blankdb_path = NULL;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }

    free(db_path_a);
    free(db_path_b);
    free(blank_b);
    free(skip_reason);

    free(ctl_path);
    ctl_path = NULL;
}

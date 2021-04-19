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
#include <ubik_internal.h>

#include "common.h"

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
valid_db(char *path)
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
	     char *dirname)
{
    struct ubiktest_dbtest *test;
    for (test = testlist; test->descr != NULL; test++) {
	if (test->func != NULL) {
	    (*test->func)(dirname);
	} else {
	    opr_Assert(ds->dbtest_func != NULL);
	    (*ds->dbtest_func)(dirname, test);
	}
    }
}

/* Return the path to the database specified in 'dbdef'. Caller must free the
 * returned string. */
static char *
get_dbpath(struct ubiktest_dbdef *dbdef)
{
    char *path;

    opr_Assert(dbdef->flat_path != NULL);
    path = afstest_src_path(dbdef->flat_path);
    opr_Assert(path != NULL);

    return path;
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

/**
 * Run db tests for the given dataset, for the given scenario ops.
 *
 * @param[in] db    The database dataset to run against (e.g. vlsmall).
 * @param[in] ops   The scenario to run.
 */
void
ubiktest_runtest(struct ubiktest_dataset *ds, struct ubiktest_ops *ops)
{
    struct afstest_server_type *server = ds->server_type;
    struct afsconf_dir *dir;
    int code;
    pid_t pid = 0;
    struct rx_securityClass *secClass = NULL;
    int secIndex = 0;
    char *src_db = NULL;
    char *db_path = NULL;
    char *db_copy = NULL;
    char *dirname = NULL;
    char *use_db = ops->use_db;
    struct ubiktest_dbdef *dbdef;
    struct ubiktest_dbtest *testlist;
    struct stat st;
    struct ubiktest_cbinfo cbinfo;
    struct afstest_server_opts opts;
    const char *progname = getprogname();

    memset(&cbinfo, 0, sizeof(cbinfo));
    memset(&opts, 0, sizeof(opts));

    opr_Assert(progname != NULL);

    dirname = afstest_BuildTestConfig(NULL);
    opr_Assert(dirname != NULL);

    diag("ubiktest dataset: %s, ops: %s, dir: %s",
	 ds->descr, ops->descr, dirname);

    dir = afsconf_Open(dirname);
    opr_Assert(dir != NULL);
    cbinfo.confdir = dirname;

    db_path = afstest_asprintf("%s/%s.DB0", dirname, server->db_name);
    cbinfo.db_path = db_path;

    /* Get the path to the sample db we're using. */

    if (use_db == NULL && ops->create_db) {
	use_db = "none";
    }

    opr_Assert(use_db != NULL);
    dbdef = find_dbdef(ds, use_db);
    if (dbdef != NULL) {
	src_db = get_dbpath(dbdef);
    }
    cbinfo.src_dbdef = dbdef;

    /* Copy the sample db into place. */
    if (src_db != NULL) {
	code = afstest_cp(src_db, db_path);
	if (code != 0) {
	    afs_com_err(progname, code,
			"while copying %s into place", src_db);
	    goto error;
	}
    }

    if (ops->pre_start != NULL) {
	(*ops->pre_start)(&cbinfo, ops);
    }

    opts.server = server;
    opts.dirname = dirname;
    opts.serverPid = &pid;
    opts.extra_argv = ops->server_argv;

    code = afstest_StartServerOpts(&opts);
    if (code != 0) {
	afs_com_err(progname, code, "while starting server");
	goto error;
    }

    code = afsconf_ClientAuthSecure(dir, &secClass, &secIndex);
    if (code != 0) {
	afs_com_err(progname, code, "while building security class");
	goto error;
    }

    cbinfo.disk_conn = rx_NewConnection(afstest_MyHostAddr(),
					htons(ds->server_type->port),
					DISK_SERVICE_ID, secClass, secIndex);
    opr_Assert(cbinfo.disk_conn != NULL);

    if (ds->uclientp != NULL) {
	code = afstest_GetUbikClient(dir, server->service_name, USER_SERVICE_ID,
				     secClass, secIndex, ds->uclientp);
	if (code != 0) {
	    afs_com_err(progname, code, "while building ubik client");
	    goto error;
	}
    }

    if (ops->create_db) {
	if (ds->create_func == NULL) {
	    bail("create_db set, but we have no create_func");
	}

	(*ds->create_func)(dirname);

	db_copy = afstest_asprintf("%s.copy", db_path);

	code = afstest_cp(db_path, db_copy);
	if (code != 0) {
	    afs_com_err(progname, code, "while copying %s -> %s", db_path, db_copy);
	    goto error;
	}

	diag("Copy of created db saved in %s", db_copy);
    }

    if (ops->post_start != NULL) {
	(*ops->post_start)(&cbinfo, ops);
    }

    if (ops->extra_dbtests != NULL) {
	run_testlist(ds, ops->extra_dbtests, dirname);
    }
    testlist = ds->tests;
    if (ops->override_dbtests != NULL && ops->override_dbtests[0].descr != NULL) {
	testlist = ops->override_dbtests;
    }
    run_testlist(ds, testlist, dirname);

    code = afstest_StopServer(pid);
    pid = 0;
    is_int(0, code, "server exited cleanly");
    if (code != 0) {
	afs_com_err(progname, code, "while stopping server");
	goto error;
    }

    code = lstat(db_path, &st);
    if (code != 0) {
	sysdiag("lstat(%s)", db_path);
    }
    is_int(0, code, ".DB0 file exists");
    if (code != 0) {
	goto error;
    }

    ok(S_ISREG(st.st_mode),
       "db %s is a regular file (mode 0x%x)", db_path,
       (unsigned)st.st_mode);

    ok(valid_db(db_path),
       "db %s is a valid ubik db", db_path);

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

    if (pid != 0) {
	int stop_code = afstest_StopServer(pid);
	if (stop_code != 0) {
	    afs_com_err(progname, stop_code, "while stopping server");
	    if (code == 0) {
		code = 1;
	    }
	}
    }

    if (code != 0) {
	bail("encountered unexpected error");
    }

    free(src_db);
    free(db_path);
    free(db_copy);
    if (dirname != NULL) {
	afstest_rmdtemp(dirname);
	free(dirname);
    }
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
    struct stat st;
    struct ubik_hdr uhdr;
    afs_uint32 length;
    FILE *fh;
    int code;
    struct rx_call *rxcall = rx_NewCall(info->disk_conn);

    memset(&uhdr, 0, sizeof(uhdr));
    memset(&st, 0, sizeof(st));

    code = stat(db_path, &st);
    if (code < 0) {
	sysbail("fstat(%s)", db_path);
    }

    fh = fopen(db_path, "r");
    if (fh == NULL) {
	sysbail("fopen(%s)", db_path);
    }

    opr_Assert(st.st_size >= HDRSIZE);
    length = st.st_size - HDRSIZE;

    code = fread(&uhdr, sizeof(uhdr), 1, fh);
    opr_Assert(code == 1);

    fclose(fh);

    uhdr.version.epoch = ntohl(uhdr.version.epoch);
    uhdr.version.counter = ntohl(uhdr.version.epoch);

    code = StartDISK_SendFile(rxcall, 0, length, &uhdr.version);
    opr_Assert(code == 0);

    code = rx_send_file(rxcall, db_path, HDRSIZE, length);

    code = rx_EndCall(rxcall, code);
    is_int(0, code, "DISK_SendFile call succeeded");
}

void
urectest_runtests(struct ubiktest_dataset *ds, char *use_db)
{
    struct ubiktest_ops utest;
    struct ubiktest_dbdef *dbdef;
    char *db_path = NULL;

    dbdef = find_dbdef(ds, use_db);
    opr_Assert(dbdef != NULL);
    db_path = get_dbpath(dbdef);

    memset(&utest, 0, sizeof(utest));

    {
	utest.rock = db_path;
	utest.descr = afstest_asprintf("run DISK_GetFile for %s", use_db);
	utest.use_db = use_db;
	utest.post_start = run_getfile;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
    {
	utest.rock = db_path;
	utest.descr = afstest_asprintf("run DISK_SendFile for %s", use_db);
	utest.use_db = "none";
	utest.post_start = run_sendfile;

	ubiktest_runtest(ds, &utest);

	free(utest.descr);
	memset(&utest, 0, sizeof(utest));
    }
}

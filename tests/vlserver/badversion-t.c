#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>

#include <rx/rx.h>
#include <ubik.h>

#include <afs/com_err.h>
#include <afs/vlserver.h>
#include <afs/cellconfig.h>

#include <tests/tap/basic.h>

#include "common.h"

static char *dirname;
static char *db_path;
static struct ubik_client *uclient;
static char* progname;

static char uheader[64] =
"\x00\x35\x45\x45\x00\x00\x00\x40" /* magic, pad1, size */
"\x5E\x00\x00\x00\x00\x00\x00\x01" /* epoch, version */
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00";

static struct {
    afs_uint32 version;
    char padding[1020];
} dbdata;

struct testinfo {
    char *descr;
    afs_uint32 dbversion;
};

static struct testinfo vlbad_tests[] = {
    {
	.descr = "Got VL_BADVERSION for old version 1",
	.dbversion = 1,
    },
    {
	.descr = "Got VL_BADVERSION for bad new version 5",
	.dbversion = 5,
    },
    {
	.descr = "Got VL_BADVERSION for bad version 6",
	.dbversion = 6,
    },
    {
	.descr = "Got VL_BADVERSION for bad version 1073741828",
	.dbversion = 1073741828,
    },
    {
	.descr = "Got VL_BADVERSION for bad version 131072",
	.dbversion = 131072,
    },
    {0}
};

static int
runtest(struct testinfo *test)
{
    int code;
    struct nvldbentry entry;
    pid_t vlpid = 0;
    FILE *fh = NULL;

    fh = fopen(db_path, "w");
    if (fh == NULL) {
	sysdiag("fopen(%s)", db_path);
	code = EIO;
	goto done;
    }

    code = fwrite(uheader, sizeof(uheader), 1, fh);
    opr_Assert(code == 1);

    dbdata.version = htonl(test->dbversion);

    code = fwrite(&dbdata, sizeof(dbdata), 1, fh);
    opr_Assert(code == 1);

    fclose(fh);
    fh = NULL;

    code = afstest_StartVLServer(dirname, &vlpid);
    if (code != 0) {
	afs_com_err(progname, code, "while starting vlserver");
	goto done;
    }

    code = ubik_VL_GetEntryByNameN(uclient, 0, "root.cell", &entry);
    is_int(VL_BADVERSION, code, "%s", test->descr);

    code = 0;

 done:
    if (vlpid != 0) {
	int code_stop = afstest_StopServer(vlpid);
	is_int(0, code_stop, "vlserver exited cleanly");
	if (code_stop != 0) {
	    afs_com_err(progname, code_stop, "while stopping vlserver");
	}
	if (code == 0) {
	    code = code_stop;
	}
    }
    return code;
}

int
main(int argc, char **argv)
{
    struct afsconf_dir *dir;
    int code = 0;
    struct rx_securityClass *secClass = NULL;
    int secIndex = 0;
    struct testinfo *test;

    /* Skip all tests if the current hostname can't be resolved */
    afstest_SkipTestsIfBadHostname();
    /* Skip all tests if the current hostname is on the loopback network */
    afstest_SkipTestsIfLoopbackNetIsDefault();
    /* Skip all tests if a vlserver is already running on this system. */
    afstest_SkipTestsIfServerRunning("afs3-vlserver");

    plan(10);

    progname = afstest_GetProgname(argv);
    dirname = afstest_BuildTestConfig(NULL);

    code = rx_Init(0);
    opr_Assert(code == 0);

    db_path = afstest_asprintf("%s/vldb.DB0", dirname);
    dir = afsconf_Open(dirname);
    opr_Assert(dir != NULL);

    code = afsconf_ClientAuthSecure(dir, &secClass, &secIndex);
    if (code != 0) {
	afs_com_err(progname, code, "while building security class");
	code = 1;
	goto done;
    }

    code = afstest_GetUbikClient(dir, AFSCONF_VLDBSERVICE, USER_SERVICE_ID,
				 secClass, secIndex, &uclient);
    if (code != 0) {
	afs_com_err(progname, code, "while building ubik client");
	code = 1;
	goto done;
    }

    for (test = vlbad_tests; test->descr != NULL; test++) {
	code = runtest(test);
	if (code != 0) {
	    code = 1;
	    goto done;
	}
    }

 done:
    afstest_rmdtemp(dirname);
    return code;
}

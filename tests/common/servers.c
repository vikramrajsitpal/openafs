#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <poll.h>

#include <rx/rx.h>

#include <afs/authcon.h>
#include <afs/cellconfig.h>
#define UBIK_INTERNALS
#include <ubik.h>

#include <tests/tap/basic.h>

#include "common.h"

struct afstest_server_type afstest_server_pt = {
    .logname = "PtLog",
    .bin_path = "src/tptserver/ptserver",
    .db_name = "prdb",
    .exec_name = "ptserver",
    .startup_string = "Starting AFS ptserver",
    .service_name = AFSCONF_PROTSERVICE,
    .port = 7002,
};

struct afstest_server_type afstest_server_vl = {
    .logname = "VLLog",
    .bin_path = "src/tvlserver/vlserver",
    .db_name = "vldb",
    .exec_name = "vlserver",
    .startup_string = "Starting AFS vlserver",
    .service_name = AFSCONF_VLDBSERVICE,
    .port = 7003,
};

static void
check_startup(struct afstest_server_type *server, pid_t pid, char *log,
	      int *a_started, int *a_stopped)
{
    int status;
    struct rx_connection *conn;
    struct ubik_debug udebug;
    afs_int32 isclone;
    pid_t exited;
    int code;

    memset(&udebug, 0, sizeof(udebug));

    opr_Assert(pid > 0);

    exited = waitpid(pid, &status, WNOHANG);
    if (exited < 0) {
	opr_Assert(errno == ECHILD);
    }

    if (exited != 0) {
	/* pid is no longer running; vlserver must have died during startup */
	*a_stopped = 1;
	return;
    }

    if (!afstest_file_contains(log, server->startup_string)) {
	/* server hasn't logged the e.g. "Starting AFS vlserver" line yet, so
	 * it's presumably still starting up. */
	return;
    }

    /*
     * If we're going to write to the db, we also need to wait until recovery
     * has the UBIK_RECHAVEDB state bit (without that, we won't be able to
     * start write transactions). If we're just reading from the db, we
     * wouldn't need to wait for this, but we don't know whether our caller
     * will be writing to the db or not. It shouldn't take long for
     * UBIK_RECHAVEDB to get set anyway, so just always check if it's been set
     * (via VOTE_XDebug).
     */

    conn = rx_NewConnection(afstest_MyHostAddr(), htons(server->port),
			    VOTE_SERVICE_ID,
			    rxnull_NewClientSecurityObject(), 0);
    code = VOTE_XDebug(conn, &udebug, &isclone);
    rx_DestroyConnection(conn);
    if (code != 0) {
	diag("VOTE_XDebug returned %d while waiting for server startup",
	     code);
	return;
    }

    if (udebug.amSyncSite && (udebug.recoveryState & UBIK_RECHAVEDB) != 0) {
	/* Okay, it's set! We have finished startup. */
	*a_started = 1;
    }
}

/*
 * Start up the given server, using the configuration in dirname, and putting
 * our logs there too.
 */
int
afstest_StartServer(struct afstest_server_type *server, char *dirname, pid_t *serverPid)
{
    pid_t pid;
    char *logPath;
    int started = 0;
    int stopped = 0;
    int try;
    FILE *fh;
    int code = 0;

    logPath = afstest_asprintf("%s/%s", dirname, server->logname);

    /* Create/truncate the log in advance (since we look at it to detect when
     * the server has started). */
    fh = fopen(logPath, "w");
    opr_Assert(fh != NULL);
    fclose(fh);

    pid = fork();
    if (pid == -1) {
	exit(1);
	/* Argggggghhhhh */
    } else if (pid == 0) {
	char *binPath, *dbPath;

	/* Child */
	binPath = afstest_obj_path(server->bin_path);
	dbPath = afstest_asprintf("%s/%s", dirname, server->db_name);

	execl(binPath, server->exec_name,
	      "-logfile", logPath, "-database", dbPath, "-config", dirname, NULL);
	fprintf(stderr, "Running %s failed\n", binPath);
	exit(1);
    }

    /*
     * Wait for the vlserver to startup. Try to check if the vlserver is ready
     * by checking the log file and the urecovery_state (check_startup()), but
     * if it's taking too long, just return success anyway. If the vlserver
     * isn't ready yet, then the caller's tests may fail, but we can't wait
     * forever.
     */

    diag("waiting for server to startup");

    usleep(5000); /* 5ms */
    check_startup(server, pid, logPath, &started, &stopped);
    for (try = 0; !started && !stopped; try++) {
	if (try > 100 * 5) {
	    diag("waited too long for server to finish starting up; "
		 "proceeding anyway");
	    goto done;
	}

	usleep(1000 * 10); /* 10ms */
	check_startup(server, pid, logPath, &started, &stopped);
    }

    if (stopped) {
	fprintf(stderr, "server died during startup\n");
	code = -1;
	goto done;
    }

    diag("server started after try %d", try);

 done:
    *serverPid = pid;

    free(logPath);

    return code;
}

int
afstest_StopServer(pid_t serverPid)
{
    int status;

    kill(serverPid, SIGTERM);

    if (waitpid(serverPid, &status, 0) < 0) {
	sysbail("waitpid");
    }

    if (WIFSIGNALED(status) && WTERMSIG(status) != SIGTERM) {
	fprintf(stderr, "Server died exited on signal %d\n", WTERMSIG(status));
	return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
	fprintf(stderr, "Server exited with code %d\n", WEXITSTATUS(status));
	return -1;
    }
    return 0;
}

static void *
exit_thread(void *rock)
{
    int end_fd = *((int*)rock);

    for (;;) {
	int nbytes;
	int dummy;

	nbytes = read(end_fd, &dummy, sizeof(dummy));
	if (nbytes >= 0) {
	    exit(0);
	}
	if (errno != EINTR) {
	    sysbail("read(server end_fd)");
	}
    }

    return NULL;
}

/**
 * Setup a custom Rx service to run.
 *
 * This doesn't actually call rx_StartServer(); it just sets up the relevant Rx
 * service. The caller must call rx_StartServer() itself at some point.
 */
int
afstest_StartTestRPCService(const char *configPath,
			    char *serviceName,
			    u_short port,
			    u_short serviceId,
			    afs_int32 (*proc) (struct rx_call *))
{
    struct afsconf_dir *dir;
    struct rx_securityClass **classes;
    afs_int32 numClasses;
    int code;
    struct rx_service *service;
    struct afsconf_bsso_info bsso;

    memset(&bsso, 0, sizeof(bsso));

    dir = afsconf_Open(configPath);
    if (dir == NULL) {
        fprintf(stderr, "Server: Unable to open config directory\n");
        return -1;
    }

    code = rx_Init(htons(port));
    if (code != 0) {
	fprintf(stderr, "Server: Unable to initialise RX\n");
	return -1;
    }

    bsso.dir = dir;
    afsconf_BuildServerSecurityObjects_int(&bsso, &classes, &numClasses);

    service = rx_NewService(0, serviceId, serviceName, classes, numClasses,
			    proc);
    if (service == NULL) {
        fprintf(stderr, "Server: Unable to start to test service\n");
        return -1;
    }

    return 0;
}

/**
 * Run the specified function in a forked process.
 *
 * When this function is called, we fork, and run proc() in the child process.
 * When proc() returns success, we signal to the parent process that it has
 * finished, and then this function returns. The child process does not exit
 * until the parent process dies; in the meantime, it stays in a
 * rx_StartServer() loop.
 *
 * If proc() returns an error, the child process exits immediately, and the
 * parent process bails.
 */
void
afstest_ForkRxProc(int (*proc)(void *rock), void *rock)
{
    int start_fds[2];
    int end_fds[2];
    int code;
    unsigned char byte;
    ssize_t nbytes;
    pid_t pid;
    struct pollfd pfd;

    memset(&pfd, 0, sizeof(pfd));

    /*
     * 'start_fds' are the pipe we use to read the "I am ready" message from
     * the child process, so we can wait until the child is ready to serve
     * requests, or it's done doing whatever it's going to do.
     */
    code = pipe(start_fds);
    if (code < 0) {
	sysbail("pipe");
    }

    /*
     * 'end_fds' are the pipe the child pid uses to see if it can quit. The
     * child process just tries to read from the pipe, and when the pipe is
     * closed, the child process knows it can exit. In this process, we never
     * write anything to it; we just let the pipe close when we exit. This way,
     * the child should always exit after this process exits, even if we
     * segfault or abort, etc.
     */
    code = pipe(end_fds);
    if (code < 0) {
	sysbail("pipe");
    }

    pid = fork();
    if (pid < 0) {
	sysbail("fork");
    }
    if (pid == 0) {
	unsigned char nul = '\0';
	pthread_t tid;

	close(start_fds[0]);
	close(end_fds[1]);

	opr_Verify(pthread_create(&tid, NULL, exit_thread, &end_fds[0]) == 0);

	code = (*proc)(rock);
	if (code != 0) {
	    exit(code);
	}

	if (write(start_fds[1], &nul, sizeof(nul)) != sizeof(nul)) {
	    sysbail("write");
	}

	rx_StartServer(1);

	exit(0);
    }

    close(start_fds[1]);
    close(end_fds[0]);

    /* Wait 5 seconds for the server pid to write something to the start_fds
     * pipe. */
    pfd.fd = start_fds[0];
    pfd.events = POLLIN;
    code = poll(&pfd, 1, 5000);
    if (code < 0) {
	sysbail("poll");
    }
    if (code == 0) {
	bail("child pid (%d) failed to indicate readiness in 5 seconds",
	     (int)pid);
    }

    nbytes = read(start_fds[0], &byte, sizeof(byte));
    if (nbytes != sizeof(byte)) {
	sysbail("read");
    }

    if (byte != 0) {
	bail("bad byte from child: %d", (int)byte);
    }

    /* end_fds[1] will be closed when we eventually exit */
}


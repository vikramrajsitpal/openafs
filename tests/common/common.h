/*
 * Copyright (c) 2010 Your File System Inc. All rights reserved.
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

#ifndef OPENAFS_TEST_COMMON_H
#define OPENAFS_TEST_COMMON_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include <roken.h>

#include <afs/opr.h>
#include <afs/com_err.h>
#include <rx/rx_opaque.h>

#include <tests/tap/basic.h>

/* config.c */

struct afstest_configinfo {
    /* Skip adding keys to the created conf dir. */
    int skipkeys;

    /* If not NULL, create a NetInfo file specifying this host, forcing servers
     * to use that ip. */
    char *force_host;

    /* List of IP addresses to use for dbservers for the cell. */
    struct sockaddr_storage *dbserver_addrs;
    int dbserver_addrs_len;
};
extern char *afstest_BuildTestConfig(struct afstest_configinfo *info);

struct afsconf_dir;
extern int afstest_AddDESKeyFile(struct afsconf_dir *dir);

/* exec.c */

struct afstest_cmdinfo {
    char *command;	/**< command to run (as given to e.g. system()) */
    int exit_code;	/**< expected exit code */
    const char *output; /**< expected command output */
    int fd;		/**< fd to read output from */

    /* The following fields are not to be set by the caller; they are set when
     * running the given command. */
    char *fd_descr;	/**< string description of 'fd' (e.g. "stdout") */
    pid_t child;	/**< child pid (after command has started) */
    FILE *pipe_fh;	/**< pipe from child (after started) */
};

extern void afstest_command_start(struct afstest_cmdinfo *cmdinfo);
extern int afstest_command_end(struct afstest_cmdinfo *cmdinfo,
			       const char *format, ...)
	AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3);
extern int is_command(struct afstest_cmdinfo *cmdinfo,
		      const char *format, ...)
	AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3);
extern int afstest_systemlp(char *command, ...);

/* files.c */

extern char *afstest_mkdtemp(void);
extern void afstest_rmdtemp(char *path);
extern char *afstest_src_path(char *path);
extern char *afstest_obj_path(char *path);
extern int afstest_file_contains(char *path, char *target);
extern int afstest_cp(char *src_path, char *dest_path);
extern int afstest_file_equal(char *path_a, char *path_b, off_t start_off);

/* okv.c */
extern int afstest_okv_dbpath(char **a_path, char *filename);

/* rxkad.c */

extern struct rx_securityClass
	*afstest_FakeRxkadClass(struct afsconf_dir *dir,
				char *name, char *instance, char *realm,
				afs_uint32 startTime, afs_uint32 endTime);
/* servers.c */

/*
 * Specifies an ubik server type that some ubiktest tests may be for (e.g.
 * vlserver, ptserver).
 */
struct afstest_server_type {
    char *logname;  /**< Log filename on disk (VLLog) */
    char *ctl_sock; /**< ctl socket filename (e.g. vl.ctl.sock) */
    char *bin_path; /**< Path to server binary, relative to top of objdir */
    char *db_name;  /**< db filename on disk (e.g. vldb) */
    char *exec_name;	    /**< argv[0] when running server (e.g. vlserver) */
    char *startup_string;   /**< string to look for in log file to indicate
			     *   that the server has successfully started */
    char *service_name;	    /**< service name for the listening port
			     *   (AFSCONF_*SERVICE) */
    afs_uint16 port;	    /**< port number for the service */
};
extern struct afstest_server_type afstest_server_vl;
extern struct afstest_server_type afstest_server_pt;

struct rx_call;

/* Options for afstest_StartServerOpts */
struct afstest_server_opts {
    struct afstest_server_type *server;
			/**< which server to start */
    char *dirname;	/**< the config dir to use */
    pid_t *serverPid;	/**< set to the server's pid, after it's successfuly
			 *   been started. */
    afs_uint32 addr;	/**< IP to contact the server */
    int multi_server;	/**< if set, we're starting up multiple servers (so
			 *   ubik startup takes much longer). */
    int rxbind;		/**< if set, pass -rxbind to server. */
    char **extra_argv;	/**< if not NULL, extra arguments passed to the server
			 *   process */
    int nowait;		/**< if set, do not wait for server to finish startup
			 *   (call again with this unset to wait for started
			 *   server). */
};
extern int afstest_StartServerOpts(struct afstest_server_opts *opts);
extern int afstest_StartServer(struct afstest_server_type *server,
			       char *dirname, pid_t *serverPid);
extern int afstest_StopServer(pid_t serverPid);
extern int afstest_StartTestRPCService(const char *, char *, u_short, u_short,
				       afs_int32 (*proc)(struct rx_call *));
extern void afstest_ForkRxProc(int (*proc)(void *rock), void *rock);

/* ubik.c */
struct ubik_client;
extern int afstest_GetUbikClient(struct afsconf_dir *dir, char *service,
				 int serviceId,
				 struct rx_securityClass *secClass,
				 int secIndex,
				 struct ubik_client **ubikClient);

/*
 * Defines an existing database file for a dataset. (e.g. a .DB0 file in vldb4
 * format)
 */
struct ubiktest_dbdef {
    char *name;		/**< Name to refer to this db in 'use_db'. */
    char *flat_path;	/**< Path to a .DB0 file, relative to the top of the
			 *   source tree. */
    char *kv_path;	/**< Path to a KV ubik dbase. */
    char *getfile_path; /**< Path to file containing a DISK_GetFile blob. */
    char *getfile2_path;/**< Path to file containing a DISK_GetFile2 blob. */
};

/*
 * Info that ubiktest passes back to various callback functions given by
 * callers. (e.g. pre_start)
 */
struct ubiktest_cbinfo {
    char *confdir;  /**< The config dir used by the server process. */
    char *db_path;  /**< abs path to the .DB0 file used by the server process. */
    struct rx_connection *disk_conn;	/**< Conn to the DISK_ service for the
					 *   running server. */
    struct ubiktest_dbdef *src_dbdef;	/**< dbdef struct for the installed db. */
    char *src_dbpath;	/**< path to the source db we installed (if any) */
    char *ctl_sock; /**< Path to the ctl socket for the running server. */
};

/*
 * Defines a scenario to setup a server process to test against a sample
 * dataset. (e.g. "use this existing db", or "create the db from scratch")
 */
struct ubiktest_ops {
    char *descr;
    char *skip_reason;
		    /**< If set, skip all tests for this scenario. */
    char *use_db;   /**< Find the dbase in 'existing_dbs' with this name, and
		     *   install that db before starting up the server process.
		     *   If "none", then don't install a db for this test. If
		     *   NULL and 'create_db' is set, we treat this as "none". */

    int create_db;  /**< If nonzero, run the dataset's 'create_func' function
		     *   to create the sample dataset via RPCs and commands,
		     *   etc. */

    int result_kv;  /**< If set, the ubik db should be a KV database after all
		     *   tests and callbacks etc have run. If not set, it
		     *   should be a flat-file db. */

    char **server_argv;	/**< If set, pass these extra arguments to the server
			 *   process. */

    int n_tests;    /**< How many TAP tests are generated by the given
		     *   pre_start etc callbacks. (This is needed for skipping
		     *   the correct number of tests if we need to skip this
		     *   entire scenario.) Can be 0 if we never skip the
		     *   scenario. */

    void (*pre_start)(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops);
		    /**< If set, run this right before starting the server
		     *   process. */

    void (*post_start)(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops);
		    /**< If set, run this after the server process has started
		     *   (and the db has been setup, etc), but before running any
		     *   dataset tests. */

    void (*post_stop)(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops);
		    /**< If set, run this after stopping the server process
		     *   after the dataset tests have run. */
    void *rock;

    struct ubiktest_dbtest *override_dbtests;
		    /**< If set, run this list of tests instead of the normal
		     *   dataset tests. */

    struct ubiktest_dbtest *extra_dbtests;
		    /**< If set, run this list of tests in addition to the
		     *   normal dataset tests. */

    int n_servers;  /**< How many dbserver sites to run in the test cell. */
};

/*
 * Defines a single test against a sample dataset. (e.g. "Run vos listvldb
 * root.cell")
 */
struct ubiktest_dbtest {
    char *descr;
    void (*func)(char *dirname);    /**< If non-NULL, run this function instead
				      *  of running a command. */

    char *cmd_args;	/**< Run a command with these args (command is run by
			 *   calling 'dbtest_func'). */
    int cmd_exitcode;	/**< Command should exit with this exit code. */
    int cmd_auth;	/**< If non-zero, run command with -localauh. */
    int cmd_sync;	/**< If non-zero, run command against the sync site
			 *   only. */
    char *cmd_stdout;   /**< Command's stdout should match this. */
    char *cmd_stderr;	/**< Command's stderr should match this. */
};

/*
 * Defines a dataset for testing an ubik application. For example, the
 * 'vlsmall' dataset is a VLDB that contains specific definitions for volumes
 * root.cell et al, and specific server definitions. The dataset may exist in
 * different database formats in the tree, or we can create the dataset from
 * scratch (via 'create_func'). So the dataset is not a specific pre-existing
 * database, but represents the set of data (volumes, or users, etc) that
 * should be in that database.
 */
struct ubiktest_dataset {
    char *descr;
    struct afstest_server_type *server_type;
				    /**< Which application server this is for
				     *   (vlserver, ptserver, etc) */

    struct ubik_client **uclientp;  /**< If non-NULL, this will be set to a
				     *   ubik_client struct for communicating
				     *   with the running server. */

    struct ubiktest_dbtest *tests;  /**< A list of tests to run to check that
				     *   the running server responds with the
				     *   correct data for the dataset. */

    int n_dbtests;  /**< How many TAP tests are generated by running the
		     *   'tests' for this dataset. (See ubiktest_ops.n_tests
		     *   for why this is needed.) */

    int n_dbtests_sync;
		    /**< How many of the 'n_dbtests' are only run against the
		     *   sync-site? (All other tests are run for all sites.) */

    void (*dbtest_func)(char *dirname, char *server,
			struct ubiktest_dbtest *test);
				    /**< The function to call to run the
				     *   commands specified in 'tests'. */

    void (*create_func)(char *dirname);
				    /**< Run this function to create the
				     *   dataset from scratch (that is, from a
				     *   blank db). */

    struct ubiktest_dbdef existing_dbs[2];
				    /**< Existing databases for the dataset. */
    struct ubiktest_dbdef _existing_dbs_nul;
};
extern void ubiktest_init(char *service, char **argv);
extern void ubiktest_runtest(struct ubiktest_dataset *ds,
			     struct ubiktest_ops *ops);
extern void ubiktest_runtest_list(struct ubiktest_dataset *ds,
				  struct ubiktest_ops *ops);
extern void urectest_runtests(struct ubiktest_dataset *ds, char *use_db);
extern int ubiktest_db_equal(char *path_a, char *path_b);

struct frztest_ops {
    char *suite;    /**< e.g. 'vl' for the 'openafs-ctl vldb-dump' command */
    char *freeze_envname;
		    /**< e.g. 'VL' for the 'OPENAFS_VL_FREEZE_ID' env var */
    char *use_db;   /**< which 'existing_dbs' dbase to use when running the
		     *   server process. */
    char **server_argv;
		    /**< args to pass to server process */

    struct ubiktest_dbdef blankdb;
			/**< db definition for a 'blank' db. That is, a db that
			 *   the relevant server would create on startup
			 *   without an existing db (not literally a db full of
			 *   zeroes). */

    struct ubiktest_dbdef baddb;
			/**< db definition for an invalid db, that the server
			 *   will not be able to use at all (specifically,
			 *   'dbcheck_func' in struct ubik_serverinit_opts
			 *   needs to return an error for it). */

    /* When 'blankdb' is installed on the server, running 'blank_cmd' should
     * result in printing 'blank_cmd_stdout' to stdout. */
    char *blank_cmd;
    char *blank_cmd_stdout;
};
extern void frztest_runtests(struct ubiktest_dataset *ds,
			     struct frztest_ops *ops);
extern void frztest_runtests_convert(struct ubiktest_dataset *ds,
				     struct frztest_ops *ops_a,
				     struct frztest_ops *ops_b);

/* network.c */
extern int afstest_IsLoopbackNetworkDefault(void);
extern int afstest_SkipTestsIfLoopbackNetIsDefault(void);
extern void afstest_SkipTestsIfBadHostname(void);
extern void afstest_SkipTestsIfServerRunning(char *name);
extern afs_uint32 afstest_MyHostAddr(void);

/* misc.c */
extern char *afstest_vasprintf(const char *fmt, va_list ap);
extern char *afstest_asprintf(const char *fmt, ...)
	AFS_ATTRIBUTE_FORMAT(__printf__, 1, 2);
extern int is_pointer(void *left, void *right, const char *fmt, ...)
	AFS_ATTRIBUTE_FORMAT(__printf__, 3, 4);

/* opaque.c */
extern int is_opaque(struct rx_opaque *left, struct rx_opaque *right,
		     const char *format, ...)
	AFS_ATTRIBUTE_FORMAT(__printf__, 3, 4);

static_inline void
afstest_SkipTestsIfNoCtl(void)
{
#ifndef AFS_CTL_ENV
    skip_all("Built without afsctl support");
#endif
}

#endif /* OPENAFS_TEST_COMMON_H */

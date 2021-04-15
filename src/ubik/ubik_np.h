/*
 * Copyright (c) 2021 Sine Nomine Associates. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef OPENAFS_UBIK_UBIK_NP_H
#define OPENAFS_UBIK_UBIK_NP_H

/*
 * ubik_np.h - Header for ubik-related items that are not stable public
 * interfaces exported to outside the OpenAFS tree, but are public to other
 * subsystems in the OpenAFS tree.
 */

#include <ubik.h>

/*** ubik.c ***/

struct ubik_serverinit_opts {
    /* IP addr of this host. */
    afs_uint32 myHost;

    /* Port to bind to. */
    short myPort;

    /* Cell info (provide this or 'serverList'). */
    struct afsconf_cell *info;

    /* Which servers in 'info' are clones. (If clones[i] is nonzero, then
     * info->hostAddr[i] is a clone.) */
    char *clones;

    /* Set of ubik server IP addrs; terminated by a 0. Provide this instead of
     * 'info'. Does not include 'myHost'. */
    afs_uint32 *serverList;

    /* Initial prefix used for naming storage files used by this system. */
    const char *pathName;

    /* If non-NULL, use this config dir for locating some files (such as
     * NetInfo). */
    char *configDir;

    /* ctl server instance to register our ubik ctl ops (optional) */
    struct afsctl_server *ctl_server;
};

int ubik_ServerInitByOpts(struct ubik_serverinit_opts *opts,
			  struct ubik_dbase **dbase);

int ubik_CopyDB(char *src_path, char *dest_path);

struct ubik_rawinit_opts {
    int r_create;
    int r_rw;
};

int ubik_RawDbase(struct ubik_dbase *dbase);
int ubik_RawTrans(struct ubik_trans *trans);
int ubik_RawHandle(struct ubik_trans *trans, FILE **a_fh);

int ubik_RawInit(char *path, struct ubik_rawinit_opts *ropts,
		 struct ubik_dbase **dbase);
void ubik_RawClose(struct ubik_dbase **a_dbase);
int ubik_RawGetHeader(struct ubik_trans *trans, struct ubik_hdr *hdr);
int ubik_RawGetVersion(struct ubik_trans *trans, struct ubik_version *version);
int ubik_RawSetVersion(struct ubik_trans *trans, struct ubik_version *version);

typedef void (*ubik_writehook_func)(struct ubik_dbase *tdb, afs_int32 fno,
				    void *bp, afs_int32 pos, afs_int32 count);
int ubik_InstallWriteHook(ubik_writehook_func func);

/* freeze_client.c */

struct ubik_freeze_client;
struct ubik_freezeinit_opts {
    struct afsctl_clientinfo *fi_cinfo;

    int fi_forcenest;
    int fi_nonest;
    int fi_needsync;
    int fi_needrw;

    afs_uint32 fi_timeout_ms;
};

int ubik_FreezeInit(struct ubik_freezeinit_opts *opts,
		    struct ubik_freeze_client **a_freeze);
int ubik_FreezeIsNested(struct ubik_freeze_client *freeze,
			afs_uint64 *a_freezeid);
int ubik_FreezeSetEnv(struct ubik_freeze_client *freeze);
void ubik_FreezePrintEnv(struct ubik_freeze_client *freeze, FILE *fh);
void ubik_FreezeDestroy(struct ubik_freeze_client **a_freeze);
int ubik_FreezeBegin(struct ubik_freeze_client *freeze, afs_uint64 *a_freezeid,
		     struct ubik_version64 *a_version, char **a_dbpath);
int ubik_FreezeAbort(struct ubik_freeze_client *freeze, char *message);
int ubik_FreezeEnd(struct ubik_freeze_client *freeze, char *message);
int ubik_FreezeAbortId(struct ubik_freeze_client *freeze, afs_uint64 freezeid,
		       char *message);
int ubik_FreezeAbortForce(struct ubik_freeze_client *freeze, char *message);
int ubik_FreezeInstall(struct ubik_freeze_client *freeze, char *path,
		       char *backup_suffix);
int ubik_FreezeDistribute(struct ubik_freeze_client *freeze);

#endif /* OPENAFS_UBIK_UBIK_NP_H */

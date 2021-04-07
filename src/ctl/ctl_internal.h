/*
 * Copyright (c) 2021 Sine Nomine Associates. All rights reserved.
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

#ifndef OPENAFS_CTL_INTERNAL_H
#define OPENAFS_CTL_INTERNAL_H

#include <roken.h>

#include <afs/afsctl.h>
#include <afs/afsutil.h>
#include <afs/opr.h>
#include <opr/lock.h>

#include <sys/socket.h>
#include <sys/un.h>

/*
 * AFSCTL_PROTO_VERSION is an arbitrary version string that must match between
 * the afsctl server and client. Any time the afsctl protocol significantly
 * changes, this constant should change, to prevent a mismatched client and
 * server from talking to each other (and possibily misinterpreting data, or
 * failing for some operations, etc). The string can be anything, but currently
 * the convention is based on the openafs commit around when the protocol was
 * last changed.
 *
 * If changing this is too heavy of a hammer, it's also possible to just change
 * the name of an individual rpc, or change the names of various fields, etc.
 * But it may be safer to just change this constant for bigger changes, to
 * avoid possible issues with client/server version mismatches.
 */
#define AFSCTL_PROTO_VERSION "openafs 1.9.1-145-If420701fe"

struct afsctl_call {
    FILE *sock_fh;

    /* If this is non-zero, the call has been aborted with this error code. We
     * can no longer send or receive anything on the call. */
    int error;

    /* server-only items */
    struct afsctl_server *server;
    char *descr;
};

/*** ctl_common.c ***/

int ctl_socket(const char *server_type, const char *path, int *a_sock,
	       struct sockaddr_un *addr);
int ctl_call_create(int *a_sock, struct afsctl_call **a_ctl);
void ctl_send_abort(struct afsctl_call *ctl, int code);

#endif /* OPENAFS_CTL_INTERNAL_H */

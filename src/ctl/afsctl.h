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

#ifndef OPENAFS_AFSCTL_H
#define OPENAFS_AFSCTL_H

#include <afsconfig.h>
#include <afs/param.h>

#ifdef AFS_CTL_ENV
# include <jansson.h>
# if JANSSON_VERSION_HEX < 0x020800
#  error Jansson v2.8+ is required
# endif
#else
typedef struct json_stub {
    int stub;
} json_t;
#endif

/*** common.c ***/

struct afsctl_call;

int afsctl_socket_path(const char *server_type, char **a_path);
int afsctl_send_pack(struct afsctl_call *ctl, const char *fmt, ...);
int afsctl_recv_unpack(struct afsctl_call *ctl, json_t **a_jobj,
		       const char *fmt, ...);
int afsctl_wait_recv(struct afsctl_call *ctl, afs_uint32 timeout_ms);
int afsctl_call_shutdown_read(struct afsctl_call *ctl);
void afsctl_call_destroy(struct afsctl_call **a_ctl);

/*** server.c ***/

struct afsctl_server;

struct afsctl_serverinfo {
    char *server_type;
    char *sock_path;
};

typedef int (*afsctl_reqfunc)(struct afsctl_call *ctl, json_t *in_args,
			      json_t **out_args);
struct afsctl_server_method {
    char *name;
    afsctl_reqfunc func;
};

int afsctl_server_create(struct afsctl_serverinfo *sinfo,
			 struct afsctl_server **a_srv);
int afsctl_server_reg(struct afsctl_server *srv,
		      struct afsctl_server_method *methlist);
int afsctl_server_listen(struct afsctl_server *srv);
const char * afsctl_call_describe(struct afsctl_call *ctl);

/*** client.c ***/

struct afsctl_clientinfo {
    char *server_type;
    char *sock_path;
    char *reason;
};

int afsctl_client_start(struct afsctl_clientinfo *cinfo, const char *method,
			json_t *in_args, struct afsctl_call **a_ctl);
int afsctl_client_end(struct afsctl_call *ctl, json_t **out_args);
int afsctl_client_call(struct afsctl_clientinfo *cinfo, const char *method,
		       json_t *in_args, json_t **out_args);

#endif /* OPENAFS_AFSCTL_H */

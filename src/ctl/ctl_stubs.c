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

/*
 * stubs for liboafs_ctl, for when we don't have libjansson available. All
 * routines should just return an error. This exists to make the build easier,
 * so things can unconditionally link against liboafs_ctl.
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>

#include "afsctl.h"

#ifdef AFS_CTL_ENV
# error ctl_stubs.c should not be built when jansson is available
#endif

static int stub_err = EAFNOSUPPORT;

int
afsctl_send_pack(struct afsctl_call *ctl, const char *fmt, ...)
{
    return stub_err;
}

int
afsctl_recv_unpack(struct afsctl_call *ctl, json_t **a_jobj, const char *fmt, ...)
{
    return stub_err;
}

int
afsctl_wait_recv(struct afsctl_call *ctl, afs_uint32 a_ms)
{
    return stub_err;
}

int
afsctl_client_start(struct afsctl_clientinfo *cinfo, const char *method,
		    json_t *in_args, struct afsctl_call **a_ctl)
{
    return stub_err;
}

int
afsctl_client_end(struct afsctl_call *ctl, json_t **out_args)
{
    return stub_err;
}

int
afsctl_client_call(struct afsctl_clientinfo *cinfo, const char *method,
		   json_t *in_args, json_t **out_args)
{
    return stub_err;
}

const char *
afsctl_call_describe(struct afsctl_call *ctl)
{
    return NULL;
}

int
afsctl_server_create(struct afsctl_serverinfo *sinfo,
		     struct afsctl_server **a_srv)
{
    return stub_err;
}

int
afsctl_server_reg(struct afsctl_server *srv,
		  struct afsctl_server_method *methlist)
{
    return stub_err;
}

int
afsctl_server_listen(struct afsctl_server *srv)
{
    return stub_err;
}

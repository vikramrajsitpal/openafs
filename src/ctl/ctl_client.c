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

#include <afsconfig.h>
#include <afs/param.h>

#include "ctl_internal.h"

static int
client_hello(struct afsctl_call *ctl, const char *server_type,
	     const char *reason, const char *method, json_t *in_args)
{
    int code;
    json_t *jobj = NULL;
    char *r_server = NULL;
    const char *comm = getprogname();

    if (comm == NULL) {
	comm = "<unknown>";
    }

    if (server_type == NULL) {
	code = EINVAL;
	goto done;
    }

    code = afsctl_recv_unpack(ctl, &jobj, "{s:s}", "server", &r_server);
    if (code != 0) {
	goto done;
    }

    if (strcmp(r_server, server_type) != 0) {
	ViceLog(0, ("ctl: Server's advertised type (%s) does not match given type (%s)\n",
		r_server, server_type));
	code = EPROTOTYPE;
	goto done;
    }

    code = afsctl_send_pack(ctl, "{s:s, s:{s:i, s:s, s:s?}, s:s, s:O}",
			    "server", server_type,
			    "client",
				"pid", (int)getpid(),
				"comm", comm,
				"reason", reason,
			    "method", method,
			    "params", in_args);
    if (code != 0) {
	goto done;
    }

 done:
    json_decref(jobj);
    return code;
}

/**
 * Start a call to an afsctl server.
 *
 * Note that afsctl_client_start() ALWAYS decs a reference on 'in_args', even
 * if the function returns an error. This may seem odd, but allows for
 * convenient use of json_pack() for input arguments, like so:
 *
 *    code = afsctl_client_start(&cinfo, "method",
 *				 json_pack("{xxx}", xxx),
 *				 &out_args);
 *
 * @param[in] cinfo Connection info.
 * @param[in] method	Function/method name to call.
 * @param[in] in_args	Input arguments for the remote call. This function
 *			steals the reference to 'in_args' (that is, we always
 *			dec a ref on it); the caller can json_incref() it if
 *			the caller wants to keep a ref. This must not be NULL;
 *			if there are no input arguments to the method, provide
 *			a JSON object like json_null().
 * @param[out] a_ctl	On success, set to the call context for the call.
 *
 * @return errno error codes
 */
int
afsctl_client_start(struct afsctl_clientinfo *cinfo, const char *method,
		    json_t *in_args, struct afsctl_call **a_ctl)
{
    int code;
    struct sockaddr_un addr;
    struct afsctl_call *ctl = NULL;
    int sock = -1;

    memset(&addr, 0, sizeof(addr));

    if (in_args == NULL) {
	/*
	 * Assume an allocation/json_pack call failed. If someone gave us a
	 * literal NULL, this error can be confusing, but they shouldn't be
	 * calling us with NULL.
	 */
	code = ENOMEM;
	goto done;
    }

    code = ctl_socket(cinfo->server_type, cinfo->sock_path, &sock, &addr);
    if (code != 0) {
	goto done;
    }

    code = ctl_call_create(&sock, &ctl);
    if (code != 0) {
	goto done;
    }

    code = connect(fileno(ctl->sock_fh), &addr, sizeof(addr));
    if (code != 0) {
	code = errno;
	goto done;
    }

    code = client_hello(ctl, cinfo->server_type, cinfo->reason, method,
			in_args);
    if (code != 0) {
	goto done;
    }

    *a_ctl = ctl;
    ctl = NULL;

 done:
    afsctl_call_destroy(&ctl);
    if (sock >= 0) {
	close(sock);
    }
    json_decref(in_args);
    return code;
}

/**
 * End a client call.
 *
 * Someday, we may want an afsctl_client_end_unpack() variant, like
 * afsctl_recv_unpack(), for convenient unpacking of output arguments. But we
 * don't currently do a lot of unconditional out_args unpacking, so don't
 * bother with that for now.
 *
 * @param[in] ctl   The afsctl call.
 * @param[out] out_args	On success, set to the output arguments returned by the
 *			server. If NULL, the output arguments are ignored.
 *
 * @return errno error codes
 */
int
afsctl_client_end(struct afsctl_call *ctl, json_t **out_args)
{
    int code;
    json_t *jobj = NULL;

    /* If the server is waiting for more data from us, make sure it knows it's
     * not going to get any. */
    code = shutdown(fileno(ctl->sock_fh), SHUT_WR);
    if (code != 0) {
	return errno;
    }

    code = afsctl_recv_unpack(ctl, NULL, "{s:O}", "result", &jobj);
    if (code != 0) {
	goto done;
    }

    if (out_args != NULL) {
	*out_args = jobj;
	jobj = NULL;
    }

 done:
    json_decref(jobj);
    return code;
}

/**
 * Make a simple afsctl call.
 *
 * This is a simple convenience wrapper around afsctl_client_start,
 * afsctl_client_end, and afsctl_call_destroy. Note that 'in_args' always has a
 * reference dec'd, just like with afs_client_start().
 *
 * @param[in] cinfo	See afs_client_start().
 * @param[in] method	See afs_client_start().
 * @param[in] in_args	See afs_client_start().
 * @param[out] out_args	See afs_client_end().
 *
 * @return errno error codes
 */
int
afsctl_client_call(struct afsctl_clientinfo *cinfo, const char *method,
		   json_t *in_args, json_t **out_args)
{
    int code;
    struct afsctl_call *ctl = NULL;

    code = afsctl_client_start(cinfo, method, in_args, &ctl);
    if (code != 0) {
	goto done;
    }

    code = afsctl_client_end(ctl, out_args);
    if (code != 0) {
	goto done;
    }

 done:
    afsctl_call_destroy(&ctl);
    return code;
}

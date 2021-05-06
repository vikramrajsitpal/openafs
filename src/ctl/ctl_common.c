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
 * afsctl - A mechanism for issuing RPCs between processes on the same host
 * over a unix domain socket, primarily for the purpose of interacting with or
 * controlling daemons (hence the 'ctl', for 'control'). A general overview of
 * how this works:
 *
 * All data sent over afsctl is encoded in packets of JSON objects for ease of
 * use / rapid dev. Our requests look somewhat similar to JSON-RPC, but
 * modified in a few ways:
 *
 * - There are no request IDs, and no "batch" requests. Each connection just
 *   gets one request.
 *
 * - We have no "jsonrpc" version fields, but instead have an "afsctl" version
 *   field (which should always be AFSCTL_PROTO_VERSION).
 *
 * - There are some extra fields in client requests, to record some info about
 *   the calling process (the "client" field).
 *
 * - Before the client sends a request, the server sends a hello-style message
 *   to just show the afsctl version and server type.
 *
 * - We don't use the "message" or "data" fields in the error object. Detailed
 *   error info will presumably be logged in the server's log, so we shouldn't
 *   need detailed error reporting in the actual prototocol.
 *
 * - JSON-RPC reserves some method names and error code ranges; we ignore
 *   those.
 *
 * - Additional data can be sent during a request, besides the "request" and
 *   "response". That is, a client or server can send additional or partial
 *   data to the peer without ending the call. But, any such data is still sent
 *   as a single JSON object.
 *
 * Here's an example of a simple method call ("S" is server->client, "C" is
 * client->server):
 *
 * S: {"afsctl": "openafs 1.9.1-145-If420701f",
 *     "server": "vlserver"}
 *
 * C: {"afsctl": "openafs 1.9.1-145-If420701f",
 *     "server": "vlserver",
 *     "client": {"pid": 1234,
 *                "comm": "openafs-ctl",
 *                "reason": "testing openafs-ctl"},
 *     "method": "ubik.db-info",
 *     "params": null}
 *
 * S: {"afsctl": "openafs 1.9.1-145-If420701f",
 *     "result": {"type": "flat",
 *                "engine": {"name": "udisk",
 *                           "desc": "traditional udisk/uphys storage"},
 *                "size": 66944,
 *                "version": {"epoch": 1616893943,
 *                            "counter": 2}}}
 *
 * If an error occurs, that last messsage could instead look like this:
 *
 * S: {"afsctl": "openafs 1.9.1-145-If420701f",
 *     "error": {"code": 5379}}
 *
 * Note that there is no auth or security-related info in the requests. We rely
 * on the OS restricting access to the underlying unix socket for access
 * control (typically root). Since this is a local-only protocol, there's no
 * need for any authn/authz.
 *
 * There are a lot of strings transmitted; we're not terribly concerned about
 * efficiency/performance. The relevant operations are intended to be
 * infrequent, like dumping a database for backup or upgrading a db to a new
 * format, that kind of thing. We don't expect multiple requests per second.
 *
 * We're also not too concerned with compatibility. While we do need to detect
 * protocol mismatches to bail out with an error, we don't need to actually
 * work across versions. We assume that the client and server binaries are from
 * the same OpenAFS version; the afsctl protocol is a private interface that
 * can change across releases.
 *
 * However, with that said, we don't intentionally break the protocol between
 * releases. It is expected that AFSCTL_PROTO_VERSION is changed whenever we do
 * something to change the afsctl protocol; but as long as nothing changes in
 * the protocol, binaries across versions will technically work.
 *
 * When the server accepts a new socket from a client, it spawns a new thread
 * to handle the request. The max number of reqests we can serve in parallel is
 * MAX_CALLS; if we're at that limit and another request comes in, we abort the
 * request and close the socket before spawning a thread.
 *
 * Requests can be long-lived (unlike SYNC, where that's not advised). That
 * means that clients can issue a request to be notified immediately when
 * something happens, without needing to poll the server. For example, we could
 * have a request where a client gets notified when the ubik sync-site status
 * changes; the request would stay idle for a long time until it does. The only
 * limiting factor is exceeding the MAX_CALLS limit, which can easily be
 * raised.
 *
 * If the client doesn't care about the server's response to a request, the
 * client can just close its socket prematurely without waiting for the
 * response. We may want to do this for some sort of async or best-effort
 * requests (e.g. "please start shutting down", "reopen log files when you
 * can").
 *
 * Note that we do not currently support shutting down an afsctl server after
 * it has started running. This could be implemented, of course (but
 * interrupting the accept() thread is nontrivial across platforms), but no
 * callers currently need it. Just let the process die, and the sockets will
 * close.
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <poll.h>

#include "ctl_internal.h"

#ifdef SOCK_CLOEXEC
static const int sock_cloexec = SOCK_CLOEXEC;
#else
static const int sock_cloexec;
#endif

static int
calc_sockpath(const char *server_type, const char **a_path)
{
    if (*a_path != NULL) {
	return 0;
    }

    if (strcmp(server_type, "vlserver") == 0) {
	*a_path = AFSDIR_SERVER_VLSERVER_CTLSOCK_FILEPATH;
	return 0;

    } else if (strcmp(server_type, "ptserver") == 0) {
	*a_path = AFSDIR_SERVER_PTSERVER_CTLSOCK_FILEPATH;
	return 0;

    } else {
	ViceLog(0, ("afsctl: Internal error: no path for unknown server "
		    "type '%s'\n", server_type));
	return ENOPROTOOPT;
    }
}

/* Create a unix socket and the sockaddr to use for it. */
int
ctl_socket(const char *server_type, const char *path, int *a_sock,
	   struct sockaddr_un *addr)
{
    int sock = -1;
    int code = 0;

    memset(addr, 0, sizeof(*addr));

    code = calc_sockpath(server_type, &path);
    if (code != 0) {
	goto done;
    }

    sock = socket(AF_UNIX, SOCK_STREAM | sock_cloexec, 0);
    if (sock < 0) {
	code = errno;
	goto done;
    }

    if (strlcpy(addr->sun_path, path,
		sizeof(addr->sun_path)) >= sizeof(addr->sun_path)) {
	code = ENAMETOOLONG;
	goto done;
    }
    addr->sun_family = AF_UNIX;

    *a_sock = sock;
    sock = -1;

 done:
    if (sock >= 0) {
	close(sock);
    }
    return code;
}

/* callback for json_dump_callback() */
static int
dump_to_ctl(const char *buffer, size_t size, void *rock)
{
    struct afsctl_call *ctl = rock;
    ssize_t nbytes;

    do {
	nbytes = send(fileno(ctl->sock_fh), buffer, size, MSG_NOSIGNAL);
	if (nbytes <= 0) {
	    return -1;
	}

	size -= nbytes;
	buffer += nbytes;
    } while (size > 0);

    return 0;
}

/* Send the given json object to our peer. */
static int
send_obj(struct afsctl_call *ctl, json_t *jobj)
{
    int code;

    if (ctl->error != 0) {
	return ctl->error;
    }

    /* Set the 'afsctl' version field in every object we send. */
    code = json_object_set_new(jobj, "afsctl",
			       json_string(AFSCTL_PROTO_VERSION));
    if (code != 0) {
	ViceLog(0, ("ctl: json_object_set_new failed\n"));
	return EIO;
    }

    code = json_dump_callback(jobj, dump_to_ctl, ctl, 0);
    if (code != 0) {
	/* We probably wrote a partial json object to the socket; make sure we
	 * don't try to send anything more. */
	ctl->error = EIO;
	return EIO;
    }

    return 0;
}

/**
 * Send a json object to the peer of an afsctl call. The contents of the object
 * are constructed according to the given format string and arguments (see
 * json_pack()).
 *
 * @param[in] ctl   The afsctl call.
 * @param[in] fmt   Format string and values to give to json_pack().
 *
 * @return errno error codes
 */
int
afsctl_send_pack(struct afsctl_call *ctl, const char *fmt, ...)
{
    va_list ap;
    json_t *jobj = NULL;
    json_error_t jerror;
    int code;

    if (ctl->error != 0) {
	return ctl->error;
    }

    va_start(ap, fmt);
    jobj = json_vpack_ex(&jerror, 0, fmt, ap);
    va_end(ap);

    if (jobj == NULL) {
	ViceLog(0, ("ctl: json_vpack_ex failed (%s): %s\n", fmt, jerror.text));
	return EIO;
    }

    code = send_obj(ctl, jobj);

    json_decref(jobj);

    return code;
}

/*
 * Abort the call with the given error. This sends the error code to the peer,
 * and sets the the error for the call (preventing any further sends or
 * receives).
 */
void
ctl_send_abort(struct afsctl_call *ctl, int code)
{
    (void)afsctl_send_pack(ctl, "{s:{s:i}}", "error", "code", code);
    if (ctl->error == 0) {
	ctl->error = code;
    }
}

/* Receive a json object from the peer. */
static int
recv_obj(struct afsctl_call *ctl, json_t **a_jobj)
{
    int code;
    char *verstr;
    json_t *jobj;
    json_t *err_obj = NULL;
    json_error_t jerror;
    size_t jflags = JSON_DISABLE_EOF_CHECK;

    *a_jobj = NULL;

    if (ctl->error) {
	return ctl->error;
    }

    jobj = json_loadf(ctl->sock_fh, jflags, &jerror);
    if (jobj == NULL) {
	if (feof(ctl->sock_fh) == 0 && ferror(ctl->sock_fh) == 0) {
	    ViceLog(0, ("ctl: json_loadf failed: %s\n", jerror.text));
	}
	code = EIO;
	goto done;
    }

    /* Check the afsctl version of the recv'd object, and whether the recv'd
     * object is an abort. */

    code = json_unpack_ex(jobj, &jerror, 0,
			  "{s:s, s?:o}",
			  "afsctl", &verstr,
			  "error", &err_obj);
    if (code != 0) {
	ViceLog(0, ("ctl: json_unpack_ex(recv) failed: %s\n", jerror.text));
	code = EPROTO;
	goto done;
    }

    if (strcmp(verstr, AFSCTL_PROTO_VERSION) != 0) {
	ViceLog(0, ("ctl: protocol version mismatch: '%s' != '%s'\n",
		verstr, AFSCTL_PROTO_VERSION));
	code = EPROTO;
	goto done;
    }

    /* Check if the recv'd object is an abort. */

    if (err_obj != NULL) {
	int err_code;

	code = json_unpack_ex(err_obj, &jerror, 0, "{s:i}", "code", &err_code);
	if (code != 0) {
	    ViceLog(0, ("ctl: json_unpack_ex(error) failed: %s\n", jerror.text));
	    code = EPROTO;
	    goto done;
	}

	if (err_code == 0) {
	    ViceLog(0, ("ctl: warning: error object contained error code 0\n"));
	    err_code = EPROTO;
	}
	code = err_code;
	goto done;
    }

    *a_jobj = jobj;
    jobj = NULL;

    code = 0;

 done:
    json_decref(jobj);
    return code;
}

/**
 * Receive a json object from the peer of an afsctl call. The contents of the
 * object are unpacked according to the given format string and arguments (see
 * json_unpack()).
 *
 * @param[in] ctl   The afsctl call.
 * @param[out] a_jobj	Optional. If not NULL, this will be set to the
 *			underlying json object received on success. A caller
 *			needs this if format specifiers like 's' or 'o' are
 *			given, since those values are only valid for as long as
 *			the underlying json object. Caller must json_decref()
 *			(after the unpacked values have been processed).
 * @param[in] fmt   Format string and values to given to json_unpack().
 *
 * @return errno error codes
 */
int
afsctl_recv_unpack(struct afsctl_call *ctl, json_t **a_jobj, const char *fmt, ...)
{
    va_list ap;
    json_t *jobj = NULL;
    json_error_t jerror;
    int code;

    code = recv_obj(ctl, &jobj);
    if (code != 0) {
	goto done;
    }

    va_start(ap, fmt);
    code = json_vunpack_ex(jobj, &jerror, 0, fmt, ap);
    va_end(ap);
    if (code != 0) {
	ViceLog(0, ("ctl: json_vunpack_ex failed: %s\n", jerror.text));
	code = EPROTO;
	goto done;
    }

    if (a_jobj != NULL) {
	*a_jobj = jobj;
	jobj = NULL;
    }

 done:
    json_decref(jobj);
    return code;
}

/**
 * Wait for peer to send data on an afsctl call.
 *
 * @param[in] ctl   The ctl call.
 * @param[in] a_ms  How many milliseconds to wait (or 0 to wait forever). If
 *		    this much time has passed and the peer hasn't sent any data
 *		    (or shutdown the socket), return ETIMEDOUT.
 * @return errno error codes
 * @retval ETIMEDOUT we timed out waiting for the peer to send something
 */
int
afsctl_wait_recv(struct afsctl_call *ctl, afs_uint32 a_ms)
{
    struct pollfd fds;
    int timeout_ms;
    int code;

    if (a_ms == 0) {
	/* Wait forever */
	timeout_ms = -1;
    } else {
	timeout_ms = a_ms;
    }

    memset(&fds, 0, sizeof(fds));
    fds.fd = fileno(ctl->sock_fh);
    fds.events = POLLIN;

    code = poll(&fds, 1, timeout_ms);
    if (code < 0) {
	code = errno;
	/* Signals should be handled by another thread via softsig; make sure
	 * we don't accidentally report an EINTR as an error. */
	opr_Assert(code != EINTR);
	return code;
    }
    if (code == 0) {
	return ETIMEDOUT;
    }
    return 0;
}

/**
 * Create an afsctl call from a connected socket.
 *
 * @param[inout] a_sock	The connected socket to use. This is set to -1 once it
 *			has been associated with the afsctl call, and the
 *			caller must no longer close the socket.
 * @param[out] a_ctl	On success, set to the new afsctl call.
 * @return errno error codes
 */
int
ctl_call_create(int *a_sock, struct afsctl_call **a_ctl)
{
    struct afsctl_call *ctl;
    int code;

    *a_ctl = NULL;

    ctl = calloc(1, sizeof(*ctl));
    if (ctl == NULL) {
	code = errno;
	goto done;
    }

    ctl->sock_fh = fdopen(*a_sock, "r+");
    if (ctl->sock_fh == NULL) {
	code = errno;
	goto done;
    }

    *a_sock = -1;

    *a_ctl = ctl;
    ctl = NULL;

    code = 0;

 done:
    afsctl_call_destroy(&ctl);
    return code;
}

/*
 * Close the read-side of the underlying socket of an afsctl call, waking up
 * any thread that may be waiting on afsctl_recv_unpack() for the call.
 *
 * @return error error codes
 */
int
afsctl_call_shutdown_read(struct afsctl_call *ctl)
{
    int code;
    code = shutdown(fileno(ctl->sock_fh), SHUT_RD);
    if (code != 0) {
	return errno;
    }
    return 0;
}

/* Free the given afsctl call, and close its underlying socket. */
void
afsctl_call_destroy(struct afsctl_call **a_ctl)
{
    struct afsctl_call *ctl = *a_ctl;
    if (ctl == NULL) {
	return;
    }
    *a_ctl = NULL;

    /* If this is a server call, server_call_destroy() must have already
     * released its ref on the server instance. */
    opr_Assert(ctl->server == NULL);

    free(ctl->descr);
    ctl->descr = NULL;

    if (ctl->sock_fh != NULL) {
	fclose(ctl->sock_fh);
	ctl->sock_fh = NULL;
    }

    free(ctl);
}

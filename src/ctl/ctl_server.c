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

/*
 * Let up to MAX_CALLS calls run in parallel before we start refusing new
 * calls. This is pretty low for now just because we don't expect a lot of
 * calls over afsctl. Raise as needed; increasing this should not have much
 * negative impact.
 */
#define MAX_CALLS 8

#define ACCEPT_BACKLOG 10

/* Context for an afsctl server. */
struct afsctl_server {
    /* The socket we accept() new calls on. */
    int accept_sock;

    /* Is our accept() thread running? */
    int thread_running;

    /* The server type for this server. */
    char *server_type;

    /* Registered methods. (Each key is a method name, value is a pointer to
     * the struct afsctl_server_method wrapped in a JSON_INTEGER.) */
    json_t *methods;

    /* How many calls are running. Protected by 'lock' */
    int n_calls;
    opr_mutex_t lock;
};

typedef void *(thread_func)(void *);
static void
spawn_thread(thread_func func, void *rock)
{
    pthread_attr_t attr;
    pthread_t tid;

    opr_Verify(pthread_attr_init(&attr) == 0);
    opr_Verify(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
    opr_Verify(pthread_create(&tid, &attr, func, rock) == 0);
}

/*
 * Destroy a server-side ctl call. In this file, don't call afsctl_call_destroy
 * directly. Instead, call this, which will cleanup our server-side stuff, and
 * then call afsctl_call_destroy.
 */
static void
server_call_destroy(struct afsctl_call **a_ctl)
{
    struct afsctl_call *ctl = *a_ctl;
    if (ctl == NULL) {
	return;
    }
    *a_ctl = NULL;

    if (ctl->server != NULL) {
	struct afsctl_server *srv = ctl->server;

	opr_mutex_enter(&srv->lock);
	opr_Assert(srv->n_calls > 0);
	srv->n_calls--;
	opr_mutex_exit(&srv->lock);

	ctl->server = NULL;
    }

    afsctl_call_destroy(&ctl);
}

/* Get the struct afsctl_server_method associated with the given name. Return
 * NULL if it doesn't exist. */
static struct afsctl_server_method *
method_get(struct afsctl_server *srv, char *name)
{
    json_int_t meth_int;
    struct afsctl_server_method *meth = NULL;
    json_t *jobj;

    jobj = json_object_get(srv->methods, name);
    if (jobj == NULL) {
	return NULL;
    }

    meth_int = json_integer_value(jobj);
    opr_Assert(meth_int != 0);

    opr_StaticAssert(sizeof(meth_int) >= sizeof(meth));
    memcpy(&meth, &meth_int, sizeof(meth));

    return meth;
}

/* Register the given method on the afsctl server. If it already exists,
 * returns EEXIST. */
static int
method_reg(struct afsctl_server *srv, struct afsctl_server_method *meth)
{
    json_int_t meth_int = 0;
    int code;

    if (method_get(srv, meth->name) != NULL) {
	ViceLog(0, ("ctl: Tried to register method '%s', but it already exists.\n",
		meth->name));
	return EEXIST;
    }

    opr_StaticAssert(sizeof(meth_int) >= sizeof(meth));
    memcpy(&meth_int, &meth, sizeof(meth));

    code = json_object_set_new(srv->methods, meth->name,
			       json_integer(meth_int));
    if (code != 0) {
	ViceLog(0, ("ctl: json_object_set_new failed for method %s\n",
		meth->name));
	return EIO;
    }

    return 0;
}

/* The thread for handing a new client request. This gets spawned on every new
 * request that we handle. */
static void *
req_thread(void *rock)
{
    struct afsctl_call *ctl = rock;
    struct afsctl_server_method *meth = NULL;
    json_t *req_jobj = NULL;
    json_t *in_args = NULL;
    json_t *out_args = NULL;
    json_t *reason_obj = NULL;
    char *r_server = NULL;
    char *comm = NULL;
    char *meth_str = NULL;
    const char *reason = NULL;
    pid_t pid;
    int nbytes;
    int code;

    opr_threadname_set("afsctl request");

    /* Do the opening handshake for the call. Send out our server type, and get
     * the request info from the client. */

    code = afsctl_send_pack(ctl, "{s:s}", "server", ctl->server->server_type);
    if (code != 0) {
	goto done;
    }

    code = afsctl_recv_unpack(ctl, &req_jobj, "{s:s, s:{s:i, s:s, s?:o}, s:s, s:o}",
			      "server", &r_server,
			      "client",
				"pid", &pid,
				"comm", &comm,
				"reason", &reason_obj,
			      "method", &meth_str,
			      "params", &in_args);
    if (code != 0) {
	goto done;
    }

    if (strcmp(r_server, ctl->server->server_type) != 0) {
	/* Wrong "server" from client. */
	code = EPROTOTYPE;
	goto done;
    }

    if (reason_obj != NULL && !json_is_null(reason_obj)) {
	reason = json_string_value(reason_obj);
	if (reason == NULL) {
	    /* Non-string "reason" from client. */
	    code = EINVAL;
	    goto done;
	}
    }

    if (reason != NULL) {
	nbytes = asprintf(&ctl->descr, "pid %d, comm %s (%s)",
			  pid, comm, reason);
    } else {
	nbytes = asprintf(&ctl->descr, "pid %d, comm %s",
			  pid, comm);
    }
    if (nbytes < 0) {
	ctl->descr = NULL;
	code = ENOMEM;
	goto done;
    }

    meth = method_get(ctl->server, meth_str);
    if (meth == NULL) {
	code = ENOTSUP;
	goto done;
    }

    code = (*meth->func)(ctl, in_args, &out_args);
    if (code != 0) {
	goto done;
    }

    code = afsctl_send_pack(ctl, "{s:O?}", "result", out_args);
    if (code != 0) {
	goto done;
    }

    code = 0;

 done:
    if (code != 0) {
	ctl_send_abort(ctl, code);
    }
    json_decref(req_jobj);
    json_decref(out_args);
    server_call_destroy(&ctl);
    return NULL;
}

/* Handle a request on the given connected socket. Mostly this just causes
 * req_thread() to be called in a new thread. */
static int
handle_req(struct afsctl_server *srv, int sock)
{
    struct afsctl_call *ctl = NULL;
    int code = 0;

    code = ctl_call_create(&sock, &ctl);
    if (code != 0) {
	goto done;
    }

    /*
     * If we have too many requests running, don't accept any more; instead,
     * send a "busy" message to the client and close the socket.
     */
    opr_mutex_enter(&srv->lock);
    if (srv->n_calls > MAX_CALLS) {
	code = EBUSY;
    } else {
	srv->n_calls++;
	ctl->server = srv;
    }
    opr_mutex_exit(&srv->lock);
    if (code != 0) {
	ctl_send_abort(ctl, code);
	goto done;
    }

    spawn_thread(req_thread, ctl);

    /* 'req_thread' is responsible for our ctl now; don't destroy it here. */
    ctl = NULL;

 done:
    if (sock >= 0) {
	close(sock);
    }
    server_call_destroy(&ctl);
    return code;
}

/* Thread that runs accept() in a loop, and calls handle_req() for every
 * connected socket. */
static void *
accept_thread(void *rock)
{
    struct afsctl_server *srv = rock;

    opr_threadname_set("afsctl accept");

    for (;;) {
	int code;
	int sock;
#ifdef SOCK_CLOEXEC
	sock = accept4(srv->accept_sock, NULL, NULL, SOCK_CLOEXEC);
#else
	sock = accept(srv->accept_sock, NULL, NULL);
#endif
	if (sock < 0) {
	    code = errno;
	    ViceLog(0, ("afsctl: Error %d accepting socket\n", code));
	    sleep(1);
	    continue;
	}

	code = handle_req(srv, sock);
	if (code != 0) {
	    ViceLog(0, ("afsctl: Error %d handling request\n", code));
	    continue;
	}
    }

    return NULL;
}

/* Free a partially-initialized server. We don't currently support shutting
 * down a running server, so the server must not be running. */
static void
server_free(struct afsctl_server **a_srv)
{
    struct afsctl_server *srv = *a_srv;
    if (srv == NULL) {
	return;
    }
    *a_srv = NULL;

    /* We haven't implemented shutting down a server after the accept thread
     * has started. So if we're here, it had better not be running. */
    opr_Assert(!srv->thread_running);

    if (srv->accept_sock >= 0) {
	close(srv->accept_sock);
	srv->accept_sock = -1;
    }
    json_decref(srv->methods);
    opr_mutex_destroy(&srv->lock);
    free(srv);
}

/*
 * Return a string describing the call, containing the pid, comm, and "reason"
 * string (if one was given by the client). The caller must _NOT_ free the
 * returned string.
 *
 * This only makes sense to call on an afsctl server.
 */
const char *
afsctl_call_describe(struct afsctl_call *ctl)
{
    if (ctl->descr == NULL) {
	return "<null>";
    }
    return ctl->descr;
}

/**
 * Create an afsctl server instance.
 *
 * This doesn't cause the server to start serving requests yet; you need to
 * register methods with afsctl_server_reg, and then call afsctl_server_listen;
 * then we'll start serving requests.
 *
 * Note that an afsctl server cannot (currently) be shut down after it has been
 * started; we effectively shut down when the process exits.
 *
 * @return errno error codes
 */
int
afsctl_server_create(struct afsctl_serverinfo *sinfo,
		     struct afsctl_server **a_srv)
{
    int code;
    struct afsctl_server *srv = NULL;
    struct sockaddr_un addr;
    struct stat st;

    memset(&addr, 0, sizeof(addr));
    memset(&st, 0, sizeof(st));

    if (sinfo->server_type == NULL) {
	code = EINVAL;
	goto done;
    }

    srv = calloc(1, sizeof(*srv));
    if (srv == NULL) {
	code = errno;
	goto done;
    }

    opr_mutex_init(&srv->lock);
    srv->accept_sock = -1;
    srv->server_type = sinfo->server_type;

    srv->methods = json_object();
    if (srv->methods == NULL) {
	code = ENOMEM;
	goto done;
    }

    code = ctl_socket(sinfo->server_type, sinfo->sock_path, &srv->accept_sock,
		      &addr);
    if (code != 0) {
	goto done;
    }

    /*
     * If the socket already exists on disk, unlink it before we bind. Don't
     * unlink it if it's a non-socket, in case someone accidentally specifies
     * some other path.
     */
    code = stat(addr.sun_path, &st);
    if (code == 0 && S_ISSOCK(st.st_mode)) {
	(void)unlink(addr.sun_path);
    }

    code = bind(srv->accept_sock, &addr, sizeof(addr));
    if (code != 0) {
	code = errno;
	ViceLog(0, ("afsctl: Error %d binding to %s\n", code, addr.sun_path));
	goto done;
    }

    code = listen(srv->accept_sock, ACCEPT_BACKLOG);
    if (code != 0) {
	code = errno;
	ViceLog(0, ("afsctl: Error %d listening to socket\n", code));
	goto done;
    }

    *a_srv = srv;
    srv = NULL;

 done:
    server_free(&srv);
    return code;
}

/**
 * Register afsctl methods with the afsctl server.
 *
 * @param[in] srv   The afsctl server.
 * @param[in] methlist	An array of afsctl_server_method structs, terminated by
 *			a blank struct.
 * @return errno error codes
 * @retval EEXIST A given method name is already registered
 */
int
afsctl_server_reg(struct afsctl_server *srv,
		  struct afsctl_server_method *methlist)
{
    struct afsctl_server_method *meth;

    for (meth = methlist; meth->name != NULL; meth++) {
	int code = method_reg(srv, meth);
	if (code != 0) {
	    return code;
	}
    }

    return 0;
}

/* Start accepting and handling afsctl server requests. */
int
afsctl_server_listen(struct afsctl_server *srv)
{
    if (json_object_size(srv->methods) == 0) {
	return ENOTCONN;
    }

    /* Start accept()ing calls. */
    spawn_thread(accept_thread, srv);
    srv->thread_running = 1;

    return 0;
}

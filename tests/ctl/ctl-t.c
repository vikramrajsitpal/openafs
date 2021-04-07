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

#include "common.h"

#include <afs/afsctl.h>
#include <opr/lock.h>

#ifdef AFS_CTL_ENV

static struct {
    int server_running;

    opr_mutex_t lock;
    opr_cv_t cv;
} data;

static void
meth_preamble(void)
{
    opr_mutex_enter(&data.lock);
    opr_Assert(!data.server_running);
    data.server_running = 1;
    opr_mutex_exit(&data.lock);
}

static void
meth_postamble(void)
{
    opr_mutex_enter(&data.lock);
    opr_Assert(data.server_running);
    data.server_running = 0;
    opr_cv_broadcast(&data.cv);
    opr_mutex_exit(&data.lock);
}

static int
meth_noarg(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    meth_preamble();
    meth_postamble();
    return 0;
}

static int
meth_inarg(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    int code;
    char *arg_s;
    int arg_num;

    meth_preamble();

    code = json_unpack(in_args, "{s:s, s:i}", "str", &arg_s, "num", &arg_num);
    if (code != 0) {
	code = EINVAL;
	goto done;
    }

    is_string("str input arg for test.inarg", arg_s,
	      "test.inarg string input arg matches");
    is_int(1, arg_num, "test.inarg num input arg matches");

 done:
    meth_postamble();
    return code;
}

static int
meth_outarg(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    meth_preamble();

    *out_args = json_pack("{s:s, s:i}",
			  "str", "str output arg for test.outarg",
			  "num", 2);
    if (*out_args == NULL) {
	bail("json_pack failed");
    }

    meth_postamble();
    return 0;
}

static int
meth_botharg(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    int code;
    char *arg_s;
    int arg_num;

    meth_preamble();

    code = json_unpack(in_args, "{s:s, s:i}", "str", &arg_s, "num", &arg_num);
    if (code != 0) {
	code = EINVAL;
	goto done;
    }

    is_string("str input arg for test.botharg", arg_s,
	      "test.botharg string input arg matches");
    is_int(3, arg_num, "test.botharg num input arg matches");

    *out_args = json_pack("{s:s, s:i}",
			  "str", "str output arg for test.botharg",
			  "num", 4);
    if (*out_args == NULL) {
	bail("json_pack failed");
    }

 done:
    meth_postamble();
    return code;
}

static int
meth_fail(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    int code;
    int arg_code;

    meth_preamble();

    code = json_unpack(in_args, "{s:i}", "code", &arg_code);
    if (code != 0) {
	code = EINVAL;
	goto done;
    }

    code = arg_code;

 done:
    meth_postamble();
    return code;
}

static int
meth_hang(struct afsctl_call *ctl, json_t *in_args, json_t **out_args)
{
    int code;
    int timeout_ms;
    int should_timeout;

    meth_preamble();

    code = json_unpack(in_args, "{s:i, s:b}",
		       "timeout_ms", &timeout_ms,
		       "should_timeout", &should_timeout);
    if (code != 0) {
	code = EINVAL;
	goto done;
    }

    code = afsctl_send_pack(ctl, "{s:s}", "hang_data", "data for hang");
    if (code != 0) {
	goto done;
    }

    code = afsctl_wait_recv(ctl, timeout_ms);
    if (should_timeout) {
	is_int(ETIMEDOUT, code, "wait_recv call timed out");
    } else {
	is_int(0, code, "wait_recv call succeeded");
    }

    code = 0;

 done:
    meth_postamble();
    return code;
}

static struct afsctl_server_method methods[] = {
    { .name = "test.noarg",   .func = meth_noarg },
    { .name = "test.inarg",   .func = meth_inarg },
    { .name = "test.outarg",  .func = meth_outarg },
    { .name = "test.botharg", .func = meth_botharg },
    { .name = "test.fail",    .func = meth_fail },
    { .name = "test.hang",    .func = meth_hang },
    {0}
};

struct testcase {
    char *descr;
    char *method;
    char *in_json;
    char *out_json;
    int hang_ms;
    int hang_code;
    int code;
};
static struct testcase cases[] = {
    {
	.descr = "call test.noarg",
	.method = "test.noarg",
    },
    {
	.descr = "call test.inarg",
	.method = "test.inarg",
	.in_json = "{\"str\":\"str input arg for test.inarg\","
		   " \"num\":1}",
    },
    {
	.descr = "call test.outarg",
	.method = "test.outarg",
	.out_json = "{\"str\":\"str output arg for test.outarg\","
		     "\"num\":2}",
    },
    {
	.descr = "call test.botharg",
	.method = "test.botharg",
	.in_json = "{\"str\":\"str input arg for test.botharg\","
		    "\"num\":3}",
	.out_json = "{\"str\":\"str output arg for test.botharg\","
		     "\"num\":4}",
    },
    {
	.descr = "ignore test.botharg output",
	.method = "test.botharg",
	.in_json = "{\"str\":\"str input arg for test.botharg\","
		   " \"num\":3}",
    },
    {
	.descr = "call test.fail",
	.method = "test.fail",
	.in_json = "{\"code\":42}",
	.code = 42,
    },
    {
	.descr = "call test.hang (timeout)",
	.method = "test.hang",
	.in_json = "{\"timeout_ms\":1,\"should_timeout\":true}",
	.hang_ms = 100,
    },
    {
	.descr = "call test.hang (no timeout)",
	.method = "test.hang",
	.in_json = "{\"timeout_ms\":500,\"should_timeout\":false}",
	/* Wait 20ms to try to make sure the server thread is waiting inside
	 * afsctl_wait_recv(). */
	.hang_ms = 20,
	.hang_code = ETIMEDOUT,
    },
    {0}
};

static void
runtests(void)
{
    static char *server_type = "testserver";
    int code;
    char *dirname;
    char *sock_path;
    struct afsctl_serverinfo sinfo;
    struct afsctl_clientinfo cinfo;
    struct afsctl_server *srv = NULL;
    struct testcase *test = NULL;

    memset(&sinfo, 0, sizeof(sinfo));
    memset(&cinfo, 0, sizeof(cinfo));

    opr_mutex_init(&data.lock);
    opr_cv_init(&data.cv);

    dirname = afstest_mkdtemp();
    sock_path = afstest_asprintf("%s/test.ctl.sock", dirname);

    sinfo.sock_path = sock_path;

    /* Start ctl server. */

    code = afsctl_server_create(&sinfo, &srv);
    is_int(EINVAL, code,
	   "afsctl_server_create without server_type fails with EINVAL");

    sinfo.server_type = server_type;

    code = afsctl_server_create(&sinfo, &srv);
    opr_Assert(code == 0);

    code = afsctl_server_reg(srv, methods);
    opr_Assert(code == 0);

    code = afsctl_server_listen(srv);
    opr_Assert(code == 0);

    /* Run ctl client calls against server. */

    cinfo.sock_path = sock_path;

    code = afsctl_client_call(&cinfo, "test.noarg", json_null(), NULL);
    is_int(EINVAL, code,
	   "client call without server_type fails with EINVAL");

    cinfo.server_type = "wrong";
    code = afsctl_client_call(&cinfo, "test.noarg", json_null(), NULL);
    is_int(EPROTOTYPE, code,
	   "client call with wrong server_type fails with EPROTOTYPE");

    cinfo.server_type = server_type;

    code = afsctl_client_call(&cinfo, "test.what", json_null(), NULL);
    is_int(ENOTSUP, code,
	   "client call with bad method fails with ENOTSUP");

    for (test = cases; test->descr != NULL; test++) {
	json_t *in_args = json_null();
	json_t *out_args = NULL, **out_args_p = NULL;

	if (test->in_json != NULL) {
	    json_error_t jerror;
	    in_args = json_loads(test->in_json, 0, &jerror);
	    if (in_args == NULL) {
		bail("%s: json_loads(in_json) failed: %s", test->descr, jerror.text);
	    }
	}
	if (test->out_json != NULL) {
	    out_args_p = &out_args;
	}

	if (test->hang_ms == 0) {
	    code = afsctl_client_call(&cinfo, test->method, in_args, out_args_p);

	} else {
	    struct afsctl_call *ctl = NULL;
	    json_t *jobj = NULL;
	    char *hang_data;

	    code = afsctl_client_start(&cinfo, test->method, in_args, &ctl);
	    if (code != 0) {
		bail("afsctl_client_start returned %d", code);
	    }

	    code = afsctl_recv_unpack(ctl, &jobj, "{s:s}", "hang_data", &hang_data);
	    if (code != 0) {
		bail("afsctl_recv_unpack returned %d", code);
	    }

	    is_string("data for hang", hang_data, "hang data matches");

	    code = afsctl_wait_recv(ctl, test->hang_ms);
	    is_int(test->hang_code, code,
		   "%s: afsctl_wait_recv returns %d", test->descr, test->hang_code);

	    code = afsctl_client_end(ctl, out_args_p);

	    afsctl_call_destroy(&ctl);
	    json_decref(jobj);
	}

	is_int(test->code, code, "%s: call return code == %d", test->descr, test->code);

	if (test->out_json != NULL) {
	    char *out_json;
	    opr_Assert(out_args != NULL);

	    out_json = json_dumps(out_args, JSON_COMPACT);
	    opr_Assert(out_json != NULL);

	    is_string(test->out_json, out_json, "%s: output args match", test->descr);
	}
    }

    /* Wait for the server thread to finish running. It should finish quickly,
     * since we ended the ctl. */
    opr_mutex_enter(&data.lock);
    if (data.server_running) {
	struct timespec waittime;
	struct timeval tv;
	memset(&waittime, 0, sizeof(waittime));
	memset(&tv, 0, sizeof(tv));

	opr_Verify(gettimeofday(&tv, NULL) == 0);
	/* Wait 1 second for the server to finish. */
	waittime.tv_sec = tv.tv_sec + 1;
	waittime.tv_nsec = tv.tv_usec * 1000;
	code = opr_cv_timedwait(&data.cv, &data.lock, &waittime);
	opr_Assert(code == 0 || code == ETIMEDOUT);
    }
    is_int(0, data.server_running, "server thread finished");
    opr_mutex_exit(&data.lock);

    afstest_rmdtemp(dirname);
}
#endif /* AFS_CTL_ENV */

int
main(int argc, char *argv[])
{
#ifdef AFS_CTL_ENV
    setprogname(argv[0]);
    plan(27);
    runtests();
#else
    skip_all("Built without afsctl support");
#endif
    return 0;
}

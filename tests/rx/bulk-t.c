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

#include <rx/rx_bulk.h>
#include "rx_conn.h"
#include "rx_internal.h"
#include "common.h"
#include "test.h"

/*
 * rxgen won't give us an rxbulk_TEST_CheckOddSingle function, because
 * TEST_CheckOddSingle is a non-bulk RPC. We fake our own client stub function
 * here, so we can try running this in a bulk call to test that it fails.
 *
 * This is just a copy of the rxgen-generated rxbulk_TEST_CheckOdd with the
 * opcode changed.
 */
static int
fake_rxbulk_TEST_CheckOddSingle(struct rx_bulk *z_bulk, int val)
{
    static int z_op = 103;
    int z_result;
    XDR *z_xdrs = NULL;
    struct rxbulk_call_info z_callinfo;

    memset(&z_callinfo, 0, sizeof(z_callinfo));
    z_callinfo.op = z_op;
    z_callinfo.cstat.rxInterface = TEST_STATINDEX;
    z_callinfo.cstat.currentFunc = 2;
    z_callinfo.cstat.totalFunc = TEST_NO_OF_STAT_FUNCS;

    z_result = rxbulk_newcall(z_bulk, &z_callinfo, &z_xdrs);
    if (z_result != 0) {
	goto fail;
    }

    /* Marshal the arguments */
    if ((!xdr_int(z_xdrs, &z_op))
	 || (!xdr_int(z_xdrs, &val))) {
	z_result = RXGEN_CC_MARSHAL;
	goto fail;
    }

    z_result = RXGEN_SUCCESS;
 fail:
    return z_result;
}

static int
start_server(void *rock)
{
    int code;

    code = rx_Init(htons(TEST_PORT));
    if (code != 0) {
	bail("rx_Init returned %d", code);
    }

    return afstest_StartTestRPCService(NULL, "test", TEST_PORT,
				       TEST_SERVICE_ID, TEST_ExecuteRequest);
}

int
main(int argc, char *argv[])
{
    struct rx_connection *rxconn;
    struct rx_connection *bad_conn;
    struct rxbulk_init_opts opts = {
	.rpc = {
	    .start = StartTEST_BulkCall,
	    .end = EndTEST_BulkCall,
	},
    };
    struct rxbulk_single_error err;
    struct rxbulk_inline_errors inline_errs;
    int inline_codes[5];
    int three = 0, thirty = 0;
    int result = 0;
    char *foobar = NULL;
    char *fooquux = NULL;
    struct rx_bulk *bulk = NULL;
    int code;
    int call_i;

    memset(&err, 0, sizeof(err));
    memset(&inline_errs, 0, sizeof(inline_errs));
    memset(&inline_codes, 0, sizeof(inline_codes));

    setprogname(argv[0]);

    afstest_ForkRxProc(start_server, NULL);

    plan(138);

    code = rx_Init(0);
    if (code != 0) {
	bail("rx_Init returned %d", code);
    }

    rxconn = rx_NewConnection(htonl(0x7f000001), htons(TEST_PORT),
			      TEST_SERVICE_ID, rxnull_NewClientSecurityObject(), 0);

    bad_conn = rx_NewConnection(htonl(0x7f000001), htons(TEST_PORT),
				TEST_SERVICE_ID, rxnull_NewClientSecurityObject(), 0);
    rxi_ConnectionError(bad_conn, RX_PROTOCOL_ERROR);


    /* Just to make sure the RPCs behave as expected. */
    diag("Run some RPCs without rxbulk");

    code = TEST_CheckOdd(rxconn, 5);
    is_int(0, code, "CheckOdd(5)");

    code = TEST_CheckOdd(rxconn, 6);
    is_int(TEST_CHECKODD_NOTODD, code, "CheckOdd(6)");

    code = TEST_CheckOddSingle(rxconn, 5);
    is_int(0, code, "CheckOddSingle(5)");

    code = TEST_CheckOddSingle(rxconn, 6);
    is_int(TEST_CHECKODD_NOTODD, code, "CheckOddSingle(6)");

    code = TEST_Sum(rxconn, 10, 2, &result);
    is_int(0, code, "Sum success");
    is_int(12, result, "Sum result");

    code = TEST_Concat(rxconn, "foo", "bar", &foobar);
    is_int(0, code, "Concat success");
    is_string("foobar", foobar, "Concat value");

    /* Check that callNumber is where we expec.t */
    is_int(6, rxconn->callNumber[0], "rxconn callNumber");

    /* Check that rxbulk_free(&NULL) does nothing. */
    rxbulk_free(&bulk);



    code = rxbulk_init(&bulk, NULL);
    is_int(EINVAL, code, "rxbulk_init(NULL) gives EINVAL");

    code = rxbulk_init(&bulk, &opts);
    is_int(0, code, "rxbulk_init succeeds");

    code = rxbulk_runall(bulk, rxconn, &err);
    is_int(EINVAL, code, "rxbulk_runall without any calls returns EINVAL");
    is_int(-1, err.idx, "err.idx is -1");
    is_int(0, err.op, "err.op is 0");

    code = rxbulk_runall(bulk, rxconn, NULL);
    is_int(EINVAL, code,
	   "rxbulk_runall without any calls returns EINVAL (NULL single_err)");

    diag("Run some RPCs in a bulk call");

    code = rxbulk_TEST_CheckOdd(bulk, 3);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    code = rxbulk_TEST_CheckOdd(bulk, 5);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    code = rxbulk_TEST_Sum(bulk, 1, 2, &three);
    is_int(0, code, "rxbulk_TEST_Sum success");

    code = rxbulk_TEST_Sum(bulk, 10, 20, &thirty);
    is_int(0, code, "rxbulk_TEST_Sum success");

    code = rxbulk_TEST_Concat(bulk, "foo", "quux", &fooquux);
    is_int(0, code, "rxbulk_TEST_Concat success");

    is_int(5, rxbulk_ncalls(bulk), "rxbulk_ncalls is 5");

    code = rxbulk_runall(bulk, rxconn, &err);
    is_int(0, code, "rxbulk_runall success");
    is_int(3, three, "Sum is 3");
    is_int(30, thirty, "Sum is 30");
    is_string("fooquux", fooquux, "Concat is fooquux");

    is_int(7, rxconn->callNumber[0], "rxconn callNumber");

    three = 0;
    thirty = 0;
    osi_Free(fooquux, strlen(fooquux)+1);
    fooquux = NULL;

    rxbulk_reset(bulk);


    diag("Try to run a non-bulk RPC (TEST_CheckOddSingle) in a bulk call");

    code = rxbulk_TEST_CheckOdd(bulk, 3);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    /* This one should fail. */
    code = fake_rxbulk_TEST_CheckOddSingle(bulk, 5);
    is_int(0, code, "fake rxbulk_TEST_CheckOddSingle success");

    code = rxbulk_TEST_Sum(bulk, 1, 2, &three);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    code = rxbulk_TEST_Sum(bulk, 10, 20, &thirty);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    code = rxbulk_TEST_Concat(bulk, "foo", "quux", &fooquux);
    is_int(0, code, "rxbulk_TEST_Concat success");

    code = rxbulk_runall(bulk, rxconn, &err);
    is_int(RXGEN_OPCODE, code, "rxbulk_runall fails with RXGEN_OPCODE");
    is_int(1, err.idx, "err.idx");
    is_string("TEST_CheckOddSingle", TEST_TranslateOpCode(err.op), "err.op");

    is_int(0, three, "Sum not run");
    is_int(0, thirty, "Sum not run");
    ok(fooquux == NULL, "Concat not run");

    is_int(8, rxconn->callNumber[0], "rxconn callNumber");

    rxbulk_reset(bulk);


    diag("Run a bulk call with a failing RPC");

    code = rxbulk_TEST_CheckOdd(bulk, 3);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    /* This one should fail. */
    code = rxbulk_TEST_CheckOdd(bulk, 6);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    code = rxbulk_TEST_Sum(bulk, 1, 2, &three);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    code = rxbulk_TEST_Sum(bulk, 10, 20, &thirty);
    is_int(0, code, "rxbulk_TEST_CheckOdd success");

    code = rxbulk_TEST_Concat(bulk, "foo", "quux", &fooquux);
    is_int(0, code, "rxbulk_TEST_Concat success");

    code = rxbulk_runall(bulk, rxconn, &err);
    is_int(TEST_CHECKODD_NOTODD, code,
	   "rxbulk_runall fails with TEST_CHECKODD_NOTODD");
    is_int(1, err.idx, "err.idx");
    is_string("TEST_CheckOdd", TEST_TranslateOpCode(err.op), "err.op");

    is_int(0, three, "Sum not run");
    is_int(0, thirty, "Sum not run");
    ok(fooquux == NULL, "Concat not run");

    is_int(9, rxconn->callNumber[0], "rxconn callNumber");


    code = rxbulk_runall(bulk, bad_conn, &err);
    is_int(RX_PROTOCOL_ERROR, code, "rxbulk_runall fails against a bad conn");
    is_int(-1, err.idx, "err.idx");
    is_string("TEST_CheckOdd", TEST_TranslateOpCode(err.op), "err.op");


    diag("Run an inline bulk call");

    code = rxbulk_runall_inline(bulk, rxconn, NULL);
    is_int(EINVAL, code, "rxbulk_runall_inline with NULL errors");

    inline_errs.codes = inline_codes;

    inline_errs.n_calls = 0;
    code = rxbulk_runall_inline(bulk, rxconn, &inline_errs);
    is_int(EINVAL, code, "rxbulk_runall_inline with invalid error n_calls");
    inline_errs.n_calls = sizeof(inline_codes)/sizeof(inline_codes[0]);

    inline_errs.codes = NULL;
    code = rxbulk_runall_inline(bulk, rxconn, &inline_errs);
    is_int(EINVAL, code, "rxbulk_runall_inline with invalid error codes");
    inline_errs.codes = inline_codes;

    code = rxbulk_runall_inline(bulk, rxconn, &inline_errs);
    is_int(0, code, "rxbulk_runall_inline success");
    is_int(3, three, "Sum is 3");
    is_int(30, thirty, "Sum is 30");
    is_string("fooquux", fooquux, "Concat is fooquux");

    for (call_i = 0; call_i < inline_errs.n_calls; call_i++) {
	if (call_i == 1) {
	    is_int(TEST_CHECKODD_NOTODD, inline_codes[call_i],
		   "inline_codes[%d] TEST_CHECKODD_NOTODD", call_i);
	} else {
	    is_int(0, inline_codes[call_i], "inline_codes[%d] success", call_i);
	}
    }

    is_int(10, rxconn->callNumber[0], "rxconn callNumber");

    three = 0;
    thirty = 0;
    osi_Free(fooquux, strlen(fooquux)+1);
    fooquux = NULL;

    rxbulk_free(&bulk);
    ok(bulk == NULL, "rxbulk_free NULLs arg");
    rxbulk_free(&bulk);


    /*
     * Run a bulk call with a failing call followed by some calls with large
     * input args. This should trigger the server inarg-"draining" behavior;
     * check that we still properly get error information when this happens,
     * and that the remaining calls still don't run.
     */
    diag("Run a failing bulk call with large input args");

#define N_CALLS 30
    {
	char *outstr[N_CALLS];
	char bigstr[512];

	memset(outstr, 0, sizeof(outstr));
	memset(bigstr, 0, sizeof(bigstr));

	/* Make 'bigstr' a string of 500 'a's. */
	memset(bigstr, 'a', 500);

	code = rxbulk_init(&bulk, &opts);
	is_int(0, code, "rxbulk_init succeeds");

	code = rxbulk_TEST_Sum(bulk, 1, 2, &three);
	is_int(0, code, "rxbulk_TEST_Sum success");

	code = rxbulk_TEST_CheckOdd(bulk, 2);
	is_int(0, code, "rxbulk_TEST_CheckOdd success");

	for (call_i = 0; call_i < N_CALLS; call_i++) {
	    code = rxbulk_TEST_Concat(bulk, bigstr, bigstr, &outstr[call_i]);
	    is_int(0, code, "rxbulk_TEST_Concat success [%d]", call_i);
	}

	/* Try to go over BULK_MAXCALLS. */
	code = rxbulk_TEST_CheckOdd(bulk, 3);
	is_int(E2BIG, code, "extra rxbulk_TEST_CheckOdd fails with E2BIG");

	memset(&err, 0, sizeof(err));
	code = rxbulk_runall(bulk, rxconn, &err);
	is_int(TEST_CHECKODD_NOTODD, code,
	       "rxbulk_runall fails with TEST_CHECKODD_NOTODD");
	is_int(1, err.idx, "err.idx");
	is_string("TEST_CheckOdd", TEST_TranslateOpCode(err.op), "err.op");
	is_int(3, three, "TEST_Sum still ran successfully");

	for (call_i = 0; call_i < N_CALLS; call_i++) {
	    ok(outstr[call_i] == NULL, "outstr[%d] still NULL", call_i);
	}

	rxbulk_free(&bulk);
    }

    diag("Run truncated TEST_BulkCall");

    {
	struct rx_call *rxcall = rx_NewCall(rxconn);

	code = StartTEST_BulkCall(rxcall, 0x1);
	is_int(0, code, "StartTEST_BulkCall success");

	code = EndTEST_BulkCall(rxcall);
	is_int(0, code, "EndTEST_BulkCall success");

	code = rx_EndCall(rxcall, code);
	is_int(RXGEN_SS_UNMARSHAL, code,
	       "rx_EndCall fails with RXGEN_SS_UNMARSHAL");
    }

    rx_DestroyConnection(rxconn);

    return 0;
}

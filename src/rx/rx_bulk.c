/*
 * Copyright (c) 2021 Sine Nomine Associates
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>

#include "rx_internal.h"
#include "rx_call.h"
#include "rx_conn.h"
#include "rx_globals.h"
#include <rx/xdr.h>
#include <afs/rxgen_consts.h>

#include "rx_bulk.h"

/* Flags given to the *_BulkCall RPC. Note that these are constants that go
 * over the wire. */
#define RX_BULKCALL_INLINE   0x1
#define RX_BULKCALL_NOINLINE 0x2

/*
 * An arbitrary limit to how many calls we can service in a single bulk call.
 * Raise as needed, but rx_bulk.calls should be changed to be allocated
 * dynamically if we need much more than this.
 */
#define BULK_MAXCALLS 32

struct rx_bulk {
    /* The underlying *_BulkCall rpc to run. */
    struct rxbulk_rpc rpc;

    /* Our buffered input args. */
    XDR xdrs_inargs;

    /* Per-call information. */
    int n_calls;
    struct rxbulk_call_info calls[BULK_MAXCALLS];
};

/**
 * Initialize an rxbulk context.
 *
 * An rxbulk context can be used like so, to run mutiple RPCs at once:
 *
 * struct rxbulk_init_opts opts = {
 *	.rpc = {
 *	    .start = StartPKG_BulkCall,
 *	    .end = EndPKG_BulkCall,
 *	},
 * };
 * code = rxbulk_init(&bulk, &opts);
 * code = rxbulk_PKG_Foo(bulk, 0x1234, &some_data);
 * code = rxbulk_PKG_Bar(bulk, "foo");
 * code = rxbulk_PKG_Baz(bulk);
 * code = rxbulk_runall(bulk, rxconn, &errinfo);
 * rxbulk_free(&bulk);
 *
 * The rxbulk_PKG_Foo() etc functions are generated by rxgen, and are similar
 * to normal PKG_Foo() calls. But in this example, the PKG_Foo() RPC is
 * buffered, and not actually run (and any output args are not valid) until
 * rxbulk_runall() finishes successfully.
 *
 * If any of the inner RPCs fail, rxbulk_runall() will return an error, and all
 * of the RPCs after the failed one will not be run at all. (See
 * rxbulk_runall() for information about seeing which inner RPC failed.)
 */
int
rxbulk_init(struct rx_bulk **a_bulk, struct rxbulk_init_opts *opts)
{
    int code;
    struct rx_bulk *bulk = NULL;

    if (opts == NULL || opts->rpc.start == NULL || opts->rpc.end == NULL) {
	code = EINVAL;
	goto done;
    }

    bulk = calloc(1, sizeof(*bulk));
    if (bulk == NULL) {
	code = ENOMEM;
	goto done;
    }

    bulk->rpc = opts->rpc;
    xdrbuf_create(&bulk->xdrs_inargs, 0);

    *a_bulk = bulk;
    bulk = NULL;
    code = 0;

 done:
    rxbulk_free(&bulk);
    return code;
}

/**
 * Free the given rxbulk context.
 *
 * @param[inout] a_bulk	The context to free. If NULL, this is a no-op. Set to
 *			NULL on return.
 */
void
rxbulk_free(struct rx_bulk **a_bulk)
{
    struct rx_bulk *bulk = *a_bulk;

    *a_bulk = NULL;
    if (bulk == NULL) {
	return;
    }

    rxbulk_reset(bulk);
    xdr_destroy(&bulk->xdrs_inargs);
    free(bulk);
}

/**
 * Return the number of RPCs in this bulk handle.
 *
 * @return  number of RPCs
 */
int
rxbulk_ncalls(struct rx_bulk *bulk)
{
    return bulk->n_calls;
}

/**
 * This is used to implement the generated rxbulk_PKG_Foo() functions; it
 * should generally only be called by generated code.
 *
 * @param[in] bulk  The rxbulk context.
 * @param[in] callinfo	Info for the call.
 * @param[out] a_xdrs	On success, set to the XDR handle to use to write input
 *			arguments to the call.
 *
 * @retval E2BIG    You have tried to make too many calls for this rxbulk
 *		    context.
 */
int
rxbulk_newcall(struct rx_bulk *bulk, struct rxbulk_call_info *callinfo,
	       XDR **a_xdrs)
{
    int code;
    int call_i;
    struct rxbulk_call_info *bcall;

    if (callinfo->op == 0 || callinfo->cstat.totalFunc == 0) {
	code = EINVAL;
	goto done;
    }

    call_i = bulk->n_calls;
    if (call_i >= BULK_MAXCALLS) {
	code = E2BIG;
	goto done;
    }

    bulk->n_calls++;

    bcall = &bulk->calls[call_i];
    bcall->op = callinfo->op;
    bcall->cstat = callinfo->cstat;

    bcall->outargs_cb = callinfo->outargs_cb;
    if (callinfo->outargs_rock.val != NULL) {
	code = rx_opaque_copy(&bcall->outargs_rock, &callinfo->outargs_rock);
	if (code != 0) {
	    goto done;
	}
    }

    bcall->inargs_start = xdr_getpos(&bulk->xdrs_inargs);

    *a_xdrs = &bulk->xdrs_inargs;
    code = 0;

 done:
    return code;
}

static void
record_stats(struct rx_bulk *bulk, struct rx_call *rxcall, int last_call_ran)
{
    struct clock queue;
    struct clock exec;
    int call_i;
    struct rx_peer *peer = rxcall->conn->peer;

    clock_GetTime(&exec);
    clock_Sub(&exec, &rxcall->startTime);
    queue = rxcall->startTime;
    clock_Sub(&queue, &rxcall->queueTime);

    /*
     * For each inner call up to last_call_ran, record in our stats that we
     * ran the call. We count the call even if the call failed, just like
     * normal PKG_Call() calls do. But don't count anything after
     * last_call_ran, because the RPC may not have actually been called
     * (e.g. calls after a failing call in a non-inline BulkCall).
     *
     * For all the calls that ran, we record the same queue and execution time
     * for the calls. This isn't really accurate: if we run 2 calls via rxbulk,
     * and each one takes 1 second to run on the server, then we'll record each
     * call as taking 2 seconds. But we have no way of knowing how much time
     * was taken for one call vs the other, so we just record the whole time
     * for all calls in the rxbulk call. You can kind of think of it like the
     * calls were executing in parallel; so they took 2 seconds in total, but
     * each of those 2 calls were running at about the same time.
     */
    for (call_i = 0; call_i <= last_call_ran; call_i++) {
	afs_uint64 bytesSent;
	afs_uint64 inargs_end;
	struct rxbulk_call_info *bcall;

	bcall = &bulk->calls[call_i];

	if (call_i < bulk->n_calls - 1) {
	    inargs_end = bulk->calls[call_i+1].inargs_start;
	} else {
	    inargs_end = xdr_getpos(&bulk->xdrs_inargs);
	}
	bytesSent = inargs_end - bcall->inargs_start;

	rxi_IncrementTimeAndCount(peer,
				  bcall->cstat.rxInterface,
				  bcall->cstat.currentFunc,
				  bcall->cstat.totalFunc, &queue, &exec,
				  bytesSent, bcall->bytesRcvd, 1);
    }
}

static int
runall_common(int is_inline, struct rx_bulk *bulk, struct rx_connection *conn,
	      struct rxbulk_single_error *single_err,
	      struct rxbulk_inline_errors *inline_errs)
{
    struct rx_call *rxcall = NULL;
    struct rx_opaque buf;
    XDR xdrs_rx_raw;
    XDR xdrs_rx;
    struct xdrsplit_info splinfo;
    int failed_call = -1;
    int last_call_ran = -1;
    int call_i;
    int code;
    int eof = 0; /* eof marker */
    afs_uint32 rpc_flags;

    memset(&buf, 0, sizeof(buf));
    memset(&xdrs_rx_raw, 0, sizeof(xdrs_rx_raw));
    memset(&xdrs_rx, 0, sizeof(xdrs_rx));
    memset(&splinfo, 0, sizeof(splinfo));

    if (single_err != NULL) {
	memset(single_err, 0, sizeof(*single_err));
	single_err->idx = -1;
    }

    if (is_inline) {
	if (inline_errs == NULL || inline_errs->codes == NULL) {
	    code = EINVAL;
	    goto done;
	}
	if (inline_errs->n_calls != bulk->n_calls) {
	    code = EINVAL;
	    goto done;
	}
	for (call_i = 0; call_i < bulk->n_calls; call_i++) {
	    /*
	     * Set the inner RPC error codes to -1 by default, just to make
	     * sure someone doesn't accidentally think the call succeeded if we
	     * bail out early.
	     */
	    inline_errs->codes[call_i] = -1;
	}
    }

    if (bulk->n_calls < 1) {
	code = EINVAL;
	goto done;
    }

    rxcall = rx_NewCall(conn);
    if (rxcall == NULL) {
	code = ENOMEM;
	goto done;
    }

    xdrrx_create(&xdrs_rx_raw, rxcall, XDR_ENCODE);

    splinfo.reader = &xdrs_rx_raw;
    splinfo.writer = &xdrs_rx_raw;
    xdrsplit_create(&xdrs_rx, &splinfo, XDR_ENCODE);

    if (is_inline) {
	rpc_flags = RX_BULKCALL_INLINE;
    } else {
	rpc_flags = RX_BULKCALL_NOINLINE;
    }

    code = (*bulk->rpc.start)(rxcall, rpc_flags);
    if (code != 0) {
	goto done;
    }

    code = xdrbuf_getbuf(&bulk->xdrs_inargs, &buf);
    if (code != 0) {
	goto done;
    }

    /* Send our (buffered) calls and input args. */
    if (!xdr_putbytes(&xdrs_rx, buf.val, buf.len) ||
	!xdr_int(&xdrs_rx, &eof)) {
	code = RXGEN_CC_MARSHAL;
	goto done;
    }

    /* Get our results for the inner RPCs. */
    xdrs_rx.x_op = XDR_DECODE;
    for (call_i = 0; call_i < bulk->n_calls; call_i++) {
	struct rxbulk_call_info *bcall;
	bcall = &bulk->calls[call_i];

	/* First, read the error code for the inner RPC. */
	if (!xdr_int(&xdrs_rx, &code)) {
	    code = RXGEN_CC_UNMARSHAL;
	    goto done;
	}

	last_call_ran = call_i;
	bcall->bytesRcvd = 0;

	if (code == 0 && bcall->outargs_cb != NULL) {
	    afs_uint64 orig_bytesRcvd = splinfo.read_bytes;

	    /* This call succeeded; read in the output args. */
	    code = (*bcall->outargs_cb)(rxcall, &xdrs_rx, &bcall->outargs_rock);

	    bcall->bytesRcvd = splinfo.read_bytes - orig_bytesRcvd;

	    if (code != 0) {
		goto done;
	    }
	}

	if (code != 0 && !is_inline) {
	    /* The inner RPC failed on the server side. For a non-inline bulk
	     * call, that means the whole bulk call fails. */
	    failed_call = call_i;

	    /* Still call 'end' on the rpc, though, so the outer PKG_BulkCall
	     * rpc still gets its stats recorded. */
	    (void)(*bulk->rpc.end)(rxcall);

	    goto done;
	}

	if (is_inline) {
	    /*
	     * For an inline bulk call, an error from an inner RPC gets
	     * recorded, but doens't cause the whole bulk call to fail (so we
	     * clear 'code' here).
	     */
	    inline_errs->codes[call_i] = code;
	    code = 0;
	}
    }

    /* All cases where 'code != 0' should have been handled above. */
    osi_Assert(code == 0);

    code = (*bulk->rpc.end)(rxcall);
    if (code != 0) {
	goto done;
    }

 done:
    if (rxcall != NULL) {
	int do_stats = 0;
	if (rx_enable_stats && last_call_ran >= 0) {
	    do_stats = 1;
	    CALL_HOLD(rxcall, RX_CALL_REFCOUNT_BEGIN);
	}

	code = rx_EndCall(rxcall, code);

	if (do_stats) {
	    record_stats(bulk, rxcall, last_call_ran);
	    CALL_RELE(rxcall, RX_CALL_REFCOUNT_BEGIN);
	}
    }

    if (code != 0 && single_err != NULL) {
	if (failed_call >= 0) {
	    /* One of our inner calls failed; let our caller know which one
	     * failed. */
	    osi_Assert(failed_call < bulk->n_calls);
	    single_err->idx = failed_call;
	    single_err->op = bulk->calls[failed_call].op;

	} else {
	    /*
	     * If we didn't have an inner call fail, give the index as -1, but
	     * give the op as the first op in the list of calls, to help
	     * identify what this call was doing.
	     */
	    single_err->idx = -1;
	    if (bulk->n_calls > 0) {
		single_err->op = bulk->calls[0].op;
	    } else {
		single_err->op = 0;
	    }
	}
    }
    return code;
}

/**
 * Run the buffered bulk calls on the given Rx conn.
 *
 * Note that the same rxbulk context can be run against multiple different Rx
 * conns. However, any output arguments will be overwritten, just like if you run:
 *
 * code = PKG_Foo(conn1, &data);
 * code = PKG_Foo(conn2, &data);
 *
 * The intended use of running against multiple Rx conns is for running RPCs
 * that don't have any output args. For running RPCs with output args, you'd
 * typically use separate rxbulk contexts for each conn, but it is possible to
 * use the same rxbulk context if you do something like this:
 *
 * code = rxbulk_PKG_Foo(bulk, &data);
 * code = rxbulk_runall(bulk, conn1, NULL);
 * data_1 = data;
 * memset(&data, 0, sizeof(data));
 *
 * code = rxbulk_runall(bulk, conn2, NULL);
 * data_2 = data;
 * memset(&data, 0, sizeof(data));
 *
 * @param[in] bulk  The rxbulk context.
 * @param[in] conn  The Rx connection to run the buffered calls on.
 * @param[out] errinfo	Optional. If non-NULL and rxbulk_runall returns an
 *			error, this contains information about which of the
 *			inner RPC calls failed.
 *
 * @returns The error code from the first failed call, or an internal error.
 */
int
rxbulk_runall(struct rx_bulk *bulk, struct rx_connection *conn,
	      struct rxbulk_single_error *single_err)
{
    return runall_common(0, bulk, conn, single_err, NULL);
}

int
rxbulk_runall_inline(struct rx_bulk *bulk, struct rx_connection *conn,
		     struct rxbulk_inline_errors *inline_errs)
{
    return runall_common(1, bulk, conn, NULL, inline_errs);
}

/**
 * Resets the rxbulk context.
 *
 * This is similar to running rxbulk_free() and rxbulk_init(), but reuses some
 * buffers, and avoids needing to specify options to rxbulk_init again.
 */
void
rxbulk_reset(struct rx_bulk *bulk)
{
    int call_i;

    for (call_i = 0; call_i < bulk->n_calls; call_i++) {
	struct rxbulk_call_info *bcall;
	bcall = &bulk->calls[call_i];

	rx_opaque_freeContents(&bcall->outargs_rock);
    }

    xdrbuf_reset(&bulk->xdrs_inargs);
    bulk->n_calls = 0;
    memset(&bulk->calls[0], 0, sizeof(bulk->calls));
}

/*
 * We can either:
 * - buffer all input args, and then have inner-RPCs write directly to the rx
 *   stream for output args
 * - have inner-RPCs read directly from the rx stream for input args, and
 *   then buffer the output args
 * - buffer both input and output args
 *
 * Buffering both input and output args seems like extra unnecessary memory
 * usage. We cannot write directly to the rx stream for output args, because we
 * don't want to send the output args if the call fails while serializing the
 * output args. So, we buffer the output args, but let calls read directly from
 * the rx stream for the input args.
 */

static int
handle_one_bulkrpc(struct rx_call *rxcall, rxbulk_proc_func bulk_proc, int op,
		   struct clock *queueStart, XDR *xdrs_out, int *a_call_failed)
{
    XDR xdrs_rx;
    XDR xdrs_innerbuf;
    XDR xdrs_split;
    int call_failed = 0;
    int code;
    struct rxbulk_call_stat cstat;
    struct clock execStart;
    struct xdrsplit_info splinfo;

    memset(&cstat, 0, sizeof(cstat));
    memset(&splinfo, 0, sizeof(splinfo));

    xdrrx_create(&xdrs_rx, rxcall, XDR_DECODE);
    xdrbuf_create(&xdrs_innerbuf, 0);

    splinfo.reader = &xdrs_rx;
    splinfo.writer = &xdrs_innerbuf;
    /* We've read a 4-byte opcode at this point for the call, so say we've
     * already read 4 bytes. */
    splinfo.read_bytes = 4;
    xdrsplit_create(&xdrs_split, &splinfo, XDR_DECODE);

    clock_GetTime(&execStart);

    /* Run the inner RPC. */
    code = (*bulk_proc)(rxcall, &xdrs_split, op, &cstat);

    if (rx_enable_stats) {
	struct clock queueTime;
	struct clock execTime;

	clock_GetTime(&execTime);
	clock_Sub(&execTime, &execStart);

	queueTime = execStart;
	clock_Sub(&queueTime, queueStart);

	rxi_IncrementTimeAndCount(rxcall->conn->peer, cstat.rxInterface,
				  cstat.currentFunc, cstat.totalFunc,
				  &queueTime, &execTime, splinfo.wrote_bytes,
				  splinfo.read_bytes, 0);
    }

    /* Write out the return code for this RPC. */
    if (!xdr_int(xdrs_out, &code)) {
	code = RXGEN_SS_MARSHAL;
	goto done;
    }

    if (code != 0) {
	/* The inner RPC failed, but that doesn't mean the bulk call gets
	 * aborted; we recorded the error code in xdrs_out, above. */
	call_failed = 1;
	code = 0;

    } else {
	/* Inner rpc was successful, so we can write its output args. */
	struct rx_opaque buf;
	xdrbuf_getbuf(&xdrs_innerbuf, &buf);
	if (buf.len > 0 && !xdr_putbytes(xdrs_out, buf.val, buf.len)) {
	    code = RXGEN_SS_MARSHAL;
	    goto done;
	}
    }

 done:
    xdr_destroy(&xdrs_innerbuf);
    *a_call_failed = call_failed;
    return code;
}

/**
 * This implements the server-side S*_BulkCall RPC, to be called from generated
 * code.
 *
 * @param[in] rxcall	The rx call.
 * @param[in] flags	The flags arg from the BulkCall RPC.
 * @param[in] bulk_proc	The call for executing bulk RPCs in this service. For
 *			example, PKG_ExecuteBulkRequest.
 */
int
rxbulk_handler(struct rx_call *rxcall, afs_uint32 flags,
	       rxbulk_proc_func bulk_proc)
{
    int code;
    int call_i = 0;
    int is_inline;
    XDR xdrs_rx;
    XDR xdrs_out;
    struct rx_opaque buf;

    xdrrx_create(&xdrs_rx, rxcall, XDR_DECODE);
    xdrbuf_create(&xdrs_out, 0);

    switch (flags) {
    case RX_BULKCALL_INLINE:
	is_inline = 1;
	break;
    case RX_BULKCALL_NOINLINE:
	is_inline = 0;
	break;
    default:
	code = RXGEN_SS_UNMARSHAL;
	goto done;
    }

    for (call_i = 0; ; call_i++) {
	int op = 0;
	int call_failed = 0;
	struct clock queueStart;

	clock_GetTime(&queueStart);

	/* Read in the opcode for the next inner RPC. */
	if (!xdr_int(&xdrs_rx, &op)) {
	    code = RXGEN_SS_UNMARSHAL;
	    goto done;
	}

	if (op == 0) {
	    /* eof */
	    code = 0;
	    if (call_i == 0) {
		/* If we haven't seen any inner calls at all, throw an error.
		 * Nobody should be calling the bulk RPC with 0 calls. */
		code = RXGEN_SS_UNMARSHAL;
		goto done;
	    }
	    goto done_rpcs;
	}

	code = handle_one_bulkrpc(rxcall, bulk_proc, op, &queueStart, &xdrs_out, &call_failed);
	if (code != 0) {
	    goto done;
	}
	if (call_failed && !is_inline) {
	    /*
	     * If the inner call failed and we're a non-inline bulk call, don't
	     * run any more inner RPCs. But make sure to rx_Read() to drain the
	     * rest of the input args sent by the client; if we just ignore the
	     * data sent by the client, Rx can behave oddly if we send the
	     * client data when the client is trying to send data to us.
	     */
	    static char devnull[1024];
	    while (rx_Read(rxcall, devnull, sizeof(devnull)) == sizeof(devnull)) {}
	    while (rx_Read(rxcall, devnull, 1) == 1) {}
	    goto done_rpcs;
	}
    }

 done_rpcs:
    /* Okay, we've run all of the inner RPCs. Now we can send our output args
     * and return codes that we've been buffering in xdrs_out. */
    xdrbuf_getbuf(&xdrs_out, &buf);
    if (buf.len > 0 && !xdr_putbytes(&xdrs_rx, buf.val, buf.len)) {
	code = RXGEN_SS_MARSHAL;
	goto done;
    }

 done:
    xdr_destroy(&xdrs_out);
    return code;
}

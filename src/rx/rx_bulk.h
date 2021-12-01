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

#ifndef OPENAFS_RX_RX_BULK_H
#define OPENAFS_RX_RX_BULK_H

#include <rx/rx.h>
#include <rx/xdr.h>
#include <rx/rx_opaque.h>

struct rx_bulk;

/* This indicates the specific RPC to call for handling a bulk call request.
 * e.g. StartPKG_BulkCall, EndPKG_BulkCall. */
struct rxbulk_rpc {
    int (*start)(struct rx_call *call, afs_uint32 flags);
    int (*end)(struct rx_call *call);
};

struct rxbulk_call_stat {
    /* Items for rx_RecordCallStatistics. */
    unsigned int rxInterface;
    unsigned int currentFunc;
    unsigned int totalFunc;
};

/* Information describing the inner RPC being called. Passed to
 * rxbulk_newcall(). */
struct rxbulk_call_info {
    /* Opcode of the op. */
    int op;

    /* Function to call to read any output arguments from the inner RPC. */
    int (*outargs_cb)(struct rx_call *rxcall, XDR *xdrs, struct rx_opaque *rock);

    /*
     * Rock to pass to end_cb. Note that this memory is copied when passed to
     * rxbulk_newcall(), so it can be freed after calling rxbulk_newcall(),
     * even though end_cb is called later.
     */
    struct rx_opaque outargs_rock;

    struct rxbulk_call_stat cstat;

    /* Offset into xdrs_inargs where the input args start for this call. */
    afs_uint64 inargs_start;
    afs_uint64 bytesRcvd;
};

struct rxbulk_single_error {
    /* If an inner RPC failed, this is the opcode of that RPC. If the whole
     * call failed, this is the opcode of the first RPC in the list. */
    int op;

    /* The index of the inner RPC that failed (starting at 0). If the whole
     * call failed, this is -1. */
    int idx;
};

struct rxbulk_inline_errors {
    int n_calls;
    afs_int32 *codes;
};

struct rxbulk_init_opts {
    struct rxbulk_rpc rpc;
};

int rxbulk_init(struct rx_bulk **a_bulk, struct rxbulk_init_opts *opts);
int rxbulk_ncalls(struct rx_bulk *bulk);
int rxbulk_newcall(struct rx_bulk *bulk, struct rxbulk_call_info *callinfo,
		   XDR **a_xdrs);
int rxbulk_runall(struct rx_bulk *bulk, struct rx_connection *conn,
		  struct rxbulk_single_error *single_err);
int rxbulk_runall_inline(struct rx_bulk *bulk, struct rx_connection *conn,
			 struct rxbulk_inline_errors *inline_errs);
void rxbulk_reset(struct rx_bulk *bulk);
void rxbulk_free(struct rx_bulk **a_bulk);

typedef int (*rxbulk_proc_func)(struct rx_call *rxcall, XDR *xdrs, int op,
				struct rxbulk_call_stat *cstat);
int rxbulk_handler(struct rx_call *rxcall, afs_uint32 flags,
		   rxbulk_proc_func bulk_proc);

#endif /* OPENAFS_RX_RX_BULK_H */

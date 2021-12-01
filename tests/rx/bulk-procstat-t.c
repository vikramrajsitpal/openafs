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

#include <rx/rxstat.h>
#include <rx/rx_bulk.h>

#include "common.h"
#include "test.h"

static u_short client_port = 1234;

static int
start_client(void *rock)
{
    int code;
    struct rx_connection *rxconn;
    int three = 0, thirty = 0;
    char *foobar = NULL;
    int inline_codes[3];
    struct rxbulk_inline_errors inline_errs;
    struct rx_bulk *bulk = NULL;
    struct rxbulk_init_opts bopts = {
	.rpc = {
	    .start = StartTEST_BulkCall,
	    .end = EndTEST_BulkCall,
	},
    };

    memset(inline_codes, 0, sizeof(inline_codes));
    memset(&inline_errs, 0, sizeof(inline_errs));

    code = rx_Init(htons(client_port));
    if (code != 0) {
	bail("client: rx_Init returned %d", code);
    }

    rx_enableProcessRPCStats();

    code = afstest_StartTestRPCService(NULL, "rpcstats", client_port,
				       RX_STATS_SERVICE_ID,
				       RXSTATS_ExecuteRequest);
    if (code != 0) {
	bail("client: failed to start stats service (%d)", code);
    }

    rxconn = rx_NewConnection(htonl(0x7f000001), htons(TEST_PORT),
			      TEST_SERVICE_ID,
			      rxnull_NewClientSecurityObject(), 0);

    opr_Verify(rxbulk_init(&bulk, &bopts) == 0);

    opr_Verify(rxbulk_TEST_Sum(bulk, 1, 2, &three) == 0);
    opr_Verify(rxbulk_TEST_Sum(bulk, 10, 20, &thirty) == 0);

    opr_Verify(rxbulk_runall(bulk, rxconn, NULL) == 0);

    opr_Assert(three == 3);
    opr_Assert(thirty == 30);
    three = 0;
    thirty = 0;

    rxbulk_reset(bulk);

    opr_Verify(rxbulk_TEST_Sum(bulk, 1, 2, &three) == 0);
    opr_Verify(rxbulk_TEST_SleepMS(bulk, 1100) == 0);
    opr_Verify(rxbulk_TEST_Sum(bulk, 10, 20, &thirty) == 0);

    opr_Verify(rxbulk_runall(bulk, rxconn, NULL) == 0);

    opr_Assert(three == 3);
    opr_Assert(thirty == 30);

    rxbulk_reset(bulk);

    opr_Verify(rxbulk_TEST_CheckOdd(bulk, 2) == 0);
    opr_Verify(rxbulk_TEST_SleepMS(bulk, 1100) == 0);
    opr_Verify(rxbulk_TEST_CheckOdd(bulk, 3) == 0);

    opr_Verify(rxbulk_runall(bulk, rxconn, NULL) == TEST_CHECKODD_NOTODD);

    rxbulk_reset(bulk);

    opr_Verify(rxbulk_TEST_CheckOdd(bulk, 2) == 0);
    opr_Verify(rxbulk_TEST_SleepMS(bulk, 1100) == 0);
    opr_Verify(rxbulk_TEST_Concat(bulk, "foo", "bar", &foobar) == 0);

    inline_errs.codes = inline_codes;
    inline_errs.n_calls = 3;
    opr_Verify(rxbulk_runall_inline(bulk, rxconn, &inline_errs) == 0);

    opr_Assert(inline_codes[0] == TEST_CHECKODD_NOTODD);
    opr_Assert(inline_codes[1] == 0);
    opr_Assert(inline_codes[2] == 0);

    opr_Assert(strcmp(foobar, "foobar") == 0);

    rxbulk_free(&bulk);

    rx_DestroyConnection(rxconn);

    return 0;
}

static int
start_server(void *rock)
{
    int code;

    code = rx_Init(htons(TEST_PORT));
    if (code != 0) {
	bail("rx_Init returned %d", code);
    }

    rx_enableProcessRPCStats();

    code = afstest_StartTestRPCService(NULL, "rpcstats", TEST_PORT,
				       RX_STATS_SERVICE_ID,
				       RXSTATS_ExecuteRequest);
    if (code != 0) {
	bail("server: failed to start stats service (%d)", code);
    }

    return afstest_StartTestRPCService(NULL, "test", TEST_PORT,
				       TEST_SERVICE_ID, TEST_ExecuteRequest);
}

static void
test_stats(int client)
{
    int code;
    afs_uint32 dummy;
    rpcStats raw_stats;
    struct rx_connection *rxconn;
    struct proc_stats stats;
    struct rxstat_entry *entry;
    u_short port;
    int remote_is_server;
    int bytes_sent, bytes_rcvd;
    int exec_time = 0;
    int exec_time_max = 0;
    char *descr;

    if (client) {
	remote_is_server = 1;
	port = client_port;
	descr = "client";

    } else {
	remote_is_server = 0;
	port = TEST_PORT;
	descr = "server";
    }

    rxconn = rx_NewConnection(htonl(0x7f000001), htons(port),
			      RX_STATS_SERVICE_ID,
			      rxnull_NewClientSecurityObject(), 0);

    memset(&raw_stats, 0, sizeof(raw_stats));
    code = RXSTATS_RetrieveProcessRPCStats(rxconn, RX_STATS_RETRIEVAL_VERSION,
					   &dummy, &dummy, &dummy,
					   &dummy, &raw_stats);
    is_int(0, code, "%s RXSTAT_RetrieveProcessRPCStats success", descr);

    memset(&stats, 0, sizeof(stats));
    teststats_unmarshal(&raw_stats, &stats);

    /*
     * Note: when we check for execution/queue times, we just check the
     * seconds. So we're just checking that the execution time, for example, is
     * 3-ish seconds.
     */
    entry = teststats_find(&stats, opcode_TEST_SleepMS);
    is_int(remote_is_server, entry->remote_is_server,
	   "%s SleepMS remote_is_server", descr);
    is_int(2, entry->invocations, "%s SleepMS invocations", descr);
    is_int(0, entry->queue_time_sum.sec, "%s SleepMS queue time", descr);
    is_int(0, entry->queue_time_min.sec, "%s SleepMS queue time min", descr);
    is_int(0, entry->queue_time_max.sec, "%s SleepMS queue time max", descr);
    is_int(2, entry->execution_time_sum.sec, "%s SleepMS exec time", descr);
    is_int(1, entry->execution_time_min.sec, "%s SleepMS exec time min", descr);
    is_int(1, entry->execution_time_max.sec, "%s SleepMS exec time max", descr);

    if (client) {
	bytes_sent = 16;
	bytes_rcvd = 0;
    } else {
	bytes_sent = 0;
	bytes_rcvd = 16;
    }
    is_int(bytes_sent, entry->bytes_sent, "%s SleepMS bytes_sent", descr);
    is_int(bytes_rcvd, entry->bytes_rcvd, "%s SleepMS bytes_rcvd", descr);

    entry = teststats_find(&stats, opcode_TEST_CheckOdd);
    is_int(remote_is_server, entry->remote_is_server,
	   "%s CheckOdd remote_is_server", descr);
    is_int(2, entry->invocations, "%s CheckOdd invocations", descr);
    is_int(0, entry->queue_time_sum.sec, "%s CheckOdd queue time", descr);
    is_int(0, entry->queue_time_min.sec, "%s CheckOdd queue time min", descr);
    is_int(0, entry->queue_time_max.sec, "%s CheckOdd queue time max", descr);
    if (client) {
	exec_time = 1;
    }
    is_int(exec_time, entry->execution_time_sum.sec, "%s CheckOdd exec time", descr);
    is_int(0, entry->execution_time_min.sec, "%s CheckOdd exec time min", descr);
    is_int(exec_time, entry->execution_time_max.sec, "%s CheckOdd exec time max", descr);

    if (client) {
	bytes_sent = 16;
	bytes_rcvd = 0;
    } else {
	bytes_sent = 0;
	bytes_rcvd = 16;
    }
    is_int(bytes_sent, entry->bytes_sent, "%s CheckOdd bytes_sent", descr);
    is_int(bytes_rcvd, entry->bytes_rcvd, "%s CheckOdd bytes_rcvd", descr);

    entry = teststats_find(&stats, opcode_TEST_Sum);
    is_int(remote_is_server, entry->remote_is_server,
	   "%s Sum remote_is_server", descr);
    is_int(4, entry->invocations, "%s Sum invocations", descr);
    is_int(0, entry->queue_time_sum.sec, "%s Sum queue time", descr);
    is_int(0, entry->queue_time_min.sec, "%s Sum queue time min", descr);
    is_int(0, entry->queue_time_max.sec, "%s Sum queue time max", descr);
    if (client) {
	exec_time = 2;
	exec_time_max = 1;
    }
    is_int(exec_time, entry->execution_time_sum.sec, "%s Sum exec time", descr);
    is_int(0, entry->execution_time_min.sec, "%s Sum exec time min", descr);
    is_int(exec_time_max, entry->execution_time_max.sec, "%s Sum exec time max", descr);
    if (client) {
	bytes_sent = 48;
	bytes_rcvd = 16;
    } else {
	bytes_sent = 16;
	bytes_rcvd = 48;
    }
    is_int(bytes_sent, entry->bytes_sent, "%s Sum bytes_sent", descr);
    is_int(bytes_rcvd, entry->bytes_rcvd, "%s Sum bytes_rcvd", descr);

    entry = teststats_find(&stats, opcode_TEST_Concat);
    is_int(remote_is_server, entry->remote_is_server,
	   "%s Concat remote_is_server", descr);
    is_int(1, entry->invocations, "%s Concat invocations", descr);
    is_int(0, entry->queue_time_sum.sec, "%s Concat queue time", descr);
    is_int(0, entry->queue_time_min.sec, "%s Concat queue time min", descr);
    is_int(0, entry->queue_time_max.sec, "%s Concat queue time max", descr);
    if (client) {
	exec_time = 1;
    }
    is_int(exec_time, entry->execution_time_sum.sec, "%s Concat exec time", descr);
    is_int(exec_time, entry->execution_time_min.sec, "%s Concat exec time min", descr);
    is_int(exec_time, entry->execution_time_max.sec, "%s Concat exec time max", descr);
    if (client) {
	bytes_sent = 20;
	bytes_rcvd = 12;
    } else {
	bytes_sent = 12;
	bytes_rcvd = 20;
    }
    is_int(bytes_sent, entry->bytes_sent, "%s Concat bytes_sent", descr);
    is_int(bytes_rcvd, entry->bytes_rcvd, "%s Concat bytes_rcvd", descr);

    entry = teststats_find(&stats, opcode_TEST_Echo);
    is_int(remote_is_server, entry->remote_is_server,
	   "%s Echo remote_is_server", descr);
    is_int(0, entry->invocations, "%s Echo invocations", descr);

    entry = teststats_find(&stats, opcode_TEST_BulkCall);
    is_int(remote_is_server, entry->remote_is_server,
	   "%s BulkCall remote_is_server", descr);
    is_int(4, entry->invocations, "%s BulkCall invocations", descr);
    is_int(0, entry->queue_time_sum.sec, "%s BulkCall queue time", descr);
    is_int(0, entry->queue_time_min.sec, "%s BulkCall queue time min", descr);
    is_int(0, entry->queue_time_max.sec, "%s BulkCall queue time max", descr);
    is_int(2, entry->execution_time_sum.sec, "%s BulkCall exec time", descr);
    is_int(0, entry->execution_time_min.sec, "%s BulkCall exec time min", descr);
    is_int(1, entry->execution_time_max.sec, "%s BulkCall exec time max", descr);
    if (client) {
	bytes_sent = 164;
	bytes_rcvd = 64;
    } else {
	bytes_sent = 64;
	bytes_rcvd = 164;
    }
    is_int(bytes_sent, entry->bytes_sent, "%s BulkCall bytes_sent", descr);
    is_int(bytes_rcvd, entry->bytes_rcvd, "%s BulkCall bytes_rcvd", descr);

    rx_DestroyConnection(rxconn);
}

int
main(int argc, char *argv[])
{
    int code;

    setprogname(argv[0]);

    fprintf(stderr, "# Running rpcs for a few seconds...\n");

    afstest_ForkRxProc(start_server, NULL);
    afstest_ForkRxProc(start_client, NULL);

    plan(106);

    code = rx_Init(0);
    if (code != 0) {
	bail("rx_Init returned %d", code);
    }

    /* Now that we've run some RPCs, check the stats on the server. */
    test_stats(0);

    /* ...and now check the stats on the client. */
    test_stats(1);

    return 0;
}

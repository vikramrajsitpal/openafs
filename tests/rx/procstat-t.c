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
#include "test.h"

static u_short client_port = 1234;

static int
start_client(void *rock)
{
    int code;
    struct rx_connection *rxconn;
    struct rx_call *rxcall;
    int res_int;
    char *res_str = NULL;
    afs_int32 writebuf[] = {
	12, 34, 5678, 9101112, 131415,
    };
    afs_int32 readbuf[5];

    memset(readbuf, 0, sizeof(readbuf));

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

    /*
     * Run some RPCs. Note that we cannot use TAP functions like is_int() here,
     * because the actual TAP tests are run in the parent process. So just
     * assert everything is okay.
     */
    opr_Verify(TEST_SleepMS(rxconn, 1100) == 0);
    opr_Verify(TEST_SleepMS(rxconn, 2100) == 0);

    opr_Verify(TEST_CheckOdd(rxconn, 1) == 0);
    opr_Verify(TEST_CheckOdd(rxconn, 2) == TEST_CHECKODD_NOTODD);

    opr_Verify(TEST_Sum(rxconn, 3, 4, &res_int) == 0);
    opr_Assert(res_int == 7);

    opr_Verify(TEST_Concat(rxconn, "foo", "bar", &res_str) == 0);
    opr_Assert(strcmp(res_str, "foobar") == 0);

    opr_StaticAssert(sizeof(writebuf) == sizeof(readbuf));

    rxcall = rx_NewCall(rxconn);
    opr_Verify(StartTEST_Echo(rxcall) == 0);
    opr_Verify(rx_Write(rxcall, (void*)writebuf, sizeof(writebuf)) == sizeof(writebuf));
    opr_Verify(rx_Read(rxcall, (void*)readbuf, sizeof(readbuf)) == sizeof(readbuf));
    opr_Verify(EndTEST_Echo(rxcall) == 0);
    opr_Verify(rx_EndCall(rxcall, 0) == 0);

    opr_Verify(memcmp(writebuf, readbuf, sizeof(writebuf)) == 0);

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
    is_int(3, entry->execution_time_sum.sec, "%s SleepMS exec time", descr);
    is_int(1, entry->execution_time_min.sec, "%s SleepMS exec time min", descr);
    is_int(2, entry->execution_time_max.sec, "%s SleepMS exec time max", descr);

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
    is_int(0, entry->execution_time_sum.sec, "%s CheckOdd exec time", descr);
    is_int(0, entry->execution_time_min.sec, "%s CheckOdd exec time min", descr);
    is_int(0, entry->execution_time_max.sec, "%s CheckOdd exec time max", descr);

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
    is_int(1, entry->invocations, "%s Sum invocations", descr);
    is_int(0, entry->queue_time_sum.sec, "%s Sum queue time", descr);
    is_int(0, entry->queue_time_min.sec, "%s Sum queue time min", descr);
    is_int(0, entry->queue_time_max.sec, "%s Sum queue time max", descr);
    is_int(0, entry->execution_time_sum.sec, "%s Sum exec time", descr);
    is_int(0, entry->execution_time_min.sec, "%s Sum exec time min", descr);
    is_int(0, entry->execution_time_max.sec, "%s Sum exec time max", descr);
    if (client) {
	bytes_sent = 12;
	bytes_rcvd = 4;
    } else {
	bytes_sent = 4;
	bytes_rcvd = 12;
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
    is_int(0, entry->execution_time_sum.sec, "%s Concat exec time", descr);
    is_int(0, entry->execution_time_min.sec, "%s Concat exec time min", descr);
    is_int(0, entry->execution_time_max.sec, "%s Concat exec time max", descr);

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
    is_int(1, entry->invocations, "%s Echo invocations", descr);
    is_int(0, entry->queue_time_sum.sec, "%s Echo queue time", descr);
    is_int(0, entry->queue_time_min.sec, "%s Echo queue time min", descr);
    is_int(0, entry->queue_time_max.sec, "%s Echo queue time max", descr);
    is_int(0, entry->execution_time_sum.sec, "%s Echo exec time", descr);
    is_int(0, entry->execution_time_min.sec, "%s Echo exec time min", descr);
    is_int(0, entry->execution_time_max.sec, "%s Echo exec time max", descr);

    if (client) {
	bytes_sent = 24;
	bytes_rcvd = 20;
    } else {
	bytes_sent = 20;
	bytes_rcvd = 24;
    }
    is_int(bytes_sent, entry->bytes_sent, "%s Echo bytes_sent", descr);
    is_int(bytes_rcvd, entry->bytes_rcvd, "%s Echo bytes_rcvd", descr);

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

    plan(102);

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

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

#include <rx/rx.h>
#include "test.h"
#include "common.h"

afs_int32
STEST_SleepMS(struct rx_call *rxcall, int ms)
{
    if (usleep(ms * 1000) < 0) {
	return errno;
    }
    return 0;
}

afs_int32
STEST_CheckOdd(struct rx_call *rxcall, int val)
{
    if (val % 2 == 1) {
	return 0;
    }
    return TEST_CHECKODD_NOTODD;
}

afs_int32
STEST_CheckOddSingle(struct rx_call *rxcall, int val)
{
    return STEST_CheckOdd(rxcall, val);
}

afs_int32
STEST_Sum(struct rx_call *rxcall, int x, int y, int *result)
{
    *result = x + y;
    return 0;
}

afs_int32
STEST_Concat(struct rx_call *rxcall, char *foo, char *bar, char **a_foobar)
{
    size_t len = strlen(foo) + strlen(bar) + 1;

    *a_foobar = osi_Alloc(len);
    if (*a_foobar == NULL) {
	return ENOMEM;
    }

    memset(*a_foobar, 0, len);
    snprintf(*a_foobar, len, "%s%s", foo, bar);
    return 0;
}

afs_int32
STEST_Echo(struct rx_call *rxcall)
{
#define MAX_VALS 64
    afs_int32 valbuf[MAX_VALS];
    afs_int32 val;
    int val_i = 0;
    int n_vals;

    while (rx_Read32(rxcall, &val) == sizeof(val)) {
	if (val_i >= MAX_VALS) {
	    return E2BIG;
	}
	valbuf[val_i] = val;
	val_i++;
    }

    n_vals = val_i;
    for (val_i = 0; val_i < n_vals; val_i++) {
	if (rx_Write32(rxcall, &valbuf[val_i]) != sizeof(valbuf[val_i])) {
	    return RX_PROTOCOL_ERROR;
	}
    }

    return 0;
}

/*
 * This ridiculous function interprets the raw array of integers in 'rpcStats',
 * and converts them into more useful structs with fields. See
 * rx_MarshallProcessRPCStats() for how this data is encoded, and
 * UnmarshallRPCStats() (in libadmin) as another example of decoding this data.
 */
void
teststats_unmarshal(rpcStats *raw_stats, struct proc_stats *a_stats)
{
    static const int VALS_PER_ENTRY = 28;
    int val_i;
    int entry_i;
    int n_entries;
    XDR xdrs;
    struct rxstat_entry *entries = NULL;

    if (raw_stats->rpcStats_len % VALS_PER_ENTRY != 0) {
	bail("got %d stats, which is not divisible by %d",
	     raw_stats->rpcStats_len, VALS_PER_ENTRY);
    }

    n_entries = raw_stats->rpcStats_len / VALS_PER_ENTRY;

    entries = bcalloc(n_entries, sizeof(*entries));

    /* Put the integer values back in NBO, so we can process them with xdr. */
    for (val_i = 0; val_i < raw_stats->rpcStats_len; val_i++) {
	raw_stats->rpcStats_val[val_i] = htonl(raw_stats->rpcStats_val[val_i]);
    }

    xdrmem_create(&xdrs, (void*)raw_stats->rpcStats_val,
		  raw_stats->rpcStats_len * sizeof(raw_stats->rpcStats_val[0]),
		  XDR_DECODE);

    for (entry_i = 0; entry_i < n_entries; entry_i++) {
	if (!xdr_rxstat_entry(&xdrs, &entries[entry_i])) {
	    bail("xdr_rxstat_entry failed");
	}
    }

    a_stats->n_entries = n_entries;
    a_stats->entries = entries;
}

struct rxstat_entry *
teststats_find(struct proc_stats *stats, afs_uint64 opcode)
{
    int entry_i;
    afs_uint32 statindex;
    afs_uint32 func_index;

    statindex = opcode >> 32;
    func_index = opcode & 0xFFFFFFFF;

    for (entry_i = 0; entry_i < stats->n_entries; entry_i++) {
	struct rxstat_entry *entry = &stats->entries[entry_i];
	if (entry->interfaceId == statindex &&
	    entry->func_index == func_index) {
	    return entry;
	}
    }
    bail("Cannot find stats for op 0x%llu", opcode);
    return NULL;
}


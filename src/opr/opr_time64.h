/*
 * Copyright (c) 2012 Your File System Inc. All rights reserved.
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
 * This header provides routines for dealing with the 100ns based AFS time
 * type, afs_time64. With this type, time is represented in units of 100ns,
 * sometimes called "clunks". Absolute time is the same as with unix time (time
 * since unix epoch, skipping leap seconds), except represented in clunks
 * instead of seconds. The count of clunks is always recorded in a signed
 * 64-bit integer.
 *
 * The actual variable is hidden behind a structure, so that accidental
 * assignment like this will fail during compilation:
 *
 *     time_t ourTime;
 *     afs_time64 theirTime;
 *
 *     ourTime = theirTime;
 *
 * But any callers can still easily access the underlying raw int64 by just
 * looking at "theirTime.clunks". The name of the internal field is a bit
 * obnoxious, which mildly discourages callers from interacting with the raw
 * value.
 */

#ifndef OPENAFS_OPR_TIME64_H
#define OPENAFS_OPR_TIME64_H

#include <afs/opr.h>
#ifdef KERNEL
# include "afs/sysincludes.h"
# include "afsincludes.h"
#else
# include <roken.h>
#endif

#define OPR_TIME64_CLUNKS_PER_US    (10LL)
#define OPR_TIME64_CLUNKS_PER_MS    (OPR_TIME64_CLUNKS_PER_US * 1000LL)
#define OPR_TIME64_CLUNKS_PER_SEC   (OPR_TIME64_CLUNKS_PER_MS * 1000LL)

#define OPR_TIME64_SECS_MAX (922337203685LL)
#define OPR_TIME64_SECS_MIN (-922337203685LL)

static_inline int
opr_time64_fromSecs(afs_int64 in, struct afs_time64 *out)
{
    if (in < OPR_TIME64_SECS_MIN || in > OPR_TIME64_SECS_MAX) {
	return ERANGE;
    }

    out->clunks = in * OPR_TIME64_CLUNKS_PER_SEC;
    return 0;
}

static_inline int
opr_time64_fromTimeval(struct timeval *in, struct afs_time64 *out)
{
    int code;

    code = opr_time64_fromSecs(in->tv_sec, out);
    if (code != 0) {
	return code;
    }

    out->clunks += in->tv_usec * OPR_TIME64_CLUNKS_PER_US;
    return 0;
}

static_inline afs_int64
opr_time64_toSecs(struct afs_time64 *in)
{
    return in->clunks / OPR_TIME64_CLUNKS_PER_SEC;
}

static_inline int
opr_time64_cmp(struct afs_time64 *t1, struct afs_time64 *t2)
{
    if (t1->clunks > t2->clunks) {
	return 1;
    }
    if (t1->clunks < t2->clunks) {
	return -1;
    }
    return 0;
}

static_inline int
opr_time64_now(struct afs_time64 *out)
{
    struct timeval tv;
    memset(&tv, 0, sizeof(tv));

#ifdef KERNEL
    {
	osi_timeval32_t tv32;
	memset(&tv32, 0, sizeof(tv32));
	osi_GetTime(&tv32);
	tv.tv_sec = tv32.tv_sec;
	tv.tv_usec = tv32.tv_usec;
    }
#else
    if (gettimeofday(&tv, NULL) != 0) {
	return EIO;
    }
#endif

    return opr_time64_fromTimeval(&tv, out);
}

#endif /* OPENAFS_OPR_TIME64_H */

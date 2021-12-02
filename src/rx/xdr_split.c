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
#include "xdr.h"

static struct xdr_ops xdrsplit_ops;

/**
 * Create an xdrsplit instance.
 *
 * An 'xdrsplit' instance redirects the xdr read calls to one xdr object, and
 * xdr write calls to another object. This allows, for example, a caller to
 * create a single xdr object where reads come from one xdrmem buffer, and
 * writes go to another xdrmem buffer. Or a caller could let xdr reads come
 * from an rx call (xdrrx), but writes get buffered (xdrmem, xdrbuf).
 *
 * A count of how many bytes have been successfully read and written is also
 * recorded in splinfo->read_bytes and splinfo->wrote_bytes.
 *
 * @param[out] xdrs	The xdrsplit instance.
 * @param[in] splinfo	Specifies the underlying xdr streams, and records the
 *			bytes read and written.
 * @param[in] op	The direction the xdr stream starts in.
 */
void
xdrsplit_create(XDR * xdrs, struct xdrsplit_info *splinfo, enum xdr_op op)
{
    memset(xdrs, 0, sizeof(*xdrs));

    xdrs->x_op = op;
    xdrs->x_ops = &xdrsplit_ops;

    xdrs->x_private = (void*)splinfo;
}

static bool_t
xdrsplit_getint32(XDR *xdrs, afs_int32 *val)
{
    struct xdrsplit_info *splinfo = (void*)xdrs->x_private;
    int success = xdr_getint32(splinfo->reader, val);
    if (success) {
	splinfo->read_bytes += sizeof(*val);
    }
    return success;
}

static bool_t
xdrsplit_putint32(XDR *xdrs, afs_int32 *val)
{
    struct xdrsplit_info *splinfo = (void*)xdrs->x_private;
    int success = xdr_putint32(splinfo->writer, val);
    if (success) {
	splinfo->wrote_bytes += sizeof(*val);
    }
    return success;
}

static bool_t
xdrsplit_getbytes(XDR *xdrs, caddr_t addr, u_int len)
{
    struct xdrsplit_info *splinfo = (void*)xdrs->x_private;
    int success = xdr_getbytes(splinfo->reader, addr, len);
    if (success) {
	splinfo->read_bytes += len;
    }
    return success;
}

static bool_t
xdrsplit_putbytes(XDR *xdrs, caddr_t addr, u_int len)
{
    struct xdrsplit_info *splinfo = (void*)xdrs->x_private;
    int success = xdr_putbytes(splinfo->writer, addr, len);
    if (success) {
	splinfo->wrote_bytes += len;
    }
    return success;
}

static bool_t
xdrsplit_setpos(XDR *xdrs, u_int pos)
{
    /* Not implemented */
    return FALSE;
}

static afs_int32 *
xdrsplit_inline(XDR *xdrs, u_int len)
{
    /* Not implemented */
    return NULL;
}

static void
xdrsplit_destroy(XDR *xdrs)
{
    /*
     * The caller should xdr_destroy the underlying xdr streams on their own,
     * so don't xdr_destroy xread and xwrite here, to avoid double-freeing
     * something.
     */
}

static struct xdr_ops xdrsplit_ops = {
#ifndef HAVE_STRUCT_LABEL_SUPPORT
    xdrsplit_getint32,
    xdrsplit_putint32,
    xdrsplit_getbytes,
    xdrsplit_putbytes,
    NULL, /* getpos not implemented */
    xdrsplit_setpos,
    xdrsplit_inline,
    xdrsplit_destroy,
#else
    .x_getint32 = xdrsplit_getint32,
    .x_putint32 = xdrsplit_putint32,
    .x_getbytes = xdrsplit_getbytes,
    .x_putbytes = xdrsplit_putbytes,
    .x_getpostn = NULL, /* getpost not implemented */
    .x_setpostn = xdrsplit_setpos,
    .x_inline = xdrsplit_inline,
    .x_destroy = xdrsplit_destroy
#endif
};

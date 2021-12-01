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
#include <afs/opr.h>
#include "xdr.h"
#include <rx/rx_opaque.h>

/*
 * xdrbuf - Similar to xdrmem with XDR_ENCODE, but we dynamically build our
 * buffer of encoded data, instead of using a prebuilt buffer.
 */

static struct xdr_ops xdrbuf_ops;

/* Set out default max buffer to 16 MiB. Raise as needed; we just don't use
 * very large buffers for now. */
#define XDRBUF_MAXLEN_DEFAULT (1024*1024 * 16)

/* Alloc memory in 4k chunks. */
#define XDRBUF_ALLOC_BLOCK 4096

struct xbuf {
    struct rx_opaque buf;
    size_t remaining;
    void *next_addr;
};

/**
 * Create an xdrbuf instance.
 *
 * xdrbuf will record written xdr data in an internal buffer, which grows as
 * needed (up to the given 'maxlen' limit, if any). The data can be retrieved
 * afterwrads with xdrbuf_getbuf(). You must xdr_destroy() the xdrbuf instance
 * to free the internal buffer.
 *
 * Note that xdrbuf only works in XDR_ENCODE mode.
 *
 * @param[out] xdrs The xdr handle.
 * @param[in] maxlen	The maximum length of data we'll encode. If 0, use the
 *			default max (currently 16MiB). If negative, there is no
 *			max (the dynamic buffer can grow unbounded). If the
 *			limit is exceeded, the xdr encoding routine will return
 *			an error.
 */
void
xdrbuf_create(XDR * xdrs, int maxlen)
{
    memset(xdrs, 0, sizeof(*xdrs));

    if (maxlen == 0) {
	/* Specifying 0 means to use the default max. */
	maxlen = XDRBUF_MAXLEN_DEFAULT;
    }
    if (maxlen < 0) {
	/* Specifying a negative number means "no max". */
	maxlen = 0;
    }

    xdrs->x_op = XDR_ENCODE;
    xdrs->x_ops = &xdrbuf_ops;

    /* Stores our rx_opaque */
    xdrs->x_private = NULL;

    xdrs->x_handy = maxlen;
}

/**
 * Get the dynamic buffer of encoded data.
 *
 * Note that this buffer may change and get reallocated as more data is written
 * to the stream. The returned buffer is only guaranteed to be valid until the
 * stream is written to again (or is xdr_destroy()'d).
 *
 * @param[in] xdrs  The xdr handle.
 * @param[out] buf  Set to the buffer of data on return. This can be a 0-length
 *		    buffer if no data has been written yet.
 *
 * @retval EINVAL   This xdr handle is not an xdrbuf instance.
 */
int
xdrbuf_getbuf(XDR *xdrs, struct rx_opaque *buf)
{
    struct xbuf *xbuf = (void*)xdrs->x_private;

    if (xdrs->x_ops != &xdrbuf_ops) {
	return EINVAL;
    }

    if (xbuf == NULL) {
	memset(buf, 0, sizeof(*buf));
	return 0;
    }

    *buf = xbuf->buf;
    buf->len -= xbuf->remaining;

    return 0;
}

/**
 * Reset the internal buffer.
 *
 * This is similar to running xdr_destroy and xdrbuf_create, but reuses the
 * existing internal buffer without freeing it.
 */
void
xdrbuf_reset(XDR *xdrs)
{
    struct xbuf *xbuf = (void*)xdrs->x_private;

    if (xbuf == NULL) {
	return;
    }

    memset(xbuf->buf.val, 0, xbuf->buf.len);
    xbuf->remaining = xbuf->buf.len;
    xbuf->next_addr = xbuf->buf.val;
}

static void
xdrbuf_destroy(XDR *xdrs)
{
    struct xbuf *xbuf = (void*)xdrs->x_private;

    opr_Assert(xdrs->x_ops == &xdrbuf_ops);

    if (xbuf == NULL) {
	return;
    }

    rx_opaque_freeContents(&xbuf->buf);
    free(xbuf);

    memset(xdrs, 0, sizeof(*xdrs));
    xdrs->x_ops = &xdrbuf_ops;
}

static bool_t
xdrbuf_putbytes(XDR *xdrs, caddr_t addr, u_int len)
{
    struct xbuf *xbuf = (void*)xdrs->x_private;
    int max_len = xdrs->x_handy;

    if (xbuf == NULL) {
	xbuf = calloc(1, sizeof(*xbuf));
    }
    xdrs->x_private = (void*)xbuf;
    if (xbuf == NULL) {
	return FALSE;
    }

    if (max_len > 0 && len > max_len - (xbuf->buf.len - xbuf->remaining)) {
	/* Make sure this putbytes isn't going to put us over our limit (if we
	 * have one). */
	return FALSE;
    }

    if (len > xbuf->remaining) {
	/* We don't have enough space; must alloc more memory. */
	size_t need_bytes = len - xbuf->remaining;
	size_t alloc_bytes;
	int code;

	/* Round up to the next XDRBUF_ALLOC_BLOCK */
	alloc_bytes = ((need_bytes - 1) / XDRBUF_ALLOC_BLOCK) + 1;
	alloc_bytes *= XDRBUF_ALLOC_BLOCK;

	code = rx_opaque_realloc(&xbuf->buf, xbuf->buf.len + alloc_bytes);
	if (code != 0) {
	    return FALSE;
	}

	xbuf->remaining += alloc_bytes;
	xbuf->next_addr = (char*)xbuf->buf.val + xbuf->buf.len - xbuf->remaining;
	
	opr_Assert(xbuf->remaining <= xbuf->buf.len);
	opr_Assert(xbuf->remaining >= len);
    }


    memcpy(xbuf->next_addr, addr, len);

    xbuf->next_addr = (char*)xbuf->next_addr + len;
    xbuf->remaining -= len;

    return (TRUE);
}

static bool_t
xdrbuf_putint32(XDR *xdrs, afs_int32 *val)
{
    afs_int32 val_net = htonl(*val);
    return xdrbuf_putbytes(xdrs, (void*)&val_net, sizeof(val_net));
}

static u_int
xdrbuf_getpos(XDR *xdrs)
{
    struct xbuf *xbuf = (void*)xdrs->x_private;

    if (xbuf == NULL) {
	return 0;
    }

    return xbuf->buf.len - xbuf->remaining;
}

static bool_t
xdrbuf_getint32(XDR *xdrs, afs_int32 *val)
{
    return FALSE;
}

static bool_t
xdrbuf_getbytes(XDR *xdrs, caddr_t addr, u_int len)
{
    return FALSE;
}

static bool_t
xdrbuf_setpos(XDR *xdrs, u_int pos)
{
    return FALSE;
}

static afs_int32 *
xdrbuf_inline(XDR *xdrs, u_int len)
{
    return NULL;
}

static struct xdr_ops xdrbuf_ops = {
#ifndef HAVE_STRUCT_LABEL_SUPPORT
    xdrbuf_getint32,
    xdrbuf_putint32,
    xdrbuf_getbytes,
    xdrbuf_putbytes,
    xdrbuf_getpos,
    xdrbuf_setpos,
    xdrbuf_inline,
    xdrbuf_destroy,
#else
    .x_getint32 = xdrbuf_getint32,
    .x_putint32 = xdrbuf_putint32,
    .x_getbytes = xdrbuf_getbytes,
    .x_putbytes = xdrbuf_putbytes,
    .x_getpostn = xdrbuf_getpos,
    .x_setpostn = xdrbuf_setpos,
    .x_inline = xdrbuf_inline,
    .x_destroy = xdrbuf_destroy
#endif
};

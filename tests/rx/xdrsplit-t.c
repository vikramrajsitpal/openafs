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

#include <rx/xdr.h>
#include "common.h"

int
main(void)
{
    XDR xdrs;
    XDR xread;
    XDR xwrite;
    int intval = 0;
    char *strval = NULL;
    struct rx_opaque exp, got;
    struct xdrsplit_info splinfo;

    char readbuf[] = "\x00\x00\x00\x2a\x00\x00\x00\x03\x66\x6f\x6f\x00";
    char writebuf[8];

    memset(&splinfo, 0, sizeof(splinfo));

    plan(19);

    xdrmem_create(&xread, readbuf, sizeof(readbuf), XDR_DECODE);
    xdrmem_create(&xwrite, writebuf, sizeof(writebuf), XDR_ENCODE);

    splinfo.reader = &xread;
    splinfo.writer = &xwrite;
    xdrsplit_create(&xdrs, &splinfo, XDR_DECODE);

    ok(xdr_int(&xdrs, &intval), "read xdr_int");
    is_int(0x2a, intval, "int value matches");
    is_int(4, splinfo.read_bytes, "read_bytes");

    ok(xdr_string(&xdrs, &strval, 50), "read xdr_string");
    is_string("foo", strval, "string value matches");
    is_int(12, splinfo.read_bytes, "read_bytes");

    ok(!xdr_int(&xdrs, &intval), "read xdr_int fails");
    is_int(12, splinfo.read_bytes, "read_bytes");
    is_int(0, splinfo.wrote_bytes, "wrote_bytes");

    xdrs.x_op = XDR_ENCODE;

    intval = 0x36;
    ok(xdr_int(&xdrs, &intval), "write xdr_int");
    is_int(4, splinfo.wrote_bytes, "wrote_bytes");

    intval = 0x67;
    ok(xdr_int(&xdrs, &intval), "xdr_int [write]");
    is_int(8, splinfo.wrote_bytes, "wrote_bytes");

    intval = 0;
    ok(!xdr_int(&xdrs, &intval), "xdr_int fails [write]");
    is_int(12, splinfo.read_bytes, "read_bytes");
    is_int(8, splinfo.wrote_bytes, "wrote_bytes");

    exp.val = "\x00\x00\x00\x36\x00\x00\x00\x67";
    exp.len = 8;
    got.val = writebuf;
    got.len = xdr_getpos(&xwrite);

    is_opaque(&exp, &got, "write buffer matches");

    ok(!xdr_setpos(&xdrs, 0), "xdr_setpos fails");
    ok(xdr_inline(&xdrs, 1) == NULL, "xdr_inline fails");

    xdr_destroy(&xdrs);

    return 0;
}

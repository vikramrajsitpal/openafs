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

#include <rx/rx_opaque.h>
#include <ubik.h>
#include "common.h"

#define check_opaque(xdrs, val, descr) \
	do_check_opaque(xdrs, val, sizeof(val)-1, descr)

static void
do_check_opaque(XDR *xdrs, void *val, size_t len, const char *str)
{
    struct rx_opaque exp = {.val = val, .len = len};
    struct rx_opaque got;
    xdrbuf_getbuf(xdrs, &got);
    is_opaque(&exp, &got, "%s", str);
}

int
main(void)
{
    XDR xdrs;
    XDR xmem;
    int intval = 42;
    char *strval = "foo";
    int i;

    static char membuf[16384];

    plan(31);

    xdrbuf_create(&xdrs, 0);

    ok(xdr_int(&xdrs, &intval), "xdr_int");
    ok(xdr_string(&xdrs, &strval, 10), "xdr_string");
    is_int(12, xdr_getpos(&xdrs), "xdr_getpos");

    check_opaque(&xdrs, "\x00\x00\x00\x2a\x00\x00\x00\x03\x66\x6f\x6f\x00",
		 "simple int,string");

    xdrmem_create(&xmem, membuf, sizeof(membuf), XDR_ENCODE);
    ok(xdr_int(&xmem, &intval), "xdr_int[mem]");
    ok(xdr_string(&xmem, &strval, 10), "xdr_string [mem]");
    do_check_opaque(&xdrs, membuf, xdr_getpos(&xmem), "xdrmem results match");

    xdrbuf_reset(&xdrs);

    check_opaque(&xdrs, "", "blank buf post-reset");

    ok(xdr_int(&xdrs, &intval), "xdr_int");
    ok(xdr_string(&xdrs, &strval, 10), "xdr_string");
    is_int(12, xdr_getpos(&xdrs), "xdr_getpos");

    check_opaque(&xdrs, "\x00\x00\x00\x2a\x00\x00\x00\x03\x66\x6f\x6f\x00",
		 "simple int,string post-reset");

    xdr_destroy(&xdrs);
    /* Make sure destroy'ing twice is ok. */
    xdr_destroy(&xdrs);

    check_opaque(&xdrs, "", "blank buf post-destroy");

    xdrbuf_create(&xdrs, 5);
    ok(xdr_int(&xdrs, &intval), "xdr_int");
    ok(!xdr_int(&xdrs, &intval), "xdr_int over limit");

    check_opaque(&xdrs, "\x00\x00\x00\x2a", "xdr_int after limit check");

    xdr_destroy(&xdrs);

    xdrbuf_create(&xdrs, -1);
    xdrmem_create(&xmem, membuf, sizeof(membuf), XDR_ENCODE);

    /* Encode 4 ubik_debug structs, to make sure we encode over 4k of data. */

    for (i = 0; i < 4; i++) {
	struct ubik_debug udebug;
	memset(&udebug, 0, sizeof(udebug));
	/* Just set some arbitrary fields in the middle of the struct. */
	udebug.lastYesTime = i+1;
	udebug.interfaceAddr[0] = i*52;

	ok(xdr_ubik_debug(&xdrs, &udebug), "xdr_ubik_debug [%d]", i);
	ok(xdr_ubik_debug(&xmem, &udebug), "xdr_ubik_debug [%d, mem]", i);
    }

    is_int(4544, xdr_getpos(&xdrs), "ubik_debug getpos");
    is_int(4544, xdr_getpos(&xmem), "ubik_debug getpos [mem]");

    do_check_opaque(&xdrs, membuf, xdr_getpos(&xmem), "ubik_debug xdrmem results match");

    ok(!xdr_getint32(&xdrs, &intval), "xdr_getint32 fails");
    ok(!xdr_getbytes(&xdrs, (void*)&intval, sizeof(intval)), "xdr_getbytes fails");
    ok(!xdr_setpos(&xdrs, 0), "xdr_setpos fails");
    ok(xdr_inline(&xdrs, 1) == NULL, "xdr_inline fails");

    xdr_destroy(&xdrs);

    return 0;
}

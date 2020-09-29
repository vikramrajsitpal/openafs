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
#include "common.h"

int
main(void)
{
    struct rx_opaque *buf;
    struct rx_opaque buf_s = RX_EMPTY_OPAQUE;
    struct rx_opaque copy = RX_EMPTY_OPAQUE;
    struct rx_opaque exp = RX_EMPTY_OPAQUE;
    struct rx_opaque_stringbuf strbuf;
    int code;

    plan(21);

    buf_s.val = "foo\0bar";
    buf_s.len = 7;
    copy.val = "foo\0bar";
    copy.len = 7;
    is_int(0, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == 0");

    copy.len = 8;
    is_int(-1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == -1 (length)");

    copy.len = 6;
    is_int(1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == 1 (length)");

    copy.val = "zoo\0bar";
    copy.len = 7;
    is_int(-1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == -1 (data)");
    copy.len = 6;
    is_int(-1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == -1 (data, shorter)");
    copy.len = 8;
    is_int(-1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == -1 (data, longer)");

    copy.len = 0;
    is_int(1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == 1 (data vs 0-len)");

    copy.val = "boo\0bar";
    copy.len = 7;
    is_int(1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == 1 (data)");
    copy.len = 6;
    is_int(1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == 1 (data, shorter)");
    copy.len = 8;
    is_int(1, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == 1 (data, longer)");

    buf_s.len = 0;
    copy.len = 0;
    is_int(0, rx_opaque_cmp(&buf_s, &copy), "rx_opaque_cmp == 0 (0-len vs 0-len)");


    buf = rx_opaque_new("some\0data", 10);
    opr_Assert(buf != NULL);
    exp.val = "some\0data";
    exp.len = 10;
    is_opaque(&exp, buf, "rx_opaque_new");

    rx_opaque_free(&buf);
    is_pointer(NULL, buf, "rx_opaque_free");

    code = rx_opaque_alloc(&buf_s, 5);
    opr_Assert(code == 0);
    exp.val = "\0\0\0\0\0";
    exp.len = 5;
    is_opaque(&exp, &buf_s, "rx_opaque_alloc");

    rx_opaque_freeContents(&buf_s);
    is_pointer(NULL, buf_s.val, "rx_opaque_freeContents(val)");
    is_int(0, buf_s.len, "rx_opaque_freeContents(len)");

    code = rx_opaque_populate(&buf_s, "some\0data", 10);
    opr_Assert(code == 0);
    exp.val = "some\0data";
    exp.len = 10;
    is_opaque(&exp, &buf_s, "rx_opaque_populate");

    rx_opaque_copy(&copy, &buf_s);
    rx_opaque_zeroFreeContents(&buf_s);
    is_opaque(&exp, &copy, "rx_opaque_copy");
    is_pointer(NULL, buf_s.val, "rx_opaque_zeroFreeContents(val)");
    is_int(0, buf_s.len, "rx_opaque_zeroFreeContents(len)");

    is_string("10:736f6d65006461746100", rx_opaque_stringify(&copy, &strbuf),
	      "rx_opaque_stringify");
    rx_opaque_freeContents(&copy);

    return 0;
}

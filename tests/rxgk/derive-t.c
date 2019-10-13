/* Test the tk derivation code inside rxgk */

#include <afsconfig.h>
#include <afs/param.h>
#include <afs/stds.h>

#include <errno.h>
#include <rx/rx.h>
#include <rx/rxgk.h>
#include <afs/rfc3961.h>
#include <afs/opr.h>

#include <tests/tap/basic.h>
#include <assert.h>

#include "common.h"

struct tcase {
    char *descr;

    struct rx_opaque k0_raw;
    struct rx_opaque tk_raw;

    afs_uint32 enctype;

    afs_uint32 epoch;
    afs_uint32 cid;
    rxgkTime start_time;
    afs_uint32 key_number;
};

#define DERIVE_TEST(a_descr, in, out, a_epoch, a_cid, a_start_time, a_key_number) \
    { \
	.descr = (a_descr), \
	.k0_raw = { \
	    .len = sizeof(in) - 1, \
	    .val = (in), \
	}, \
	.tk_raw = { \
	    .len = sizeof(out) - 1, \
	    .val = (out), \
	}, \
	.enctype = ETYPE_AES128_CTS_HMAC_SHA1_96, \
	.epoch = (a_epoch), \
	.cid = (a_cid), \
	.start_time = (a_start_time), \
	.key_number = (a_key_number), \
    }

static struct tcase derive_tests[] = {
    DERIVE_TEST("key_number 1",
		"1234567890123456",
		"\x61\x8b\xbb\xaa\x4c\xb8\xd9\x82\xb3\x09\x7c\x67\x95\x40\x40\x9f",
		1571007429, 0x760a9c24, 15710085940000001LL, 1),

    DERIVE_TEST("key_number 2",
		"1234567890123456",
		"\x95\xb6\x9f\xce\xc9\x4e\x44\x7c\x87\x3d\x7a\x38\xf3\x9f\x76\x60",
		1571007429, 0x760a9c24, 15710085940000002LL, 2),

    DERIVE_TEST("different k0",
		"\xde\xad\xbe\xef\xba\xdd\xca\xfe\xd0\xd0\xca\xca\xde\xad\xc0\xde",
		"\xac\x4b\x3f\x65\x3b\x40\xec\x5b\xd5\x2c\xe9\xdd\x9d\x7c\x3e\xf7",
		1571007429, 0x760a9c24, 15710085940000003LL, 1),

    DERIVE_TEST("start_time 0, key_number 9999",
		"\xde\xad\xbe\xef\xba\xdd\xca\xfe\xd0\xd0\xca\xca\xde\xad\xc0\xde",
		"\x82\x6a\x16\xd5\x94\xf9\x2f\xca\x7c\x43\x4f\xf1\xe7\x35\xe2\x81",
		1571000000, 0xdeadb33f, 0, 9999),
    {0}
};

static void
key2data(rxgk_key key, struct rx_opaque *data)
{
    struct key_impl {
	krb5_context ctx;
	krb5_keyblock key;
    };
    krb5_keyblock *keyblock = &((struct key_impl *)key)->key;
    data->len = keyblock->keyvalue.length;
    data->val = keyblock->keyvalue.data;
}

int
main(void)
{
    struct tcase *test;
    int code;

    plan(12);

    for (test = derive_tests; test->k0_raw.val != NULL; test++) {
	rxgk_key k0 = NULL;
	rxgk_key tk = NULL;
	struct rx_opaque keydata;

	memset(&keydata, 0, sizeof(keydata));

	code = rxgk_make_key(&k0, test->k0_raw.val, test->k0_raw.len,
			     test->enctype);
	is_int(0, code, "[%s] rxgk_make_key succeeds", test->descr);

	code = rxgk_derive_tk(&tk, k0, test->epoch, test->cid,
			      test->start_time, test->key_number);
	is_int(0, code, "[%s] rxgk_derive_tk succeeds", test->descr);

	key2data(tk, &keydata);
	is_opaque(&test->tk_raw, &keydata, "[%s] key matches", test->descr);
    }

    return 0;
}

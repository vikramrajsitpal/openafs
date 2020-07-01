#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>

#include <tests/tap/basic.h>
#include <afs/okv.h>
#include <afs/opr.h>

#include <pthread.h>

#include "common.h"

/* Needed for checking dbh->dbh_disk */
#include "okv_internal.h"

#ifndef MIN
# define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static char *prefix;
static char *dbdir;

static struct config_data {
    char *descr;

    /* Engine name to specify */
    char *engine_arg;

    /* Expected resulting engine name */
    char *engine_res;

    /* Expected error code */
    int error;
} configs[] = {
    {
       .descr = "defaults",
       .engine_arg = NULL,
       .engine_res = "lmdb",
    },
    {
       .descr = "lmdb",
       .engine_arg = "lmdb",
       .engine_res = "lmdb",
    },
    {
       .descr = "invalid engine",
       .engine_arg = "invalid",
       .error = EINVAL,
    },
    {0}
};


struct kv_data {
    struct rx_opaque key;
    struct rx_opaque value;
    int seen;
};

#define KV_ITEM(keystr, valstr) { \
	.key = { \
	    .len = sizeof(keystr)-1, \
	    .val = keystr \
	}, \
	.value = { \
	    .len = sizeof(valstr)-1, \
	    .val = valstr, \
	}, \
	.seen = 0 \
    }

static struct kv_data prefill_items[] = {
    KV_ITEM("key1", "early value"),
    KV_ITEM("\x00\x00", "foo"),
    {{0}}
};

static struct kv_data data_items[] = {
    KV_ITEM("key1", "value2"),
    KV_ITEM("\x00\x00", "two nulls"),
    KV_ITEM("\x00\x00\x00", "\x00"),
    {
	.key = {
	    .len = sizeof("nullval") - 1,
	    .val = "nullval",
	},
	.value = {
	    .len = 0,
	    .val = NULL,
	},
	.seen = 0
    },

    /* 16 lines, each line is 16 bytes; plus one; total 257 */
    KV_ITEM(
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee"
	    ,
	    "value for big key"),

    KV_ITEM("key for big value",
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee\xee"
	    "\xee"
	    ),
    {{0}}
};

static struct kv_data extra_items[] = {
    KV_ITEM("extra1", "extra value"),
    KV_ITEM("foo", "bar"),
    {{0}}
};
#undef KV_DATA

static int
buf_cmp(struct rx_opaque *buf_a, struct rx_opaque *buf_b, int target_diff)
{
    int diff = rx_opaque_cmp(buf_a, buf_b);
    if (diff != target_diff) {
	struct rx_opaque_stringbuf sbuf;
	diag(" left buf: %s", rx_opaque_stringify(buf_a, &sbuf));
	diag("right buf: %s", rx_opaque_stringify(buf_b, &sbuf));
    }
    return diff;
}

static void *
populate_data_part2(void *rock)
{
    struct okv_trans *tx = rock;
    struct kv_data *item;
    int code;

    for (item = data_items; item->key.val != NULL; item++) {
	code = okv_put(tx, &item->key, &item->value, OKV_PUT_REPLACE);
	is_int(0, code, "okv_put returns success");
    }

    for (item = data_items; item->key.val != NULL; item++) {
	code = okv_put(tx, &item->key, &item->value, 0);
	is_int(EEXIST, code, "duplicate okv_put failed with EEXIST");
    }

    code = okv_commit(&tx);
    is_int(0, code, "okv_commit returns success");

    return NULL;
}

static void
populate_data(struct okv_dbhandle *dbh, int threaded)
{
    struct okv_trans *tx = NULL;
    struct kv_data *item;
    int flags = 0;
    int code;

    if (threaded) {
	flags = OKV_BEGIN_XTHREAD;
    }

    code = okv_begin(dbh, OKV_BEGIN_RO | flags, &tx);
    is_int(0, code, "okv_begin (RO) returns success");
    if (code != 0) {
	goto done;
    }

    for (item = prefill_items; item->key.val != NULL; item++) {
	code = okv_put(tx, &item->key, &item->value, 0);
	is_int(EACCES, code, "okv_put (RO) fails with EACCES");
    }

    okv_abort(&tx);

    code = okv_begin(dbh, OKV_BEGIN_RW | flags, &tx);
    is_int(0, code, "okv_begin (RW) returns success");
    if (code != 0) {
	goto done;
    }

    for (item = prefill_items; item->key.val != NULL; item++) {
	code = okv_put(tx, &item->key, &item->value, 0);
	is_int(0, code, "okv_put (prefill) returns success");
    }

    if (threaded) {
	/* Do some operations in another thread. */
	pthread_t tid;
	opr_Verify(pthread_create(&tid, NULL, populate_data_part2, tx) == 0);
	opr_Verify(pthread_join(tid, NULL) == 0);
    } else {
	populate_data_part2(tx);
    }
    tx = NULL;

    code = okv_begin(dbh, OKV_BEGIN_RW | flags, &tx);
    is_int(0, code, "okv_begin (again) returns success");
    if (code != 0) {
	goto done;
    }

    /* Run okv_put on these keys, but abort the tx instead of committing it, so
     * the data should never actually be valid in the db. */
    for (item = extra_items; item->key.val != NULL; item++) {
	code = okv_put(tx, &item->key, &item->value, 0);
	is_int(0, code, "okv_put (extra) returns success");
    }

 done:
    okv_abort(&tx);
}

static void *
do_begin(void *rock)
{
    struct okv_dbhandle *dbh = rock;
    struct okv_trans *tx = NULL;
    int code;

    code = okv_begin(dbh, OKV_BEGIN_RW | OKV_BEGIN_XTHREAD, &tx);
    is_int(0, code, "okv_begin returns success");
    if (code != 0) {
	return NULL;
    }
    return tx;
}

static void
check_all_items(struct okv_trans *tx)
{
    int code;
    int n_unseen = 0;
    struct rx_opaque key;
    struct rx_opaque value;
    struct rx_opaque prev_key;
    struct kv_data *item;

    memset(&prev_key, 0, sizeof(prev_key));
    memset(&key, 0, sizeof(key));
    memset(&value, 0, sizeof(value));

    for (;;) {
	struct kv_data *found_item = NULL;
	int eof = 0;
	int diff;

	code = okv_next(tx, &key, &value, &eof);
	is_int(0, code, "okv_next returns success");
	if (code != 0) {
	    break;
	}
	if (eof) {
	    break;
	}

	if (prev_key.val != NULL) {
	    is_int(-1, buf_cmp(&prev_key, &key, -1),
		   "okv_next returns key in order");
	    rx_opaque_freeContents(&prev_key);
	}
	opr_Verify(rx_opaque_copy(&prev_key, &key) == 0);

	/* Find which item our 'key' corresponds to. */
	for (item = data_items; item->key.val != NULL; item++) {
	    if (rx_opaque_cmp(&item->key, &key) == 0) {
		found_item = item;
		break;
	    }
	}
	ok(found_item != NULL, "okv_next gives us a known key");
	if (found_item == NULL) {
	    continue;
	}

	is_int(0, found_item->seen, "okv_next item not already seen");

	diff = buf_cmp(&found_item->value, &value, 0);
	is_int(0, diff, "okv_next gives us the right value");
	if (diff == 0) {
	    found_item->seen = 1;
	}
    }

    for (item = data_items; item->key.val != NULL; item++) {
	if (!item->seen) {
	    struct rx_opaque_stringbuf sbuf;
	    diag("key %s not seen", rx_opaque_stringify(&item->key, &sbuf));
	    n_unseen++;
	}
    }
    is_int(0, n_unseen, "okv_next found all keys");

    rx_opaque_freeContents(&prev_key);
}

static void
check_dbh(struct okv_dbhandle *dbh, int threaded)
{
    struct okv_trans *tx = NULL;
    struct kv_data *item;
    struct rx_opaque key;
    struct rx_opaque value;
    struct okv_statinfo st;
    int noent;
    int code;

    memset(&key, 0, sizeof(key));
    memset(&value, 0, sizeof(value));
    memset(&st, 0, sizeof(st));

    if (threaded) {
	/* Do the okv_begin call in another thread. */
	pthread_t tid;
	void *retval = NULL;
	opr_Verify(pthread_create(&tid, NULL, do_begin, dbh) == 0);
	opr_Verify(pthread_join(tid, &retval) == 0);
	tx = retval;
	if (tx == NULL) {
	    goto done;
	}

    } else {
	code = okv_begin(dbh, OKV_BEGIN_RO, &tx);
	is_int(0, code, "okv_begin returns success");
	if (code != 0) {
	    goto done;
	}
    }

    /* Check that okv_get returns the right data for everything in
     * 'data_items'. */
    for (item = data_items; item->key.val != NULL; item++) {
	/* Check that we do the right thing both when noent is NULL, and
	 * non-NULL. */

	code = okv_get(tx, &item->key, &value, NULL);
	is_int(0, code, "okv_get returns success");
	is_int(0, buf_cmp(&item->value, &value, 0),
	       "okv_get returns correct value");

	noent = 0;
	code = okv_get(tx, &item->key, &value, &noent);
	is_int(0, code, "okv_get returns success");
	ok(!noent, "noent has correct value");
	is_int(0, buf_cmp(&item->value, &value, 0),
	       "okv_get returns correct value");
    }

    {
	char val_str[7];

	memset(val_str, 0, sizeof(val_str));

	key.val = "key1";
	key.len = 4;
	code = okv_get_copy(tx, &key, val_str, sizeof(val_str)-1, NULL);
	is_int(0, code, "okv_get_copy success");
	is_string("value2", val_str, "okv_get_copy returns correct value");
	memset(val_str, 0, sizeof(val_str));

	code = okv_get_copy(tx, &key, val_str, sizeof(val_str), NULL);
	is_int(EIO, code, "okv_get_copy (wrong size) EIO");

	code = okv_get_copy(tx, &key, val_str, sizeof(val_str)-1, &noent);
	is_int(0, code, "okv_get_copy (noent) success");
	is_string("value2", val_str,
		  "okv_get_copy (noent) returns correct value");
	is_int(0, noent, "okv_get_copy (noent) gives correct noent");

	key.val = "key2";
	code = okv_get_copy(tx, &key, val_str, sizeof(val_str)-1, NULL);
	is_int(ENOENT, code, "okv_get_copy ENOENT");

	code = okv_get_copy(tx, &key, val_str, sizeof(val_str)-1, &noent);
	is_int(0, code, "okv_get_copy (noent) success");
	is_int(1, noent, "okv_get_copy (noent) gives correct noent");
    }

    /* Check that stuff in extra_items is _NOT_ in the db. */

    for (item = extra_items; item->key.val != NULL; item++) {

	code = okv_get(tx, &item->key, &value, NULL);
	is_int(ENOENT, code, "okv_get (extra) returns ENOENT");

	noent = 0;
	code = okv_get(tx, &item->key, &value, &noent);
	is_int(0, code, "okv_get (noent) returns success");
	ok(noent, "okv_get (extra) gives correct noent");
    }

    /* Now go through the entire db using okv_next, and verify that it gives
     * the right data (and only the right data; no extra stuff. */

    for (item = data_items; item->key.val != NULL; item++) {
	item->seen = 0;
    }

    code = okv_stat(tx, &st);
    is_int(0, code, "okv_stat returns success");
    ok(st.os_entries != NULL, "okv_stat returns os_entries");
    is_int(6, (st.os_entries != NULL ? *st.os_entries : -1),
	   "os_entries has the correct number");
    memset(&st, 0, sizeof(st));

    check_all_items(tx);

    /* Now try deleting some items, and verify the db still looks as it should. */
    okv_abort(&tx);
    code = okv_begin(dbh, OKV_BEGIN_RW, &tx);
    is_int(0, code, "okv_begin returns success");
    if (code != 0) {
	goto done;
    }

    /* Try deleting something that shouldn't exist. */
    key = extra_items[0].key;
    code = okv_del(tx, &key, NULL);
    is_int(ENOENT, code, "okv_del(NULL) returns ENOENT");

    noent = 0;
    code = okv_del(tx, &key, &noent);
    is_int(0, code, "okv_del(&noent) returns success");
    ok(noent, "okv_del indicates noent");

    /* Now actually delete something. */
    key = data_items[2].key;
    code = okv_del(tx, &key, NULL);
    is_int(0, code, "okv_del[2] returns success");
    is_int(0, buf_cmp(&data_items[2].key, &key, 0),
       "okv_del[2] key unchanged");

    /* Delete the first item in the db. */
    noent = 0;
    key = data_items[1].key;
    code = okv_del(tx, &key, &noent);
    is_int(0, code, "okv_del[1] returns success");
    ok(!noent, "okv_del[1] indicates non-noent");
    is_int(0, buf_cmp(&data_items[1].key, &key, 0),
       "okv_del[1] key unchanged");

    /*
     * Reset our 'seen' values, and set the items as just deleted as 'seen', to
     * check that we don't see them when iterating through all items. Then
     * check that we still see everything else.
     */
    for (item = data_items; item->key.val != NULL; item++) {
	item->seen = 0;
    }
    data_items[1].seen = 1;
    data_items[2].seen = 1;

    code = okv_stat(tx, &st);
    is_int(0, code, "okv_stat returns success (again)");
    ok(st.os_entries != NULL, "okv_stat returns os_entries (again)");
    is_int(4, (st.os_entries != NULL ? *st.os_entries : -1),
	   "os_entries has the correct number (again)");

    check_all_items(tx);

 done:
    okv_abort(&tx);
}

static void
check_db_path(char *dir_path, int threaded)
{
    struct okv_dbhandle *dbh_1 = NULL;
    struct okv_dbhandle *dbh_2 = NULL;
    int code;

    code = okv_open(dir_path, &dbh_1);
    is_int(0, code, "okv_open(1) returns success");
    if (code != 0) {
	goto done;
    }

    code = okv_dbhandle_setflags(dbh_1, OKV_DBH_NOSYNC, 1);
    is_int(0, code, "okv_dbhandle_setflags(OKV_DBH_NOSYNC, 1) success");

    code = okv_dbhandle_setflags(dbh_1, OKV_DBH_NOSYNC, 0);
    is_int(0, code, "okv_dbhandle_setflags(OKV_DBH_NOSYNC, 0) success");

    code = okv_dbhandle_setflags(dbh_1, OKV_DBH_FLAGMASK + 1, 1);
    is_int(EINVAL, code, "okv_dbhandle_setflags(badflag, 1) returns EINVAL");

    code = okv_dbhandle_setflags(dbh_1, OKV_DBH_FLAGMASK + 1, 0);
    is_int(EINVAL, code, "okv_dbhandle_setflags(badflag, 0) returns EINVAL");

    check_dbh(dbh_1, threaded);

    code = okv_open(dir_path, &dbh_2);
    is_int(0, code, "okv_open(2) returns success");
    if (code != 0) {
	goto done;
    }

    check_dbh(dbh_2, threaded);

    ok(dbh_1->dbh_disk == dbh_2->dbh_disk,
       "dbh_1 and dbh_2 use the same okv_disk");

 done:
    okv_close(&dbh_1);
    okv_close(&dbh_2);
}

static void
test_newdb(struct config_data *cdata, int threaded)
{
    char *descr;
    char *copy_dir = NULL;
    struct okv_dbhandle *dbh = NULL;
    struct okv_dbhandle *dummy_db = NULL;
    struct okv_dbhandle *copy_dbh = NULL;
    struct okv_create_opts c_opts;
    int code;

    memset(&c_opts, 0, sizeof(c_opts));

    diag("Testing newly created db (%s)%s", cdata->descr,
	 (threaded ? " [threads]" : ""));

    c_opts.engine = cdata->engine_arg;

    code = okv_create(dbdir, &c_opts, &dbh);
    is_int(cdata->error, code, "okv_create returns %d", cdata->error);
    if (code != 0) {
	goto done;
    }

    is_string(cdata->engine_res, okv_dbhandle_engine(dbh),
	      "okv_create gives expected engine name");

    descr = okv_dbhandle_descr(dbh);
    ok(descr != NULL, "okv_dbhandle_descr works (%s)", descr);

    code = okv_create(dbdir, &c_opts, &dummy_db);
    ok(code != 0, "duplicate okv_create failed");
    if (code == 0) {
	goto done;
    }

    populate_data(dbh, threaded);

    okv_close(&dbh);
    ok(dbh == NULL, "okv_close NULLs arg");

    okv_close(&dbh);
    ok(1, "duplicate okv_close is okay");

    check_db_path(dbdir, threaded);

    if (getenv("OKV_GENERATE_DB") != NULL) {
	diag("db is in %s", dbdir);
	diag("Not running further tests; exiting now.");
	exit(1);
    }

    code = okv_open(dbdir, &dbh);
    is_int(0, code, "okv_open (reopen) succeeds");

    copy_dir = afstest_asprintf("%s.copy", dbdir);

    diag("Testing copy db, %s", copy_dir);

    code = okv_create(copy_dir, NULL, &copy_dbh);
    is_int(0, code, "okv_create (copy) succeeds");

    code = okv_copyall(dbh, copy_dbh);
    is_int(0, code, "okv_copyall succeeds");

    check_db_path(copy_dir, threaded);

 done:
    okv_close(&dbh);
    okv_close(&dummy_db);
    okv_close(&copy_dbh);

    code = okv_unlink(dbdir);
    is_int(0, code, "okv_unlink returns success");
    /* assert here, since if the unlink fails, all other tests are going to
     * screw up, too */
    opr_Assert(code == 0);

    if (copy_dir != NULL) {
	code = okv_unlink(copy_dir);
	is_int(0, code, "okv_unlink (copy) returns success");
	opr_Assert(code == 0);

	free(copy_dir);
    }
}

static void
test_existing(void)
{
    char *name = "tests/okv/smalldb.lmdb";
    char *dir_path = NULL;
    int code;

    code = afstest_okv_dbpath(&dir_path, name);
    is_int(0, code, "%s exists", name);
    if (code != 0) {
	goto done;
    }

    if (dir_path == NULL) {
	skip_block(398, "%s does not exist for this platform", name);
	goto done;
    }

    diag("Checking data from existing db %s", name);
    check_db_path(dir_path, 0);
    check_db_path(dir_path, 1);

 done:
    free(dir_path);
}

int
main(void)
{
    struct config_data *conf;
    char *null_descr;
    int code;
    struct okv_dbhandle *dbh = NULL;

    plan(2471);

    prefix = afstest_mkdtemp();
    opr_Assert(prefix != NULL);

    dbdir = afstest_asprintf("%s/dbase", prefix);

    null_descr = okv_dbhandle_descr(NULL);
    ok(null_descr != NULL,
       "okv_dbhandle_descr(NULL) returns non-NULL (%s)", null_descr);

    ok(okv_dbhandle_engine(NULL) == NULL,
       "okv_dbhandle_engine(NULL) returns NULL");

    code = okv_open(dbdir, &dbh);
    is_int(ENOENT, code, "okv_open fails with ENOENT");

    code = mkdir(dbdir, 0700);
    opr_Assert(code == 0);

    code = okv_create(dbdir, NULL, &dbh);
    ok(code != 0, "okv_create on existing dir failed");

    okv_close(&dbh);

    code = rmdir(dbdir);
    opr_Assert(code == 0);

    for (conf = &configs[0]; conf->descr != NULL; conf++) {
	test_newdb(conf, 0);
	test_newdb(conf, 1);
    }

    test_existing();

    afstest_rmdtemp(prefix);

    return 0;
}

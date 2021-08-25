/*
 * Copyright (c) 2010 Your File System Inc. All rights reserved.
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

/*!
 * Common functions for building a configuration directory
 */

#include <afsconfig.h>
#include <afs/param.h>
#include <roken.h>

#include <afs/cellconfig.h>

#include <hcrypto/des.h>

#include <tests/tap/basic.h>
#include "common.h"

static FILE *
openConfigFile(char *dirname, char *filename) {
    char *path = NULL;
    FILE *file;

    path = afstest_asprintf("%s/%s", dirname, filename);

    file = fopen(path, "w");
    free(path);
    return file;
}

static struct sockaddr *
storage2sa(struct sockaddr_storage *storage)
{
    return (struct sockaddr *)storage;
}

/*!
 * Build a test configuration directory, containing a CellServDB and ThisCell
 * file for the "example.org" cell. Also populates the KeyFile unless
 * info->skipkeys is set.
 *
 * @param[in] info  Various details for how to create the config dir. If NULL,
 *		    use a default zeroed struct.
 * @return
 * The path to the configuration directory. This should be freed by the caller
 * using free()
 */
char *
afstest_BuildTestConfig(struct afstest_configinfo *info)
{
    char *dir = NULL;
    FILE *file;
    struct afsconf_dir *confdir = NULL;
    struct afstest_configinfo info_defaults;
    struct sockaddr_storage default_addr;
    struct sockaddr_storage *dbserver_addrs = NULL;
    int n_addrs;
    int addr_i;
    int code;

    memset(&info_defaults, 0, sizeof(info_defaults));
    memset(&default_addr, 0, sizeof(default_addr));

    if (info == NULL) {
	info = &info_defaults;
    }

    dir = afstest_mkdtemp();
    if (dir == NULL) {
	goto error;
    }

    if (info->dbserver_addrs == NULL) {
	struct sockaddr_in *sin = (struct sockaddr_in *)&default_addr;
	/*
	 * Work out which IP address to use in our CellServDB. We figure this out
	 * according to the IP address which ubik is most likely to pick for one of
	 * our db servers
	 */
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = afstest_MyHostAddr();
	dbserver_addrs = &default_addr;
	n_addrs = 1;

    } else {
	/* Use the given dbserver IPs. */
	dbserver_addrs = info->dbserver_addrs;
	n_addrs = info->dbserver_addrs_len;
    }

    file = openConfigFile(dir, "CellServDB");
    fprintf(file, ">example.org # An example cell\n");
    for (addr_i = 0; addr_i < n_addrs; addr_i++) {
	char hoststr[64];

	code = getnameinfo(storage2sa(&dbserver_addrs[addr_i]),
			   sizeof(dbserver_addrs[addr_i]),
			   hoststr, sizeof(hoststr), NULL, 0,
			   NI_NUMERICHOST);
	if (code != 0) {
	    bail("getnameinfo returned %d", code);
	}

	fprintf(file, "%s #test%d.example.org\n", hoststr, addr_i);
    }
    fclose(file);

    /* Create a ThisCell file */
    file = openConfigFile(dir, "ThisCell");
    fprintf(file, "example.org");
    fclose(file);

    if (!info->skipkeys) {
	confdir = afsconf_Open(dir);
	if (confdir == NULL) {
	    goto error;
	}

	code = afstest_AddDESKeyFile(confdir);
	if (code != 0) {
	    goto error;
	}

	afsconf_Close(confdir);
	confdir = NULL;
    }

    if (info->force_host != NULL) {
	file = openConfigFile(dir, "NetInfo");
	fprintf(file, "f %s\n", info->force_host);
	fclose(file);
    }

    return dir;

 error:
    if (confdir != NULL) {
	afsconf_Close(confdir);
    }
    if (dir != NULL) {
	afstest_rmdtemp(dir);
	free(dir);
    }
    return NULL;
}

int
afstest_AddDESKeyFile(struct afsconf_dir *dir)
{
    char keymaterial[]="\x19\x17\xff\xe6\xbb\x77\x2e\xfc";

    /* Make sure that it is actually a valid key */
    DES_set_odd_parity((DES_cblock *)keymaterial);

    return afsconf_AddKey(dir, 1, keymaterial, 1);
}

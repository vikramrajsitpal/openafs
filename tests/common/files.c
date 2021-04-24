/*
 * Copyright (c) 2020 Sine Nomine Associates. All rights reserved.
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
 * Common file-related functions for test programs
 */

#include <afsconfig.h>
#include <afs/param.h>
#include <roken.h>

#include <afs/opr.h>
#include <afs/afsutil.h>

#include <tests/tap/basic.h>

#include "common.h"

char *
afstest_mkdtemp(void)
{
    char *template;
    char *tmp;

    tmp = afstest_obj_path("tests/tmp");

    /* Try to make sure 'tmp' exists. */
    (void)mkdir(tmp, 0700);

    template = afstest_asprintf("%s/afs_XXXXXX", tmp);

    free(tmp);

#if defined(HAVE_MKDTEMP)
    return mkdtemp(template);
#else
    /*
     * Note that using the following is not a robust replacement
     * for mkdtemp as there is a possible race condition between
     * creating the name and creating the directory itself.  The
     * use of this routine is limited to running tests.
     */
    if (mktemp(template) == NULL)
	return NULL;
    if (mkdir(template, 0700))
	return NULL;
    return template;
#endif
}

void
afstest_rmdtemp(char *path)
{
    int code;
    struct stat st;

    /* Sanity check, only zap directories that look like ours */
    opr_Assert(strstr(path, "afs_") != NULL);
    if (getenv("MAKECHECK") == NULL) {
	/* Don't delete tmp dirs if we're not running under 'make check'. */
	return;
    }
    code = lstat(path, &st);
    if (code != 0) {
	/* Given path doesn't exist (or we can't access it?) */
	return;
    }
    if (!S_ISDIR(st.st_mode)) {
	/* Path isn't a dir; that's weird. Bail out to be on the safe side. */
	return;
    }
    afstest_systemlp("rm", "-rf",
#if defined(AFS_LINUX_ENV) || defined(AFS_SUN511_ENV)
		     "--one-file-system",
#endif
		     path, (char*)NULL);
}

static char *
path_from_tdir(char *env_var, char *filename)
{
    char *tdir;
    char *path;
    char *top_rel, *top_abs;

    /* C_TAP_SOURCE/C_TAP_BUILD in the env points to 'tests/' in the
     * srcdir/objdir. */

    tdir = getenv(env_var);
    if (tdir == NULL) {
	/*
	 * If C_TAP_SOURCE/C_TAP_BUILD isn't set, we assume we're running from
	 * the same cwd as one of the test programs (e.g. 'tests/foo/'). So to
	 * get to 'tests/', just go up one level.
	 */
	tdir = "..";
    }

    /* 'tdir' points to the 'tests' dir, so go up one level to get to the top
     * srcdir/objdir. */
    top_rel = afstest_asprintf("%s/..", tdir);
    top_abs = realpath(top_rel, NULL);
    free(top_rel);
    if (top_abs == NULL) {
	sysbail("realpath");
    }

    /*
     * The given 'filename' is relative to the top srcdir/objdir, so to get the
     * full path, append 'filename' to the top srcdir/objdir. Note that the
     * given path may not exist yet, so we cannot run the full path through
     * realpath().
     */
    path = afstest_asprintf("%s/%s", top_abs, filename);
    free(top_abs);

    return path;
}

char *
afstest_src_path(char *path)
{
    return path_from_tdir("C_TAP_SOURCE", path);
}

char *
afstest_obj_path(char *path)
{
    return path_from_tdir("C_TAP_BUILD", path);
}

/**
 * Check if the given file contains a string. To keep things simple, the string
 * to look for must appear on a single line, and must appear in the first 128
 * bytes of that line.
 *
 * @param[in] path  Path to the file to check.
 * @param[in] target	The string to look for in 'path'.
 *
 * @retval 1 The file contains the given string.
 * @retval 0 The file does not contain the given string (or we encountered an
 *           error).
 */
int
afstest_file_contains(char *path, char *target)
{
    FILE *fh;
    char line[128];
    int found = 0;

    fh = fopen(path, "r");
    if (fh == NULL) {
	diag("%s: Failed to open %s", __func__, path);
	goto done;
    }

    while (fgets(line, sizeof(line), fh) != NULL) {
	if (strstr(line, target) != NULL) {
	    found = 1;
	    goto done;
	}
    }

 done:
    if (fh != NULL) {
	fclose(fh);
    }
    return found;
}

int
afstest_cp(char *src_path, char *dest_path)
{
    int code;

    /* Instead of needing to reimplement the logic for copying a file, just run
     * 'cp'. */
    code = afstest_systemlp("cp", src_path, dest_path, (char*)NULL);
    if (code != 0) {
	return EIO;
    }
    return 0;
}

/*
 * Return 1 if the two given files are equal; 0 otherwise. Ignore the first
 * 'start_off' bytes of the contents of each file.
 */
int
afstest_file_equal(char *path_a, char *path_b, off_t start_off)
{
    int equal = 0;
    FILE *fh_a = NULL, *fh_b = NULL;

    fh_a = fopen(path_a, "r");
    if (fh_a == NULL) {
	sysdiag("Failed to open %s", path_a);
	goto done;
    }
    fh_b = fopen(path_b, "r");
    if (fh_b == NULL) {
	sysdiag("Failed to open %s", path_b);
	goto done;
    }

    if (fseek(fh_a, start_off, SEEK_SET) < 0) {
	sysdiag("fseek(%s)", path_a);
	goto done;
    }
    if (fseek(fh_b, start_off, SEEK_SET) < 0) {
	sysdiag("fseek(%s)", path_b);
	goto done;
    }

    for (;;) {
	int cur_a, cur_b;
	int eof_a, eof_b;

	cur_a = fgetc(fh_a);
	cur_b = fgetc(fh_b);

	eof_a = feof(fh_a) ? 1 : 0;
	eof_b = feof(fh_b) ? 1 : 0;

	if (eof_a && eof_b) {
	    break;
	}

	if (ferror(fh_a)) {
	    diag("Failed to read data from %s", path_a);
	    goto done;
	}
	if (ferror(fh_b)) {
	    diag("Failed to read data from %s", path_b);
	    goto done;
	}

	if (eof_a) {
	    diag("hit eof for %s @ 0x%lx", path_a, ftell(fh_a));
	    goto done;
	}
	if (eof_b) {
	    diag("hit eof for %s @ 0x%lx", path_b, ftell(fh_b));
	    goto done;
	}

	if (cur_a != cur_b) {
	    diag("contents mismatch @ 0x%lx: 0x%x != 0x%x (%s != %s)",
		 ftell(fh_a)-1, cur_a, cur_b, path_a, path_b);
	    goto done;
	}
    }

    equal = 1;

 done:
    if (fh_a != NULL) {
	fclose(fh_a);
    }
    if (fh_b != NULL) {
	fclose(fh_b);
    }
    return equal;
}

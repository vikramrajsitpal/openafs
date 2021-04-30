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

#include <roken.h>
#include <afs/opr.h>
#include <pthread.h>

/* opr_progress - Routines to manage showing progress to the user for
 * (potentially) long-running operations. */

struct opr_progress {
    volatile int need_update;

    afs_int64 cur_val;

    int bkg_running;
    int spin_i;
    pthread_t bkg_tid;

    char *descr;
    struct opr_progress_opts opts;
};

static void
prog_vprintf(struct opr_progress *prog, const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
}

static void
AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3)
prog_printf(struct opr_progress *prog, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    prog_vprintf(prog, fmt, ap);
    va_end(ap);
}

static void
prog_fflush(struct opr_progress *prog)
{
    fflush(stderr);
}

static void
bkg_sleep(struct opr_progress *prog, int delay_ms)
{
    usleep(delay_ms * 1000);

    if (prog->opts.bkg_spinner) {
	static const char spinchars[] = { '|', '/', '-', '\\' };
	char spinc;
	int state;

	prog->spin_i = (prog->spin_i + 1) % sizeof(spinchars);
	spinc = spinchars[prog->spin_i];

	opr_Verify(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state) == 0);
	prog_printf(prog, "%s... %c\r", prog->descr, spinc);
	prog_fflush(prog);
	opr_Verify(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &state) == 0);

    } else {
	prog->need_update = 1;
    }
}

static void *
bkg_thread(void *rock)
{
    struct opr_progress *prog = rock;
    int delay = prog->opts.delay_ms;
    int interval = prog->opts.interval_ms;

    /* By default: after 1 second, update our progress every 250ms. */
    if (delay == 0) {
	delay = 1000;
    }
    if (interval == 0) {
	interval = 250;
	if (prog->opts.bkg_spinner) {
	    /* For the background spinner mode, do updates every 500ms. */
	    interval = 500;
	}
    }

    bkg_sleep(prog, delay);
    for (;;) {
	bkg_sleep(prog, interval);
    }

    return NULL;
}

static void
bkg_stop(struct opr_progress *prog)
{
    if (prog->bkg_running) {
	opr_Verify(pthread_cancel(prog->bkg_tid) == 0);
	opr_Verify(pthread_join(prog->bkg_tid, NULL) == 0);
	prog->bkg_running = 0;
    }
}

static void
progress_free(struct opr_progress **a_prog)
{
    struct opr_progress *prog = *a_prog;
    if (prog == NULL) {
	return;
    }
    *a_prog = NULL;

    bkg_stop(prog);
    free(prog->descr);
    free(prog);
}

static void
show_progress(struct opr_progress *prog)
{
    afs_int64 max_val = prog->opts.max_val;
    afs_int64 cur_val = prog->cur_val;

    if (max_val == 0) {
	prog_printf(prog, "%s... (%lld)", prog->descr, cur_val);

    } else {
	int pct;
	if (cur_val >= max_val) {
	    pct = 100;
	} else {
	    pct = cur_val * 100 / max_val;
	}
	prog_printf(prog, "%s... %3u%% (%lld / %lld)", prog->descr, pct,
		   cur_val, max_val);
    }
}

/**
 * Start reporting progress.
 *
 * @param[in] opts  Various options
 * @param[in] fmt   Message to print to indicate what progress is being
 *		    reported. Takes normal printf formatting codes and
 *		    arguments. For example: "Reticulating splines" will
 *		    eventually print something like:
 *		    "Reticulating splines...  34% (34/100)"
 * @return progress context
 * @retval NULL If the given opts say to stay completely quiet. NULL can still
 *              be given to other opr_progress functions to perform no-ops.
 */
struct opr_progress *
opr_progress_start(struct opr_progress_opts *opts, const char *fmt, ...)
{
    static struct opr_progress_opts defaults;
    struct opr_progress *prog = NULL;
    va_list ap;

    if (opts == NULL) {
	opts = &defaults;
    }

    if (opts->quiet) {
	return NULL;
    }

    prog = calloc(1, sizeof(*prog));
    opr_Assert(prog != NULL);

    prog->opts = *opts;
    prog->cur_val = opts->start_val;

    /* If force_disable is set, it overrides force_enable. If neither is set,
     * choose our default based on isatty(). */
    if (!prog->opts.force_disable && !prog->opts.force_enable) {
	prog->opts.force_disable = !isatty(fileno(stderr));
    }
    prog->opts.force_enable = !prog->opts.force_disable;

    va_start(ap, fmt);
    if (prog->opts.force_enable) {
	opr_Verify(vasprintf(&prog->descr, fmt, ap) >= 0);
	prog_printf(prog, "%s... \r", prog->descr);

    } else {
	prog_vprintf(prog, fmt, ap);
	prog_printf(prog, "... ");
    }
    va_end(ap);

    prog_fflush(prog);

    if (prog->opts.force_enable) {
	opr_Verify(pthread_create(&prog->bkg_tid, NULL, bkg_thread, prog) == 0);
	prog->bkg_running = 1;
    }

    return prog;
}

/**
 * Indicate absolute progress.
 *
 * @param[in] prog  Progress ctx
 * @param[in] val   Progress amount is set to this value
 */
void
opr_progress_set(struct opr_progress *prog, afs_int64 val)
{
    if (prog == NULL || prog->opts.force_disable) {
	return;
    }

    prog->cur_val = val;

    if (!prog->need_update) {
	return;
    }
    prog->need_update = 0;

    show_progress(prog);
    prog_printf(prog, "\r");
    prog_fflush(prog);
}

/**
 * Indicate incremental progress.
 *
 * @param[in] prog  Progress ctx
 * @param[in] amt   Amount of additional progress to indicate
 */
void
opr_progress_add(struct opr_progress *prog, afs_int64 amt)
{
    if (prog == NULL) {
	return;
    }

    opr_progress_set(prog, prog->cur_val + amt);
}

/**
 * Free opr_progress, and print "done" message.
 *
 * @param[inout] a_prog	Progress ctx to end and free. Set to NULL on return.
 * @param[in] error	Error code of operation. If non-zero, just a single
 *			newline is printed instead of a "done" message (to help
 *			error messages be seen clearly).
 */
void
opr_progress_done(struct opr_progress **a_prog, int error)
{
    afs_int64 cur_val;
    struct opr_progress *prog = *a_prog;

    if (prog == NULL) {
	return;
    }
    *a_prog = NULL;

    if (error != 0) {
	prog_printf(prog, "\n");
	goto done;
    }

    if (prog->opts.force_disable) {
	prog_printf(prog, "done.\n");
	goto done;
    }

    bkg_stop(prog);

    cur_val = prog->cur_val;

    if (prog->opts.bkg_spinner || cur_val == 0) {
	prog_printf(prog, "%s... done.\n", prog->descr);

    } else {
	if (error == 0 && prog->opts.max_val != 0) {
	    /* We're "done" without error, so say we're at 100% completion
	     * instead of printing partial progress. */
	    prog->opts.max_val = cur_val;
	}
	show_progress(prog);
	prog_printf(prog, ", done.\n");
    }
    prog_fflush(prog);

 done:
    progress_free(&prog);
}

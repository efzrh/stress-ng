/*
 * Copyright (C)      2024 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-killpid.h"

static const stress_help_t help[] = {
	{ NULL,	"sighup N",	 "start N workers generating SIGHUP signals" },
	{ NULL,	"sighup-ops N", "stop after N bogo SIGHUP operations" },
	{ NULL,	NULL,		 NULL }
};

typedef struct {
	volatile bool signalled;	/* True if handler handled SIGHUP */
	volatile pid_t pid;
	volatile double count;
	volatile double t_start;
	volatile double latency;
} stress_sighup_info_t;

static stress_sighup_info_t *sighup_info;

static void MLOCKED_TEXT stress_sighup_handler(int num)
{
	(void)num;

	if (sighup_info) { /* Should always be not null */
		double latency = stress_time_now() - sighup_info->t_start;

		sighup_info->signalled = true;
		if ((sighup_info->t_start > 0.0) && (latency > 0.0)) {
			sighup_info->latency += latency;
			sighup_info->count += 1.0;
		}
	}
}

static int stress_sighup_raise_signal(stress_args_t *args)
{
	pid_t pid;
	int ret, status;

again:
	pid = fork();
	if (pid < 0) {
		if (stress_redo_fork(args, errno))
			goto again;
		if (!stress_continue(args))
			return 0;
		pr_fail("%s: fork failed: %d (%s)\n",
			args->name, errno, strerror(errno));
		return -1;
	} else if (pid == 0) {
		VOID_RET(int, stress_sighandler(args->name, SIGHUP, stress_sighup_handler, NULL));

		/* Raising SIGHUP without an handler will abort */
		sighup_info->t_start = stress_time_now();
		shim_raise(SIGHUP);
		_exit(0);
	}
rewait:
	ret = shim_waitpid(pid, &status, 0);
	if (ret < 0) {
		if (errno == EINTR)
			goto rewait;
		pr_fail("%s: waitpid failed: %d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_FAILURE;
	} else {
		if (sighup_info->signalled == false) {
			pr_fail("%s SIGHUP signal handler did not get called\n",
				args->name);
			return EXIT_FAILURE;
		}
	}
	return 0;
}

static void stress_sighup_closefds(int fds[2])
{
	(void)close(fds[0]);
	(void)close(fds[1]);
}

static int stress_sighup_process_group(stress_args_t *args)
{
	pid_t pid;
	int ret, status;
	char msg = 'x';

	VOID_RET(int, stress_sighandler(args->name, SIGHUP, stress_sighup_handler, NULL));

	sighup_info->pid = 0;
again:
	pid = fork();
	if (pid < 0) {
		if (stress_redo_fork(args, errno))
			goto again;
		if (!stress_continue(args))
			return 0;
		pr_fail("%s: fork failed: %d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_FAILURE;
	} else if (pid == 0) {
		pid_t pid2;
		int fds[2];

		if (pipe(fds) < 0)
			return 0;

		sighup_info->t_start = 0.0;

		pid2 = fork();
		if (pid2 < 0) {
			stress_sighup_closefds(fds);
			return 0;
		} else if (pid2 == 0) {
			sighup_info->pid = getpid();
			if (write(fds[1], &msg, 1) < 1)
				_exit(0);
			(void)kill(getpid(), SIGSTOP);
			stress_sighup_closefds(fds);
			_exit(0);
		} else {
			sighup_info->pid = pid2;
			setpgid(pid2, 0);
			/* Wait for child to stop itself */
			if (read(fds[0], &msg, 0) < 1) {
				/*
				 * Parent kills itself and kernel delivers
				 * SIGHUP to child
				 */
				sighup_info->t_start = stress_time_now();
				(void)kill(getpid(), SIGKILL);
			}
			stress_sighup_closefds(fds);
			_exit(0);
		}
	}
rewait:
	ret = shim_waitpid(pid, &status, 0);
	if (ret < 0) {
		if (errno == EINTR)
			goto rewait;
		pr_fail("%s: waitpid failed: %d (%s)\n",
			args->name, errno, strerror(errno));
		if (sighup_info->pid != 0)
			(void)stress_kill_pid_wait(sighup_info->pid, &status);
		return EXIT_FAILURE;
	}
	if (sighup_info->pid != 0)
		(void)stress_kill_pid_wait(sighup_info->pid, &status);
	return 0;
}

/*
 *  stress_sighup
 *	stress by generating segmentation faults by
 *	writing to a read only page
 */
static int stress_sighup(stress_args_t *args)
{
	double rate;
	int rc = EXIT_SUCCESS;

	if (stress_sighandler(args->name, SIGHUP, stress_sighup_handler, NULL) < 0)
		return EXIT_NO_RESOURCE;

	sighup_info = (stress_sighup_info_t *)mmap(NULL, sizeof(*sighup_info),
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_ANONYMOUS,
				-1, 0);
	if (sighup_info == MAP_FAILED) {
		pr_inf_skip("%s: failed to mmap sighup information, "
			"errno=%d (%s), skipping stressor\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}
	stress_set_vma_anon_name((void *)sighup_info, sizeof(*sighup_info), "state");
	sighup_info->count = 0.0;

	stress_set_proc_state(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		const bool rnd = stress_mwc1();

		sighup_info->signalled = false;

		rc = rnd ? stress_sighup_raise_signal(args) :
			   stress_sighup_process_group(args);
		if (rc == EXIT_SUCCESS) {
			stress_bogo_inc(args);
		} else {
			break;
		}
	} while (stress_continue(args));

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	rate = (sighup_info->count > 0.0) ? sighup_info->latency / sighup_info->count : 0.0;
	stress_metrics_set(args, 0, "nanosec SIGHUP latency",
		rate * STRESS_DBL_NANOSECOND, STRESS_METRIC_HARMONIC_MEAN);

	(void)munmap((void *)sighup_info, sizeof(*sighup_info));

	return rc;
}

const stressor_info_t stress_sighup_info = {
	.stressor = stress_sighup,
	.class = CLASS_SIGNAL | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.help = help
};

/*
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <limits.h>
#include <sys/signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include "user.h"
#include "kern_util.h"
#include "os.h"
#include "um_malloc.h"
#include "kern_constants.h"

struct helper_data {
	void (*pre_exec)(void*);
	void *pre_data;
	char **argv;
	int fd;
	char *buf;
};

static int helper_child(void *arg)
{
	struct helper_data *data = arg;
	char **argv = data->argv;
	int errval;

	if (data->pre_exec != NULL)
		(*data->pre_exec)(data->pre_data);
	errval = execvp_noalloc(data->buf, argv[0], argv);
	printk("helper_child - execvp of '%s' failed - errno = %d\n", argv[0],
	       -errval);
	write(data->fd, &errval, sizeof(errval));
	kill(os_getpid(), SIGKILL);
	return 0;
}

/* Returns either the pid of the child process we run or -E* on failure.
 * XXX The alloc_stack here breaks if this is called in the tracing thread, so
 * we need to receive a preallocated stack (a local buffer is ok). */
int run_helper(void (*pre_exec)(void *), void *pre_data, char **argv)
{
	struct helper_data data;
	unsigned long stack, sp;
	int pid, fds[2], ret, n;

	stack = alloc_stack(0, __cant_sleep());
	if (stack == 0)
		return -ENOMEM;

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	if (ret < 0) {
		ret = -errno;
		printk("run_helper : pipe failed, errno = %d\n", errno);
		goto out_free;
	}

	ret = os_set_exec_close(fds[1]);
	if (ret < 0) {
		printk("run_helper : setting FD_CLOEXEC failed, ret = %d\n",
		       -ret);
		goto out_close;
	}

	sp = stack + UM_KERN_PAGE_SIZE - sizeof(void *);
	data.pre_exec = pre_exec;
	data.pre_data = pre_data;
	data.argv = argv;
	data.fd = fds[1];
	data.buf = __cant_sleep() ? kmalloc(PATH_MAX, UM_GFP_ATOMIC) :
					kmalloc(PATH_MAX, UM_GFP_KERNEL);
	pid = clone(helper_child, (void *) sp, CLONE_VM, &data);
	if (pid < 0) {
		ret = -errno;
		printk("run_helper : clone failed, errno = %d\n", errno);
		goto out_free2;
	}

	close(fds[1]);
	fds[1] = -1;

	/*
	 * Read the errno value from the child, if the exec failed, or get 0 if
	 * the exec succeeded because the pipe fd was set as close-on-exec.
	 */
	n = read(fds[0], &ret, sizeof(ret));
	if (n == 0) {
		ret = pid;
	} else {
		if (n < 0) {
			n = -errno;
			printk("run_helper : read on pipe failed, ret = %d\n",
			       -n);
			ret = n;
			kill(pid, SIGKILL);
		}
		CATCH_EINTR(waitpid(pid, NULL, __WCLONE));
	}

out_free2:
	kfree(data.buf);
out_close:
	if (fds[1] != -1)
		close(fds[1]);
	close(fds[0]);
out_free:
	free_stack(stack, 0);
	return ret;
}

int run_helper_thread(int (*proc)(void *), void *arg, unsigned int flags,
		      unsigned long *stack_out)
{
	unsigned long stack, sp;
	int pid, status, err;

	stack = alloc_stack(0, __cant_sleep());
	if (stack == 0)
		return -ENOMEM;

	sp = stack + UM_KERN_PAGE_SIZE - sizeof(void *);
	pid = clone(proc, (void *) sp, flags, arg);
	if (pid < 0) {
		err = -errno;
		printk("run_helper_thread : clone failed, errno = %d\n",
		       errno);
		return err;
	}
	if (stack_out == NULL) {
		CATCH_EINTR(pid = waitpid(pid, &status, __WCLONE));
		if (pid < 0) {
			err = -errno;
			printk("run_helper_thread - wait failed, errno = %d\n",
			       errno);
			pid = err;
		}
		if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
			printk("run_helper_thread - thread returned status "
			       "0x%x\n", status);
		free_stack(stack, 0);
	} else
		*stack_out = stack;
	return pid;
}

int helper_wait(int pid, int nohang, char *pname)
{
	int ret, status;
	int wflags = __WCLONE;

	if (nohang)
		wflags |= WNOHANG;

	if (!pname)
		pname = "helper_wait";

	CATCH_EINTR(ret = waitpid(pid, &status, wflags));
	if (ret < 0) {
		printk(UM_KERN_ERR "%s : waitpid process %d failed, "
		       "errno = %d\n", pname, pid, errno);
		return -errno;
	} else if (nohang && ret == 0) {
		printk(UM_KERN_ERR "%s : process %d has not exited\n",
		       pname, pid);
		return -ECHILD;
	} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printk(UM_KERN_ERR "%s : process %d didn't exit with "
		       "status 0\n", pname, pid);
		return -ECHILD;
	} else
		return 0;
}

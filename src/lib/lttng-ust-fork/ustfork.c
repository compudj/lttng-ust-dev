/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2009  Pierre-Marc Fournier
 * Copyright (C) 2011-2012  Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

/* Has to be included first to override dlfcn.h */
#include <common/compat/dlfcn.h>

#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sched.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>

#include <lttng/ust-fork.h>

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
static int (*__ust_before_fork)(sigset_t *save_sigset) = NULL;
static int (*__ust_after_fork_child)(sigset_t *restore_sigset) = NULL;
static int (*__ust_after_fork_parent)(sigset_t *restore_sigset) = NULL;

static void (*__ust_after_setns)(void) = NULL;
static void (*__ust_after_unshare)(void) = NULL;
static void (*__ust_after_setuid)(void) = NULL;
static void (*__ust_after_setgid)(void) = NULL;
static void (*__ust_after_seteuid)(void) = NULL;
static void (*__ust_after_setegid)(void) = NULL;
static void (*__ust_after_setreuid)(void) = NULL;
static void (*__ust_after_setregid)(void) = NULL;
static void (*__ust_after_setresuid)(void) = NULL;
static void (*__ust_after_setresgid)(void) = NULL;

static
void *_init_ust_before_fork(void)
{
	if (__ust_before_fork == NULL) {
		__ust_before_fork = dlsym(RTLD_DEFAULT, "ust_before_fork");

		if (__ust_before_fork == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_before_fork;
}

static
void *_init_ust_after_fork_child(void)
{
	if (__ust_after_fork_child == NULL) {
		__ust_after_fork_child = dlsym(RTLD_DEFAULT, "ust_after_fork_child");

		if (__ust_after_fork_child == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_fork_child;
}

static
void *_init_ust_after_fork_parent(void)
{
	if (__ust_after_fork_parent == NULL) {
		__ust_after_fork_parent = dlsym(RTLD_DEFAULT, "ust_after_fork_parent");

		if (__ust_after_fork_parent == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_fork_parent;
}

static
void *_init_ust_after_setns(void)
{
	if (__ust_after_setns == NULL) {
		__ust_after_setns = dlsym(RTLD_DEFAULT, "ust_after_setns");

		if (__ust_after_setns == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_setns;
}

static
void *_init_ust_after_unshare(void)
{
	if (__ust_after_unshare == NULL) {
		__ust_after_unshare = dlsym(RTLD_DEFAULT, "ust_after_unshare");

		if (__ust_after_unshare == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_unshare;
}

static
void *_init_ust_after_setuid(void)
{
	if (__ust_after_setuid == NULL) {
		__ust_after_setuid = dlsym(RTLD_DEFAULT, "ust_after_setuid");

		if (__ust_after_setuid == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_setuid;
}

static
void *_init_ust_after_setgid(void)
{
	if (__ust_after_setgid == NULL) {
		__ust_after_setgid = dlsym(RTLD_DEFAULT, "ust_after_setgid");

		if (__ust_after_setgid == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_setgid;
}

static
void *_init_ust_after_seteuid(void)
{
	if (__ust_after_seteuid == NULL) {
		__ust_after_seteuid = dlsym(RTLD_DEFAULT, "ust_after_seteuid");

		if (__ust_after_seteuid == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_seteuid;
}

static
void *_init_ust_after_setegid(void)
{
	if (__ust_after_setegid == NULL) {
		__ust_after_setegid = dlsym(RTLD_DEFAULT, "ust_after_setegid");

		if (__ust_after_setegid == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_setegid;
}

static
void *_init_ust_after_setreuid(void)
{
	if (__ust_after_setreuid == NULL) {
		__ust_after_setreuid = dlsym(RTLD_DEFAULT, "ust_after_setreuid");

		if (__ust_after_setreuid == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_setreuid;
}

static
void *_init_ust_after_setregid(void)
{
	if (__ust_after_setregid == NULL) {
		__ust_after_setregid = dlsym(RTLD_DEFAULT, "ust_after_setregid");

		if (__ust_after_setregid == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_setregid;
}

static
void *_init_ust_after_setresuid(void)
{
	if (__ust_after_setresuid == NULL) {
		__ust_after_setresuid = dlsym(RTLD_DEFAULT, "ust_after_setresuid");

		if (__ust_after_setresuid == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_setresuid;
}

static
void *_init_ust_after_setresgid(void)
{
	if (__ust_after_setresgid == NULL) {
		__ust_after_setresgid = dlsym(RTLD_DEFAULT, "ust_after_setresgid");

		if (__ust_after_setresgid == NULL) {
			fprintf(stderr, "%s\n", dlerror());
		}
	}

	return __ust_after_setresgid;
}

static
void _lttng_ust_fork_ctor(void)
	__attribute__((constructor));
static
void _lttng_ust_fork_ctor(void)
{
	void *handle = NULL;

	/*
	 * Load ust-2.12 in the global symbol namespace.
	 */
	handle = dlopen("liblttng-ust.so.0", RTLD_GLOBAL | RTLD_NOW);
	if (!handle) {
		fprintf(stderr, "liblttng-ust-fork.so.1: Failed to dlopen liblttng-ust.so.0: %s\n", dlerror());
		abort();
	}
}
#endif

pid_t fork(void)
{
	static pid_t (*plibc_func)(void) = NULL;
	sigset_t sigset;
	pid_t retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "fork");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"fork\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	lttng_ust_before_fork(&sigset);
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_before_fork()) {
		__ust_before_fork(&sigset);
	}
#endif

	/* Do the real fork */
	retval = plibc_func();
	saved_errno = errno;
	if (retval == 0) {
		/* child */
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
		if (_init_ust_after_fork_child()) {
			__ust_after_fork_child(&sigset);
		}
#endif
		lttng_ust_after_fork_child(&sigset);
	} else {
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
		if (_init_ust_after_fork_parent()) {
			__ust_after_fork_parent(&sigset);
		}
#endif
		lttng_ust_after_fork_parent(&sigset);
	}
	errno = saved_errno;
	return retval;
}

int daemon(int nochdir, int noclose)
{
	static int (*plibc_func)(int nochdir, int noclose) = NULL;
	sigset_t sigset;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "daemon");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"daemon\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	lttng_ust_before_fork(&sigset);
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_before_fork()) {
		__ust_before_fork(&sigset);
	}
#endif

	/* Do the real daemon call */
	retval = plibc_func(nochdir, noclose);
	saved_errno = errno;
	if (retval == 0) {
		/* child, parent called _exit() directly */
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
		if (_init_ust_after_fork_child()) {
			__ust_after_fork_child(&sigset);
		}
#endif
		lttng_ust_after_fork_child(&sigset);
	} else {
		/* on error in the parent */
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
		if (_init_ust_after_fork_parent()) {
			__ust_after_fork_parent(&sigset);
		}
#endif
		lttng_ust_after_fork_parent(&sigset);
	}
	errno = saved_errno;
	return retval;
}

int setuid(uid_t uid)
{
	static int (*plibc_func)(uid_t uid) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "setuid");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"setuid\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setuid */
	retval = plibc_func(uid);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_setuid()) {
		__ust_after_setuid();
	}
#endif
	lttng_ust_after_setuid();

	errno = saved_errno;
	return retval;
}

int setgid(gid_t gid)
{
	static int (*plibc_func)(gid_t gid) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "setgid");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"setgid\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setgid */
	retval = plibc_func(gid);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_setgid()) {
		__ust_after_setgid();
	}
#endif
	lttng_ust_after_setgid();

	errno = saved_errno;
	return retval;
}

int seteuid(uid_t euid)
{
	static int (*plibc_func)(uid_t euid) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "seteuid");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"seteuid\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real seteuid */
	retval = plibc_func(euid);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_seteuid()) {
		__ust_after_seteuid();
	}
#endif
	lttng_ust_after_seteuid();

	errno = saved_errno;
	return retval;
}

int setegid(gid_t egid)
{
	static int (*plibc_func)(gid_t egid) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "setegid");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"setegid\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setegid */
	retval = plibc_func(egid);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_setegid()) {
		__ust_after_setegid();
	}
#endif
	lttng_ust_after_setegid();

	errno = saved_errno;
	return retval;
}

int setreuid(uid_t ruid, uid_t euid)
{
	static int (*plibc_func)(uid_t ruid, uid_t euid) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "setreuid");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"setreuid\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setreuid */
	retval = plibc_func(ruid, euid);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_setreuid()) {
		__ust_after_setreuid();
	}
#endif
	lttng_ust_after_setreuid();

	errno = saved_errno;
	return retval;
}

int setregid(gid_t rgid, gid_t egid)
{
	static int (*plibc_func)(gid_t rgid, gid_t egid) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "setregid");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"setregid\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setregid */
	retval = plibc_func(rgid, egid);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_setregid()) {
		__ust_after_setregid();
	}
#endif
	lttng_ust_after_setregid();

	errno = saved_errno;
	return retval;
}

#ifdef __linux__

struct user_desc;

struct ustfork_clone_info {
	int (*fn)(void *);
	void *arg;
	sigset_t sigset;
};

static int clone_fn(void *arg)
{
	struct ustfork_clone_info *info = (struct ustfork_clone_info *) arg;

	/* clone is now done and we are in child */
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_fork_child()) {
		__ust_after_fork_child(&info->sigset);
	}
#endif
	lttng_ust_after_fork_child(&info->sigset);
	return info->fn(info->arg);
}

int clone(int (*fn)(void *), void *child_stack, int flags, void *arg, ...)
{
	static int (*plibc_func)(int (*fn)(void *), void *child_stack,
			int flags, void *arg, pid_t *ptid,
			struct user_desc *tls, pid_t *ctid) = NULL;
	/* var args */
	pid_t *ptid;
	struct user_desc *tls;
	pid_t *ctid;
	/* end of var args */
	va_list ap;
	int retval;
	int saved_errno;

	va_start(ap, arg);
	ptid = va_arg(ap, pid_t *);
	tls = va_arg(ap, struct user_desc *);
	ctid = va_arg(ap, pid_t *);
	va_end(ap);

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "clone");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"clone\" symbol.\n");
			errno = ENOSYS;
			return -1;
		}
	}

	if (flags & CLONE_VM) {
		/*
		 * Creating a thread, no need to intervene, just pass on
		 * the arguments.
		 */
		retval = plibc_func(fn, child_stack, flags, arg, ptid,
				tls, ctid);
		saved_errno = errno;
	} else {
		/* Creating a real process, we need to intervene. */
		struct ustfork_clone_info info = { .fn = fn, .arg = arg };

		lttng_ust_before_fork(&info.sigset);
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
		if (_init_ust_before_fork()) {
			__ust_before_fork(&info.sigset);
		}
#endif
		retval = plibc_func(clone_fn, child_stack, flags, &info,
				ptid, tls, ctid);
		saved_errno = errno;
		/* The child doesn't get here. */
#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
		if (_init_ust_after_fork_parent()) {
			__ust_after_fork_parent(&info.sigset);
		}
#endif
		lttng_ust_after_fork_parent(&info.sigset);
	}
	errno = saved_errno;
	return retval;
}

int setns(int fd, int nstype)
{
	static int (*plibc_func)(int fd, int nstype) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "setns");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"setns\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setns */
	retval = plibc_func(fd, nstype);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_setns()) {
		__ust_after_setns();
	}
#endif
	lttng_ust_after_setns();

	errno = saved_errno;
	return retval;
}

int unshare(int flags)
{
	static int (*plibc_func)(int flags) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "unshare");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"unshare\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setns */
	retval = plibc_func(flags);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_unshare()) {
		__ust_after_unshare();
	}
#endif
	lttng_ust_after_unshare();

	errno = saved_errno;
	return retval;
}

int setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	static int (*plibc_func)(uid_t ruid, uid_t euid, uid_t suid) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "setresuid");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"setresuid\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setresuid */
	retval = plibc_func(ruid, euid, suid);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_setresuid()) {
		__ust_after_setresuid();
	}
#endif
	lttng_ust_after_setresuid();

	errno = saved_errno;
	return retval;
}

int setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
	static int (*plibc_func)(gid_t rgid, gid_t egid, gid_t sgid) = NULL;
	int retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "setresgid");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"setresgid\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	/* Do the real setresgid */
	retval = plibc_func(rgid, egid, sgid);
	saved_errno = errno;

#if !defined(LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS)
	if (_init_ust_after_setresgid()) {
		__ust_after_setresgid();
	}
#endif
	lttng_ust_after_setresgid();

	errno = saved_errno;
	return retval;
}

#elif defined (__FreeBSD__)

pid_t rfork(int flags)
{
	static pid_t (*plibc_func)(int flags) = NULL;
	sigset_t sigset;
	pid_t retval;
	int saved_errno;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, "rfork");
		if (plibc_func == NULL) {
			fprintf(stderr, "libustfork: unable to find \"rfork\" symbol\n");
			errno = ENOSYS;
			return -1;
		}
	}

	lttng_ust_before_fork(&sigset);
	/* Do the real rfork */
	retval = plibc_func(flags);
	saved_errno = errno;
	if (retval == 0) {
		/* child */
		lttng_ust_after_fork_child(&sigset);
	} else {
		lttng_ust_after_fork_parent(&sigset);
	}
	errno = saved_errno;
	return retval;
}

/*
 * On BSD, no need to override vfork, because it runs in the context of
 * the parent, with parent waiting until execve or exit is executed in
 * the child.
 */

#else
#warning "Unknown OS. You might want to ensure that fork/clone/vfork/fork handling is complete."
#endif

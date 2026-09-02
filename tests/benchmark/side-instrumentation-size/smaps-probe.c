/*
 * SPDX-FileCopyrightText: 2026 EfficiOS, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/*
 * Report what a program's own mappings cost it, as the kernel accounts
 * them, once the dynamic loader has applied every relocation.
 *
 * Preloaded rather than linked into the programs it measures, because
 * those same binaries are what measure-size weighs: anything added to
 * them would be counted as instrumentation. A constructor in a
 * preloaded object runs after relocation and before main(), which is
 * exactly when the loader has finished dirtying pages and the program
 * has not yet touched any of its own.
 *
 * Only the mappings backed by the executable itself are reported. The
 * loader, the C library and this object are left out.
 *
 * Each mapping is followed by which of its pages this process has
 * actually faulted in, so a reader can ask what a section costs rather
 * than what the mapping it shares costs. That is the question worth
 * asking of a description which is mapped and never read: it is in the
 * address space, and demand paging means this process never fetches it.
 *
 * The answer comes from /proc/self/pagemap and not from mincore(),
 * which for a file backed mapping answers whether the page is in the
 * page cache -- true of a file just built and linked, whether or not
 * this process ever touched it.
 *
 * Environment:
 *
 *   SMAPS_PROGRAM  name of the executable to act on. Everything else
 *                  the preload reaches does nothing at all, which
 *                  matters because a program built by libtool is
 *                  reached through a wrapper shell: without this the
 *                  shell would report its own mappings, and would be
 *                  the process held rather than the program it has yet
 *                  to exec. A "lt-" prefix, which libtool gives the
 *                  binary in some configurations, is accepted too.
 *
 *   SMAPS_REPORT   file to append the report to. Written with a single
 *                  write(), which an O_APPEND regular file keeps whole,
 *                  so several processes may name the same file.
 *
 *   SMAPS_WAIT     if set, write a newline to standard output and then
 *                  read standard input to end of file before returning.
 *                  The measuring program uses this to hold several
 *                  processes alive at once, which is the only way a
 *                  clean page shows up as shared rather than as
 *                  private: a page mapped by one process alone is
 *                  Private_Clean however shareable it is. The newline
 *                  says the mappings are in place, so the measuring
 *                  program never has to guess whether they are.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define REPORT_MAX	65536

struct vma {
	unsigned long start, end;
	char perms[8];
	long rss, shared_clean, shared_dirty, private_clean, private_dirty;
};

/* Set in a pagemap entry when the page is in this process's tables. */
#define PAGEMAP_PRESENT		(1ULL << 63)
#define PAGEMAP_SWAPPED		(1ULL << 62)

/*
 * Which pages of the mapping this process has faulted in, as a run of 0
 * and 1 in the order they are mapped.
 *
 * /proc/self/pagemap and not mincore(): for a file backed mapping
 * mincore() answers whether the page is in the page cache, which it is
 * for a file just built, whether or not this process ever touched it.
 */
static void emit_resident(int pagemap, char *buf, size_t *len,
		const char *name, const struct vma *v)
{
	size_t page = (size_t) sysconf(_SC_PAGESIZE);
	size_t pages = (v->end - v->start) / page;
	size_t i;
	int n;

	if (pagemap < 0 || !pages || *len >= REPORT_MAX)
		return;

	n = snprintf(buf + *len, REPORT_MAX - *len, "%s-resident %lx ",
		name, v->start);

	if (n > 0)
		*len += (size_t) n;

	for (i = 0; i < pages && *len + 2 < REPORT_MAX; i++) {
		uint64_t entry = 0;
		off_t at = (off_t) ((v->start / page + i) * sizeof(entry));
		int here = 0;

		if (pread(pagemap, &entry, sizeof(entry), at) == sizeof(entry))
			here = (entry & (PAGEMAP_PRESENT | PAGEMAP_SWAPPED)) != 0;

		buf[(*len)++] = here ? '1' : '0';
	}

	if (*len + 1 < REPORT_MAX)
		buf[(*len)++] = '\n';
}


static void emit(int pagemap, char *buf, size_t *len, const char *name,
		const struct vma *v)
{
	int n;

	if (*len >= REPORT_MAX)
		return;

	n = snprintf(buf + *len, REPORT_MAX - *len,
		"%s %lx %lx %s %ld %ld %ld %ld %ld\n",
		name, v->start, v->end, v->perms, v->rss,
		v->shared_clean, v->shared_dirty,
		v->private_clean, v->private_dirty);

	if (n > 0)
		*len += (size_t) n;

	emit_resident(pagemap, buf, len, name, v);
}

/*
 * A smaps entry is a header line naming the mapping followed by fields
 * for it, so a field belongs to the header last seen.
 */
static void scan(const char *exe, const char *name, char *buf, size_t *len)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	int pagemap = open("/proc/self/pagemap", O_RDONLY);
	char line[8192];
	struct vma v;
	int mine = 0, have = 0;

	if (!f) {
		if (pagemap >= 0)
			close(pagemap);

		return;
	}

	while (fgets(line, sizeof(line), f)) {
		unsigned long start, end;
		char perms[8];
		long value;

		if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3) {
			if (mine && have)
				emit(pagemap, buf, len, name, &v);

			memset(&v, 0, sizeof(v));
			v.start = start;
			v.end = end;
			memcpy(v.perms, perms, sizeof(v.perms));
			/*
			 * The path is the last field of the header, so a
			 * mapping is ours when the header ends with the
			 * name of the executable.
			 */
			mine = strstr(line, exe) != NULL;
			have = 1;
			continue;
		}

		if (!mine)
			continue;

		if (sscanf(line, "Rss: %ld kB", &value) == 1)
			v.rss = value;
		else if (sscanf(line, "Shared_Clean: %ld kB", &value) == 1)
			v.shared_clean = value;
		else if (sscanf(line, "Shared_Dirty: %ld kB", &value) == 1)
			v.shared_dirty = value;
		else if (sscanf(line, "Private_Clean: %ld kB", &value) == 1)
			v.private_clean = value;
		else if (sscanf(line, "Private_Dirty: %ld kB", &value) == 1)
			v.private_dirty = value;
	}

	if (mine && have)
		emit(pagemap, buf, len, name, &v);

	if (pagemap >= 0)
		close(pagemap);

	fclose(f);
}

/*
 * Whether this process is the one asked for. A program built by libtool
 * is reached through a wrapper shell, so several processes see this
 * object and only one of them is the program.
 */
static int wanted(const char *name)
{
	const char *want = getenv("SMAPS_PROGRAM");

	if (!want || !*want)
		return 1;

	if (!strcmp(name, want))
		return 1;

	return !strncmp(name, "lt-", 3) && !strcmp(name + 3, want);
}


static void wait_for_release(void)
{
	char discard[4096];

	if (!getenv("SMAPS_WAIT"))
		return;

	if (write(STDOUT_FILENO, "\n", 1) != 1)
		return;

	while (read(STDIN_FILENO, discard, sizeof(discard)) > 0)
		;
}

__attribute__((constructor))
static void report(void)
{
	static char buf[REPORT_MAX];
	const char *path = getenv("SMAPS_REPORT");
	char exe[4096];
	const char *name;
	size_t len = 0;
	ssize_t n;
	int fd;

	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

	if (n < 0)
		return;

	exe[n] = '\0';
	name = strrchr(exe, '/');
	name = name ? name + 1 : exe;

	if (!wanted(name))
		return;

	if (path && *path) {
		scan(exe, name, buf, &len);

		fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);

		if (fd >= 0) {
			if (write(fd, buf, len) != (ssize_t) len) {
				/* Nothing to be done about a short write. */
			}

			close(fd);
		}
	}

	wait_for_release();
}

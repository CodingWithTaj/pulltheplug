#define _GNU_SOURCE
// fortify turns open() etc. into inline wrappers that clash with ours
#undef _FORTIFY_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXFD 1024 // fds past this aren't tracked, plenty for sqlite

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_openat64)(int, const char *, int, ...);
static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*real_pwrite64)(int, const void *, size_t, off64_t);
static int (*real_ftruncate)(int, off_t);
static int (*real_ftruncate64)(int, off64_t);
static int (*real_fsync)(int);
static int (*real_fdatasync)(int);
static int (*real_close)(int);
static int (*real_unlink)(const char *);
static int (*real_unlinkat)(int, const char *, int);
static int (*real_rename)(const char *, const char *);

static __thread int busy;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char *paths[MAXFD];
static char append[MAXFD];
static int tracefd = -1, datafd = -1;
static unsigned long seq;

#define R(f) real_##f = dlsym(RTLD_NEXT, #f)
static void load(void)
{
	R(open); R(open64); R(openat); R(openat64);
	R(write); R(pwrite); R(pwrite64);
	R(ftruncate); R(ftruncate64);
	R(fsync); R(fdatasync); R(close);
	R(unlink); R(unlinkat); R(rename);
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static void resolve(void) { pthread_once(&once, load); }

// the child gets its own trace handles, otherwise flock is shared with the
// parent and doesn't keep the two apart
static void child(void)
{
	pthread_mutex_init(&lock, NULL);
	if (tracefd >= 0) real_close(tracefd);
	if (datafd >= 0) real_close(datafd);
	tracefd = datafd = -1;
}

__attribute__((constructor)) static void init(void)
{
	resolve();
	pthread_atfork(NULL, NULL, child);
}

// runs stmt with the lock held, unless we're already inside our own code
#define RECORD(stmt) do { \
	if (!busy) { \
		int e_ = errno; busy = 1; \
		pthread_mutex_lock(&lock); stmt; pthread_mutex_unlock(&lock); \
		busy = 0; errno = e_; \
	} \
} while (0)

static int ends(const char *s, const char *suf)
{
	size_t n = strlen(s), m = strlen(suf);
	return n >= m && !strcmp(s + n - m, suf);
}

static int tracked(const char *path)
{
	return path && (ends(path, ".db") || ends(path, ".sqlite") ||
		ends(path, ".sqlite3") || ends(path, "-wal") || ends(path, "-journal"));
}

static int tracked_fd(int fd) { return fd >= 0 && fd < MAXFD && paths[fd]; }

static int fd_path(int fd, char *out)
{
	char link[32];
	snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
	ssize_t n = readlink(link, out, PATH_MAX - 1);
	if (n < 0) return -1;
	out[n] = 0;
	return 0;
}

// for paths that may not exist after the call; resolves the directory so it
// matches what /proc/self/fd gives for open files
static int abs_path(int dirfd, const char *path, char *out)
{
	char full[2 * PATH_MAX], dir[PATH_MAX];
	if (path[0] == '/')
		snprintf(full, sizeof full, "%s", path);
	else if (dirfd == AT_FDCWD ? !getcwd(dir, sizeof dir) : fd_path(dirfd, dir) < 0)
		return -1;
	else
		snprintf(full, sizeof full, "%s/%s", dir, path);
	char *slash = strrchr(full, '/');
	*slash = 0;
	if (!realpath(full[0] ? full : "/", dir)) return -1;
	if (snprintf(out, PATH_MAX, "%s/%s", strcmp(dir, "/") ? dir : "", slash + 1) >= PATH_MAX)
		return -1;
	return 0;
}

// keeps each path a single space-free field
static void encode(const char *s, char *out)
{
	for (; *s; s++) {
		unsigned char c = *s;
		if (c <= ' ' || c == '%') out += sprintf(out, "%%%02X", c);
		else *out++ = c;
	}
	*out = 0;
}

static void put(int fd, const void *buf, size_t n)
{
	const char *p = buf;
	while (n > 0) {
		ssize_t r = real_write(fd, p, n);
		if (r < 0) {
			if (errno == EINTR) continue;
			return;
		}
		p += r;
		n -= r;
	}
}

static int open_trace(void)
{
	if (tracefd >= 0 && datafd >= 0) return 0;
	const char *p = getenv("PULLTHEPLUG_TRACE");
	if (!p || !*p) p = "pulltheplug.trace";
	char data[PATH_MAX];
	if (snprintf(data, sizeof data, "%s.data", p) >= PATH_MAX) return -1;
	int fl = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
	if (tracefd < 0) tracefd = real_open(p, fl, 0644);
	if (datafd < 0) datafd = real_open(data, fl, 0644);
	return tracefd >= 0 && datafd >= 0 ? 0 : -1;
}

// caller holds the lock and the flock
static void line(const char *fmt, ...)
{
	char buf[8 * PATH_MAX];
	int n = snprintf(buf, sizeof buf, "%lu %d ", ++seq, getpid());
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
	buf[n++] = '\n';
	put(tracefd, buf, n);
}

#define EMIT(...) do { \
	if (open_trace() == 0) { \
		flock(tracefd, LOCK_EX); line(__VA_ARGS__); flock(tracefd, LOCK_UN); \
	} \
} while (0)

static void on_write(int fd, long long off, const void *buf, size_t len)
{
	if (!tracked_fd(fd) || open_trace() < 0) return;
	flock(tracefd, LOCK_EX);
	long long doff = lseek(datafd, 0, SEEK_END);
	put(datafd, buf, len);
	line("write %d %lld %zu %lld", fd, off, len, doff);
	flock(tracefd, LOCK_UN);
}

static void on_open(int fd, const char *path, int flags)
{
	char abs[PATH_MAX], enc[3 * PATH_MAX], fl[32] = "";
	if (fd < 0 || fd >= MAXFD || !tracked(path) || fd_path(fd, abs) < 0) return;
	free(paths[fd]);
	paths[fd] = strdup(abs);
	append[fd] = !!(flags & O_APPEND);
	if (flags & O_CREAT) strcat(fl, ",creat");
	if (flags & O_TRUNC) strcat(fl, ",trunc");
	if (flags & O_EXCL) strcat(fl, ",excl");
	encode(abs, enc);
	EMIT("open %d %s %s", fd, fl[0] ? fl + 1 : "-", enc);
}

static void on_unlink(const char *abs)
{
	char enc[3 * PATH_MAX];
	encode(abs, enc);
	EMIT("unlink %s", enc);
}

static void on_rename(const char *from, const char *to)
{
	char e1[3 * PATH_MAX], e2[3 * PATH_MAX];
	encode(from, e1);
	encode(to, e2);
	EMIT("rename %s %s", e1, e2);
	for (int i = 0; i < MAXFD; i++)
		if (paths[i] && !strcmp(paths[i], from)) {
			free(paths[i]);
			paths[i] = strdup(to);
		}
}

// mode is only passed when the flags say so
#define MODE(flags) \
	int mode = 0; \
	if ((flags) & O_CREAT || ((flags) & O_TMPFILE) == O_TMPFILE) { \
		va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); \
	}

int open(const char *path, int flags, ...)
{
	MODE(flags);
	resolve();
	int fd = real_open(path, flags, mode);
	RECORD(on_open(fd, path, flags));
	return fd;
}

int open64(const char *path, int flags, ...)
{
	MODE(flags);
	resolve();
	int fd = real_open64(path, flags, mode);
	RECORD(on_open(fd, path, flags));
	return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
	MODE(flags);
	resolve();
	int fd = real_openat(dirfd, path, flags, mode);
	RECORD(on_open(fd, path, flags));
	return fd;
}

int openat64(int dirfd, const char *path, int flags, ...)
{
	MODE(flags);
	resolve();
	int fd = real_openat64(dirfd, path, flags, mode);
	RECORD(on_open(fd, path, flags));
	return fd;
}

ssize_t write(int fd, const void *buf, size_t n)
{
	resolve();
	long long off = -1;
	RECORD(
		if (tracked_fd(fd)) {
			struct stat st;
			if (!append[fd]) off = lseek(fd, 0, SEEK_CUR);
			else if (fstat(fd, &st) == 0) off = st.st_size;
		});
	ssize_t r = real_write(fd, buf, n);
	RECORD(if (r > 0 && off >= 0) on_write(fd, off, buf, r));
	return r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
	resolve();
	ssize_t r = real_pwrite(fd, buf, n, off);
	RECORD(if (r > 0) on_write(fd, off, buf, r));
	return r;
}

ssize_t pwrite64(int fd, const void *buf, size_t n, off64_t off)
{
	resolve();
	ssize_t r = real_pwrite64(fd, buf, n, off);
	RECORD(if (r > 0) on_write(fd, off, buf, r));
	return r;
}

int ftruncate(int fd, off_t len)
{
	resolve();
	int r = real_ftruncate(fd, len);
	RECORD(if (r == 0 && tracked_fd(fd)) EMIT("truncate %d %lld", fd, (long long)len));
	return r;
}

int ftruncate64(int fd, off64_t len)
{
	resolve();
	int r = real_ftruncate64(fd, len);
	RECORD(if (r == 0 && tracked_fd(fd)) EMIT("truncate %d %lld", fd, (long long)len));
	return r;
}

int fsync(int fd)
{
	resolve();
	int r = real_fsync(fd);
	RECORD(if (r == 0 && tracked_fd(fd)) EMIT("fsync %d", fd));
	return r;
}

// replay treats it the same as fsync
int fdatasync(int fd)
{
	resolve();
	int r = real_fdatasync(fd);
	RECORD(if (r == 0 && tracked_fd(fd)) EMIT("fsync %d", fd));
	return r;
}

int close(int fd)
{
	resolve();
	char *p = NULL;
	// drop the entry first so a thread reusing the fd number can't race us;
	// if the program closes our trace fds we just reopen them later
	RECORD(
		if (fd == tracefd) tracefd = -1;
		if (fd == datafd) datafd = -1;
		if (tracked_fd(fd)) { p = paths[fd]; paths[fd] = NULL; });
	int r = real_close(fd);
	// linux frees the fd even when close fails
	RECORD(if (p) EMIT("close %d", fd));
	free(p);
	return r;
}

int unlink(const char *path)
{
	resolve();
	char abs[PATH_MAX];
	int want = !busy && tracked(path) && abs_path(AT_FDCWD, path, abs) == 0;
	int r = real_unlink(path);
	RECORD(if (r == 0 && want) on_unlink(abs));
	return r;
}

int unlinkat(int dirfd, const char *path, int flags)
{
	resolve();
	char abs[PATH_MAX];
	int want = !busy && !(flags & AT_REMOVEDIR) && tracked(path) &&
		abs_path(dirfd, path, abs) == 0;
	int r = real_unlinkat(dirfd, path, flags);
	RECORD(if (r == 0 && want) on_unlink(abs));
	return r;
}

int rename(const char *from, const char *to)
{
	resolve();
	char a[PATH_MAX], b[PATH_MAX];
	int want = !busy && (tracked(from) || tracked(to)) &&
		abs_path(AT_FDCWD, from, a) == 0 && abs_path(AT_FDCWD, to, b) == 0;
	int r = real_rename(from, to);
	RECORD(if (r == 0 && want) on_rename(a, b));
	return r;
}

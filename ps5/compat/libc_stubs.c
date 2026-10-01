/*
 * libc functions the PS5's libc lacks, for xemu and its dependencies.
 *
 * Two kinds: small real implementations of what xemu uses (on what libkernel
 * does export), and stubs that fail with ENOSYS for features the PS5 doesn't
 * have (ptys, serial ports, users and groups, the process table).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <fstab.h>
#include <poll.h>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/*
 * Real implementations
 */

/* QEMU's main loop timeout. The signal mask is never used by QEMU (NULL). */
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
          const sigset_t *sigmask)
{
    (void)sigmask;
    int ms = -1;
    if (timeout) {
        /* Round up, so a short timeout doesn't become a busy poll. */
        long long t = (long long)timeout->tv_sec * 1000 +
                      (timeout->tv_nsec + 999999) / 1000000;
        ms = t > 0x7fffffff ? 0x7fffffff : (int)t;
    }
    return poll(fds, nfds, ms);
}

int pipe2(int fds[2], int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(fds) < 0) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if ((flags & O_CLOEXEC) && fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) {
            goto fail;
        }
        if ((flags & O_NONBLOCK) &&
            fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) < 0) {
            goto fail;
        }
    }
    return 0;

fail:
    close(fds[0]);
    close(fds[1]);
    return -1;
}

void closefrom(int lowfd)
{
    int max = getdtablesize();
    for (int fd = lowfd < 0 ? 0 : lowfd; fd < max; fd++) {
        close(fd);
    }
}

/* FreeBSD's thread id, as QEMU's qemu_get_thread_id() reads it. */
int thr_self(long *id)
{
    *id = pthread_getthreadid_np();
    return 0;
}

/* timegm: the inverse of gmtime, from the proleptic Gregorian day count. */
time_t timegm(struct tm *tm)
{
    long long y = tm->tm_year + 1900LL;
    long long m = tm->tm_mon;
    y += m / 12;
    m %= 12;
    if (m < 0) {
        m += 12;
        y--;
    }
    /* Days from 1970-01-01 to y-(m+1)-01 (Howard Hinnant's days_from_civil) */
    long long yy = m < 2 ? y - 1 : y;
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    long long yoe = yy - era * 400;
    long long mp = (m + 9) % 12;
    long long doy = (153 * mp + 2) / 5;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468 + (tm->tm_mday - 1);
    return (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL +
                    tm->tm_sec);
}

char *strsignal(int sig)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "Signal %d", sig);
    return buf;
}

/* The PS5 has only the C locale: one byte per character, one global locale. */
int ___mb_cur_max(void) { return 1; }
void *uselocale(void *locale) { (void)locale; return (void *)-1; }

int posix_madvise(void *addr, size_t len, int advice)
{
    (void)addr;
    (void)len;
    (void)advice;
    return 0;
}

/*
 * Features the PS5 doesn't have
 */

/* fstab: glib's GIO lists mount points with these; the PS5 has no /etc/fstab. */
int setfsent(void) { return 0; }
struct fstab *getfsent(void) { return NULL; }
void endfsent(void) { }

/* Users and groups (QEMU's -run-with user=, glib's home directory lookup,
 * which then falls back to $HOME). */
void *getpwnam(const char *name) { (void)name; errno = ENOENT; return NULL; }
void *getpwuid(unsigned int uid) { (void)uid; errno = ENOENT; return NULL; }
int getpwnam_r(const char *name, void *pwd, char *buf, size_t size,
               void **result)
{
    (void)name;
    (void)pwd;
    (void)buf;
    (void)size;
    *result = NULL;
    return ENOENT;
}
int initgroups(const char *name, unsigned int gid)
{
    (void)name;
    (void)gid;
    errno = ENOSYS;
    return -1;
}
int setgid(unsigned int gid) { (void)gid; errno = ENOSYS; return -1; }

/* Pseudo-terminals and serial ports (QEMU's pty and serial chardevs). */
int openpty(int *amaster, int *aslave, char *name, void *termp, void *winp)
{
    (void)amaster;
    (void)aslave;
    (void)name;
    (void)termp;
    (void)winp;
    errno = ENOSYS;
    return -1;
}
char *ptsname(int fd) { (void)fd; errno = ENOSYS; return NULL; }
void cfmakeraw(void *termios) { (void)termios; }
int cfsetispeed(void *termios, unsigned int speed)
{
    (void)termios;
    (void)speed;
    errno = ENOSYS;
    return -1;
}
int cfsetospeed(void *termios, unsigned int speed)
{
    (void)termios;
    (void)speed;
    errno = ENOSYS;
    return -1;
}

/* The process table (QEMU's process name, -run-with exit-with-parent). */
void *kinfo_getproc(int pid) { (void)pid; errno = ENOSYS; return NULL; }
int procctl(int idtype, long id, int cmd, void *data)
{
    (void)idtype;
    (void)id;
    (void)cmd;
    (void)data;
    errno = ENOSYS;
    return -1;
}
int thr_kill2(int pid, long id, int sig)
{
    (void)pid;
    (void)id;
    (void)sig;
    errno = ENOSYS;
    return -1;
}

/* Network databases (libpcap filter names, slirp interface lookups). */
void *getnetbyname(const char *name) { (void)name; return NULL; }
void *getprotobyname(const char *name) { (void)name; return NULL; }
unsigned int if_nametoindex(const char *ifname) { (void)ifname; return 0; }

/* File tree walks (glib's test utilities only). */
int nftw(const char *path, void *fn, int fd_limit, int flags)
{
    (void)path;
    (void)fn;
    (void)fd_limit;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

/*
 * geteuid/getegid (linked with --wrap): the real IDs. RADV's disk shader
 * cache (Mesa's __normal_user) is off when they differ from getuid/getgid,
 * which they may after the HEN jailbreak. __real_geteuid is the kernel's.
 */
uid_t __real_geteuid(void);
gid_t __real_getegid(void);

uid_t __wrap_geteuid(void)
{
    return getuid();
}

gid_t __wrap_getegid(void)
{
    return getgid();
}

/* XOG: no eventfd on the PS5. net.c treats the failure as "no network", so a
 * game runs offline until network play gets its own step. */
#ifndef REXEMU_SYS_EVENTFD_H
#define REXEMU_SYS_EVENTFD_H

#include <errno.h>

#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x80000

static inline int eventfd(unsigned int initval, int flags)
{
    (void)initval;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

#endif

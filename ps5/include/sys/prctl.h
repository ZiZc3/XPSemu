/* XOG: hakuX only uses prctl to shorten Linux's timer slack; the PS5 has none. */
#ifndef REXEMU_SYS_PRCTL_H
#define REXEMU_SYS_PRCTL_H

#define PR_SET_TIMERSLACK 29

static inline int prctl(int option, ...)
{
    (void)option;
    return 0;
}

#endif

/*
 * DNS resolver stubs for the PS5, whose libc has no resolver.
 * glib's GIO needs these to link; xemu never resolves DNS records through GIO
 * (host names go through getaddrinfo). Every lookup fails.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <errno.h>

int res_init(void) { return -1; }
int res_query(const char *dname, int class, int type, u_char *answer, int anslen)
{ (void)dname; (void)class; (void)type; (void)answer; (void)anslen; errno = ENOSYS; return -1; }
int res_ninit(res_state state) { (void)state; return -1; }
void res_nclose(res_state state) { (void)state; }
int res_nquery(res_state state, const char *dname, int class, int type, u_char *answer, int anslen)
{ (void)state; return res_query(dname, class, type, answer, anslen); }
int dn_expand(const u_char *msg, const u_char *eom, const u_char *src, char *dst, int dstsiz)
{ (void)msg; (void)eom; (void)src; (void)dst; (void)dstsiz; return -1; }
int dn_skipname(const u_char *ptr, const u_char *eom) { (void)ptr; (void)eom; return -1; }

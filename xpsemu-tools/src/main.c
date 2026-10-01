// XPSemu Tools - standalone app-jailbreak daemon for XPSemu (PPSA97358).
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What it does: XPSemu (like PS5SX2) runs sandboxed: no /data and no
// executable memory for its JIT. At startup it publishes its PID as JSON
// {"PID":<pid>} to /download0/etahen_jailbreak (staged via .tmp + rename,
// the etaHEN/OnionHEN protocol). This daemon watches for that request file
// from outside the sandbox, verifies the PID really belongs to a
// whitelisted title, then escalates it: uid/gid -> 0, rootdir/jaildir ->
// the kernel root vnode (sandbox escape), Sony caps/authid/attr bump.
// It then deletes the request file so the app's wait loop exits.
//
// Based on the same kernel-write pattern etaHEN's Hijacker::jailbreak()
// uses (and OnionHEN's app-jailbreak service, which the PS5SX2 Helper is
// based on). Implemented here with only the ps5-payload-sdk kernel helpers,
// so firmware offsets resolve at runtime (multi-fw, no hardcoded offsets).
//
// Differences from the PS5SX2 Helper:
//  - named "XPSemu Tools", success toast is "XPSemu Started..." (no debug
//    "Jailbreak!" toast per request).
//  - whitelist enforced at /data/whitelist.txt (one title ID per line).
//  - PID <-> sandbox title-ID safety check before touching creds.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

// Provided by the loader; the SDK's kernel helpers use it to resolve
// per-firmware addresses (KERNEL_ADDRESS_*). Same pattern as Lapy JB Daemon.
uint64_t kernel_base = 0;

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------
// Primary path: the same raw kernel toast XPSemu itself uses
// (ui/xemu-os-utils-ps5.c): 0xc30-byte request, text at offset 0x2d.
// Fallback: libSceNotification JSON toast (the Lapy JB Daemon path).
// Both are weak so a firmware/SDK missing one still links and runs.
__attribute__((weak)) int sceKernelSendNotificationRequest(
    int device, void *request, size_t size, int blocking);
__attribute__((weak)) int sceNotificationSend(int userId, bool isLogged,
                                              const char *payload);

// App info for the PID <-> title-ID safety check (SDK libkernel).
typedef struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

static void notify(const char *message)
{
    // Raw kernel toast first (exactly what XPSemu uses).
    if (sceKernelSendNotificationRequest) {
        static uint8_t request[0xc30];
        memset(request, 0, sizeof(request));
        snprintf((char *)request + 0x2d, 1024, "%s", message);
        sceKernelSendNotificationRequest(0, request, sizeof(request), 0);
        return;
    }
    // Fallback: modern notification JSON (Lapy-style).
    if (sceNotificationSend) {
        char json[1024];
        snprintf(json, sizeof(json),
                 "{\"rawData\":{"
                 "\"viewTemplateType\":\"InteractiveToastTemplateB\","
                 "\"channelType\":\"Downloads\","
                 "\"useCaseId\":\"IDC\","
                 "\"toastOverwriteType\":\"Yes\","
                 "\"isImmediate\":true,"
                 "\"priority\":100,"
                 "\"viewData\":{"
                 "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
                 "\"message\":{\"body\":\"%s\"},"
                 "\"subMessage\":{\"body\":\"XPSemu Tools\"}"
                 "},"
                 "\"localNotificationId\":\"XPSEMU_TOOLS\""
                 "}}",
                 message);
        sceNotificationSend(0xFE, true, json);
    }
}

#define LOG(fmt, ...)                          \
    do {                                       \
        printf("[xpsemu-tools] " fmt "\n",     \
               ##__VA_ARGS__);                 \
        fflush(stdout);                        \
    } while (0)

// Uppercase-insensitive title-ID compare without needing strings.h.
static int title_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
#define JB_AUTHID 0x4801000000000013ULL

#define SANDBOX_BASE "/mnt/sandbox"
#define JB_DOWNLOADDIR "/download0"
// Accepted request names (same as OnionHEN: etahen + onionhen).
#define JB_NAME_ETA "etahen_jailbreak"
#define JB_NAME_ONION "onionhen_jailbreak"
// Also watch the global path: some setups expose the request there.
#define JB_GLOBAL "/download0/etahen_jailbreak"
#define WHITELIST_PATH "/data/whitelist.txt"

// 100 ms: fast enough that XPSemu's ~10 s consume window (600 x 16.7 ms
// polls) always sees us, slow enough to idle while waiting.
#define POLL_US (100 * 1000)
#define REQ_MAX 256

// ---------------------------------------------------------------------------
// Escalation: replicates Hijacker::jailbreak() via SDK helpers.
// Returns 0 on full success, -1 on any failure.
// ---------------------------------------------------------------------------
static int escalate_pid(pid_t pid)
{
    if (pid <= 0)
        return -1;
    if (!kernel_get_proc(pid))
        return -1;

    int rc = 0;

    // uid -> 0 (root) on every cred slot the kernel checks.
    if (kernel_set_ucred_uid(pid, 0) != 0)
        rc = -1;
    if (kernel_set_ucred_ruid(pid, 0) != 0)
        rc = -1;
    if (kernel_set_ucred_svuid(pid, 0) != 0)
        rc = -1;
    if (kernel_set_ucred_rgid(pid, 0) != 0)
        rc = -1;
    if (kernel_set_ucred_svgid(pid, 0) != 0)
        rc = -1;

    // cr_ngroups = 0 (ucred+0x10, not exposed by SDK helpers).
    {
        intptr_t ucred = kernel_get_proc_ucred(pid);
        if (ucred) {
            const uint32_t ngroups = 0;
            if (kernel_copyin(&ngroups, ucred + 0x10, sizeof(ngroups)) != 0)
                rc = -1;
        } else {
            rc = -1;
        }
    }

    // Sandbox escape: point rootdir + jaildir at the kernel root vnode so
    // the app sees the real "/" instead of its sandbox view.
    intptr_t rootvnode = kernel_get_root_vnode();
    if (rootvnode) {
        if (kernel_set_proc_rootdir(pid, rootvnode) != 0)
            rc = -1;
        if (kernel_set_proc_jaildir(pid, rootvnode) != 0)
            rc = -1;
    } else {
        rc = -1;
    }

    // Sony privilege bump: the caps + authid combo etaHEN uses.
    if (kernel_set_ucred_authid(pid, JB_AUTHID) != 0)
        rc = -1;
    uint8_t caps[16];
    memset(caps, 0xff, sizeof(caps));
    if (kernel_set_ucred_caps(pid, caps) != 0)
        rc = -1;

    // sceAttr: read-modify-write, byte 3 (ucred+0x83) = 0x80 enables
    // ptrace (as OnionHEN's daemon_jailbreak.cpp does).
    uint8_t attrs[32] = { 0 };
    if (kernel_get_ucred_attrs(pid, attrs) != 0) {
        rc = -1;
    } else {
        attrs[3] = 0x80;
        if (kernel_set_ucred_attrs(pid, attrs) != 0)
            rc = -1;
    }

    // Verify root actually stuck (as OnionHEN does).
    if (kernel_get_ucred_uid(pid) != 0)
        rc = -1;

    return rc;
}

// ---------------------------------------------------------------------------
// Whitelist + safety checks
// ---------------------------------------------------------------------------
// Whitelist file: one title ID per line, '#' starts a comment.
// Re-read on every request so edits apply without rebooting.
static int title_allowed(const char *title_id)
{
    if (!title_id || !*title_id)
        return 0;
    FILE *f = fopen(WHITELIST_PATH, "r");
    if (!f) {
        LOG("no %s (errno %d): denying %s", WHITELIST_PATH, errno,
            title_id);
        return 0;
    }
    char line[64];
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        // Trim leading whitespace.
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
            s++;
        if (*s == '#' || *s == '\0')
            continue;
        // Trim trailing whitespace / CRLF.
        char *e = s + strlen(s);
        while (e > s &&
               (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                e[-1] == '\n'))
            *--e = '\0';
        if (title_eq(s, title_id)) {
            ok = 1;
            break;
        }
    }
    fclose(f);
    return ok;
}

// Sandbox dir names look like "<TITLEID>_<nnn>" (e.g. PPSA97358_00).
// Returns 1 and copies the TITLEID part to out when parseable.
static int sandbox_title(const char *dirname, char *out, size_t out_size)
{
    const char *us = strchr(dirname, '_');
    size_t len = us ? (size_t)(us - dirname) : strlen(dirname);
    if (len < 4 || len > 10 || len + 1 > out_size)
        return 0;
    memcpy(out, dirname, len);
    out[len] = '\0';
    return 1;
}

static pid_t parse_pid_json(const char *txt)
{
    const char *p = strstr(txt, "\"PID\"");
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"')
        p++;
    long pid = 0;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
        if (pid > 99999)
            break;
    }
    return (pid > 0 && pid < 100000) ? (pid_t)pid : -1;
}

static ssize_t slurp(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return n;
}

// Handle one request file. sandbox_name is "" for the global path.
// Always unlinks the file when it held a parseable PID (consume signal),
// so the app stops waiting even if we refused it.
static void handle_request(const char *path, const char *sandbox_name)
{
    char buf[REQ_MAX];
    if (slurp(path, buf, sizeof(buf)) < 0)
        return;

    pid_t pid = parse_pid_json(buf);
    if (pid <= 0) {
        LOG("ignoring malformed request at %s: '%.48s'", path, buf);
        return;
    }
    if (!kernel_get_proc(pid)) {
        LOG("pid %d gone already (%s)", (int)pid, path);
        unlink(path);
        return;
    }

    // Safety check 1: the PID must report a real title ID...
    app_info_t info;
    memset(&info, 0, sizeof(info));
    char actual[16] = "";
    if (sceKernelGetAppInfo(pid, &info) == 0) {
        memcpy(actual, info.title_id, sizeof(actual) - 1);
        actual[sizeof(actual) - 1] = '\0';
    }
    if (!actual[0]) {
        LOG("refusing pid %d: no app info", (int)pid);
        unlink(path);
        return;
    }

    // ...that matches the sandbox directory the request came from.
    if (sandbox_name && sandbox_name[0]) {
        char sb_title[16];
        if (sandbox_title(sandbox_name, sb_title, sizeof(sb_title)) &&
            !title_eq(sb_title, actual)) {
            LOG("refusing pid %d: sandbox %s != app %s", (int)pid,
                sandbox_name, actual);
            unlink(path);
            return;
        }
    }

    // Safety check 2: title must be whitelisted by the user.
    if (!title_allowed(actual)) {
        LOG("refusing pid %d (%s): not in %s", (int)pid, actual,
            WHITELIST_PATH);
        unlink(path);
        return;
    }

    if (escalate_pid(pid) == 0) {
        LOG("jailbroke %s pid %d", actual, (int)pid);
        // The only user-visible toast per unlock (no debug spam).
        notify("XPSemu Started...");
    } else {
        LOG("escalation FAILED for %s pid %d", actual, (int)pid);
        notify("XPSemu: jailbreak failed");
    }
    unlink(path);
}

int main(void)
{
    payload_args_t *args = payload_get_args();
    if (!args) {
        printf("[xpsemu-tools] needs elfldr/HBL launcher\n");
        return 1;
    }
    kernel_base = args->kdata_base_addr;
    LOG("XPSemu Tools active (kernel_base 0x%llx)",
        (unsigned long long)kernel_base);

    // Single boot toast; per-unlock toast is "XPSemu Started...".
    notify("XPSemu Tools active");
    LOG("whitelist: %s; watching %s/*%s/{%s,%s} and %s", WHITELIST_PATH,
        SANDBOX_BASE, JB_DOWNLOADDIR, JB_NAME_ETA, JB_NAME_ONION, JB_GLOBAL);

    static const char *const jb_names[] = { JB_NAME_ETA, JB_NAME_ONION };
    char path[512];
    for (;;) {
        DIR *d = opendir(SANDBOX_BASE);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.')
                    continue;
                for (size_t k = 0;
                     k < sizeof(jb_names) / sizeof(jb_names[0]); k++) {
                    int n = snprintf(path, sizeof(path), "%s/%s%s/%s",
                                     SANDBOX_BASE, de->d_name, JB_DOWNLOADDIR,
                                     jb_names[k]);
                    if (n <= 0 || (size_t)n >= sizeof(path))
                        continue;
                    struct stat st;
                    if (stat(path, &st) != 0 || st.st_size == 0)
                        continue;
                    handle_request(path, de->d_name);
                }
            }
            closedir(d);
        }
        // Fallback: a request visible at the global path.
        {
            struct stat st;
            if (stat(JB_GLOBAL, &st) == 0 && st.st_size > 0)
                handle_request(JB_GLOBAL, "");
        }
        usleep(POLL_US);
    }
    return 0;
}

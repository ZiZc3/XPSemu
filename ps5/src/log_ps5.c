/*
 * XOG: the emulated CPU's thread never waits for a log line. Our own code. GPL-3.0-or-later.
 *
 * A write to a file can stop a thread for milliseconds on the console, and the C library's
 * number formatting takes a lock and allocates. The core logs from kernel calls, so those
 * costs land inside the game's DPCs and interrupts.
 *
 * Lines go into a ring of fixed slots: a slot is claimed with one compare-and-swap and filled
 * without a lock, so a thread stopped halfway holds nobody up, and a signal handler can log
 * over its own thread. A full ring counts the line and drops it. One thread writes them out.
 * The first seconds are written at once: if the system ends a title without a signal while it
 * boots, its last lines are on disk.
 *
 * On the CPU's thread formatting is done here. Every other thread keeps the C library's, so
 * anything that must match it exactly (the shader generator's numbers) doesn't change.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#undef snprintf
#undef vsnprintf

#define SLOTS 1024u
#define SLOT_BYTES 1400u
#define SYNC_SECONDS 20u

typedef struct Slot {
    volatile uint32_t ready;    /* its sequence number + 1 when its text is whole */
    uint32_t len;
    char text[SLOT_BYTES];
} Slot;

static Slot g_slots[SLOTS];
static volatile uint32_t g_head, g_tail, g_dropped;
static volatile int g_draining, g_started;
static uint64_t g_sync_until_ns;

__thread bool rx_on_vcpu;

extern void rx_log_write(const char *text, size_t len);
extern int rx_clock_gettime(clockid_t clk, struct timespec *ts);

static uint64_t now_ns(void)
{
    struct timespec t;
    rx_clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void rx_log_drain(void)
{
    if (__atomic_exchange_n(&g_draining, 1, __ATOMIC_ACQUIRE)) {
        return;     /* the other one writes them */
    }
    static char out[1u << 16];
    size_t n = 0;
    uint32_t tail = g_tail;
    const uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    while (tail != head) {
        Slot *s = &g_slots[tail % SLOTS];
        if (__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) != tail + 1) {
            break;  /* still being filled: next time */
        }
        if (n + s->len > sizeof(out)) {
            rx_log_write(out, n);
            n = 0;
        }
        memcpy(out + n, s->text, s->len);
        n += s->len;
        __atomic_store_n(&s->ready, 0, __ATOMIC_RELAXED);
        tail++;
        __atomic_store_n(&g_tail, tail, __ATOMIC_RELEASE);
    }
    if (n) {
        rx_log_write(out, n);
    }
    const uint32_t dropped = __atomic_exchange_n(&g_dropped, 0, __ATOMIC_RELAXED);
    if (dropped) {
        char b[80];
        const int m = snprintf(b, sizeof(b), "xog-log: %u lines not kept, the ring was full\n", dropped);
        rx_log_write(b, (size_t)m);
    }
    __atomic_store_n(&g_draining, 0, __ATOMIC_RELEASE);
}

void rx_log_line(const char *text, size_t len)
{
    if (!g_started || now_ns() < g_sync_until_ns) {
        rx_log_write(text, len);
        return;
    }
    len = len > SLOT_BYTES ? SLOT_BYTES : len;
    uint32_t h = __atomic_load_n(&g_head, __ATOMIC_RELAXED);
    do {
        if (h - __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE) >= SLOTS) {
            __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED);
            return;
        }
    } while (!__atomic_compare_exchange_n(&g_head, &h, h + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    Slot *s = &g_slots[h % SLOTS];
    memcpy(s->text, text, len);
    s->len = (uint32_t)len;
    __atomic_store_n(&s->ready, h + 1, __ATOMIC_RELEASE);
}

static void *writer(void *arg)
{
    (void)arg;
    pthread_set_name_np(pthread_self(), "xog-log");
    for (;;) {
        rx_log_drain();
        usleep(2000);
    }
    return NULL;
}

void rx_log_start(void)
{
    g_sync_until_ns = now_ns() + SYNC_SECONDS * 1000000000ull;
    pthread_t t;
    extern int rx_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (rx_pthread_create(&t, NULL, writer, NULL) == 0) {
        pthread_detach(t);
        g_started = 1;
    }
}

/* ---- formatting, for the CPU's thread: no lock, no allocation ---- */

typedef struct Out {
    char *p;
    size_t cap, n;
} Out;

static void put(Out *o, char c)
{
    if (o->n + 1 < o->cap) {
        o->p[o->n] = c;
    }
    o->n++;
}

static void pad(Out *o, char c, int n)
{
    while (n-- > 0) {
        put(o, c);
    }
}

/* digits with sign or prefix, width, precision and flags */
static void number(Out *o, const char *prefix, const char *digits, int nd, int width, int prec, bool left, bool zero)
{
    const int np = (int)strlen(prefix);
    const int zeros = prec > nd ? prec - nd : 0;
    const int fill = width - np - zeros - nd;
    if (!left && !zero) {
        pad(o, ' ', fill);
    }
    for (int i = 0; i < np; i++) {
        put(o, prefix[i]);
    }
    if (!left && zero) {
        pad(o, '0', fill);
    }
    pad(o, '0', zeros);
    for (int i = 0; i < nd; i++) {
        put(o, digits[i]);
    }
    if (left) {
        pad(o, ' ', fill);
    }
}

static int to_digits(uint64_t v, unsigned base, bool upper, char *b)
{
    char t[32];
    int n = 0;
    do {
        const unsigned d = (unsigned)(v % base);
        t[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v);
    for (int i = 0; i < n; i++) {
        b[i] = t[n - 1 - i];
    }
    return n;
}

/* false: not a form this does (the C library's, then) */
static bool format(Out *o, const char *f, va_list ap)
{
    static const uint64_t tens[] = {1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull};
    for (; *f; f++) {
        if (*f != '%') {
            put(o, *f);
            continue;
        }
        f++;
        bool left = false, plus = false, space = false, alt = false, zero = false;
        for (;; f++) {
            if (*f == '-') {
                left = true;
            } else if (*f == '+') {
                plus = true;
            } else if (*f == ' ') {
                space = true;
            } else if (*f == '#') {
                alt = true;
            } else if (*f == '0') {
                zero = true;
            } else {
                break;
            }
        }
        int width = 0, prec = -1;
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = true;
                width = -width;
            }
            f++;
        } else {
            while (*f >= '0' && *f <= '9') {
                width = width * 10 + (*f++ - '0');
            }
        }
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') {
                prec = va_arg(ap, int);
                f++;
            } else {
                while (*f >= '0' && *f <= '9') {
                    prec = prec * 10 + (*f++ - '0');
                }
            }
        }
        int size = 0;   /* -2 hh, -1 h, 0 int, 1 long and the like */
        if (*f == 'h') {
            f++;
            size = -1;
            if (*f == 'h') {
                f++;
                size = -2;
            }
        } else if (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't') {
            size = 1;
            if (*f++ == 'l' && *f == 'l') {
                f++;
            }
        }
        char b[40];
        switch (*f) {
        case '%':
            put(o, '%');
            break;
        case 'c':
            b[0] = (char)va_arg(ap, int);
            number(o, "", b, 1, width, -1, left, false);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            s = s ? s : "(null)";
            int n = 0;
            while ((prec < 0 || n < prec) && s[n]) {
                n++;
            }
            number(o, "", s, n, width, -1, left, false);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = size == 1 ? va_arg(ap, long long) : va_arg(ap, int);
            v = size == -1 ? (short)v : size == -2 ? (signed char)v : v;
            const uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
            int n = u == 0 && prec == 0 ? 0 : to_digits(u, 10, false, b);
            number(o, v < 0 ? "-" : plus ? "+" : space ? " " : "", b, n, width, prec, left, zero && prec < 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v = size == 1 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            v = size == -1 ? (unsigned short)v : size == -2 ? (unsigned char)v : v;
            const unsigned base = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
            int n = v == 0 && prec == 0 ? 0 : to_digits(v, base, *f == 'X', b);
            const char *pre = alt && v && base == 16 ? (*f == 'X' ? "0X" : "0x") : alt && base == 8 ? "0" : "";
            number(o, pre, b, n, width, prec, left, zero && prec < 0);
            break;
        }
        case 'p': {
            const uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            const int n = to_digits(v, 16, false, b);
            number(o, "0x", b, n, width, -1, left, false);
            break;
        }
        case 'f':
        case 'F': {
            const double v = va_arg(ap, double);
            const int p = prec < 0 ? 6 : prec;
            const double m = v < 0 ? -v : v;
            /* NaN, infinity, huge, or more digits than an integer holds */
            if (!(m < 1e12) || p > 6) {
                return false;
            }
            uint64_t whole = (uint64_t)m;
            uint64_t frac = (uint64_t)((m - (double)whole) * (double)tens[p] + 0.5);
            if (frac >= tens[p]) {
                frac -= tens[p];
                whole++;
            }
            int n = to_digits(whole, 10, false, b);
            if (p) {
                b[n++] = '.';
                for (int i = p - 1; i >= 0; i--) {
                    b[n + i] = (char)('0' + frac % 10);
                    frac /= 10;
                }
                n += p;
            }
            number(o, v < 0 ? "-" : plus ? "+" : space ? " " : "", b, n, width, -1, left, zero);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

int rx_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    if (rx_on_vcpu) {
        Out o = {buf, cap, 0};
        va_list a2;
        va_copy(a2, ap);
        const bool ok = format(&o, fmt, a2);
        va_end(a2);
        if (ok) {
            if (cap) {
                buf[o.n < cap ? o.n : cap - 1] = 0;
            }
            return (int)o.n;
        }
    }
    return vsnprintf(buf, cap, fmt, ap);
}

int rx_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = rx_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

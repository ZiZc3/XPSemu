/*
 * hakuX native core — the GLib subset (native/compat/glib/glib.h).
 */
#include "glib.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32 /* host unit tests */
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

void g_free(gpointer p)
{
    free(p);
}

gpointer g_malloc(gsize n)
{
    if (!n) {
        return NULL;
    }
    void *p = malloc(n);
    if (!p) {
        abort();
    }
    return p;
}

gpointer g_malloc0(gsize n)
{
    if (!n) {
        return NULL;
    }
    void *p = calloc(1, n);
    if (!p) {
        abort();
    }
    return p;
}

gpointer g_realloc(gpointer p, gsize n)
{
    if (!n) {
        free(p);
        return NULL;
    }
    void *q = realloc(p, n);
    if (!q) {
        abort();
    }
    return q;
}

gpointer g_try_malloc(gsize n)
{
    return n ? malloc(n) : NULL;
}

gpointer g_try_malloc0(gsize n)
{
    return n ? calloc(1, n) : NULL;
}

gpointer g_memdup2(gconstpointer p, gsize n)
{
    if (!p || !n) {
        return NULL;
    }
    void *q = g_malloc(n);
    memcpy(q, p, n);
    return q;
}

gchar *g_strdup(const gchar *s)
{
    return s ? g_memdup2(s, strlen(s) + 1) : NULL;
}

gchar *g_strndup(const gchar *s, gsize n)
{
    if (!s) {
        return NULL;
    }
    gchar *d = g_malloc(n + 1);
    strncpy(d, s, n);
    d[n] = 0;
    return d;
}

gchar *g_strdup_vprintf(const gchar *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    const int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        abort();
    }
    char *s = g_malloc((gsize)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}

gchar *g_strdup_printf(const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    return s;
}

gchar *g_build_filename(const gchar *first, ...)
{
    size_t len = 0, cap = 256;
    char *out = g_malloc(cap);
    out[0] = 0;
    va_list ap;
    va_start(ap, first);
    for (const char *part = first; part; part = va_arg(ap, const char *)) {
        size_t n = strlen(part);
        if (len + n + 2 > cap) {
            cap = (len + n + 2) * 2;
            out = g_realloc(out, cap);
        }
        if (len && out[len - 1] != '/' && part[0] != '/') {
            out[len++] = '/';
        }
        memcpy(out + len, part, n + 1);
        len += n;
    }
    va_end(ap);
    return out;
}

gint64 g_get_monotonic_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

gint64 g_get_real_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (gint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void g_usleep(gulong us)
{
    struct timespec ts = {(time_t)(us / 1000000), (long)(us % 1000000) * 1000};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static GError *new_error(const char *what, const char *path)
{
    GError *e = g_malloc0(sizeof(GError));
    e->code = errno;
    e->message = g_strdup_printf("%s %s: %s", what, path, strerror(errno));
    return e;
}

void g_error_free(GError *e)
{
    if (e) {
        g_free(e->message);
        g_free(e);
    }
}

void g_clear_error(GError **e)
{
    if (e && *e) {
        g_error_free(*e);
        *e = NULL;
    }
}

gboolean g_file_get_contents(const gchar *path, gchar **contents, gsize *length, GError **error)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error) {
            *error = new_error("open", path);
        }
        return FALSE;
    }
    size_t cap = 4096, len = 0;
    char *buf = g_malloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len - 1 == 0) {
            cap *= 2;
            buf = g_realloc(buf, cap);
        }
    }
    fclose(f);
    buf[len] = 0;
    *contents = buf;
    if (length) {
        *length = len;
    }
    return TRUE;
}

gboolean g_file_set_contents(const gchar *path, const gchar *contents, gssize length, GError **error)
{
    if (length < 0) {
        length = (gssize)strlen(contents);
    }
    char *tmp = g_strdup_printf("%s.tmp%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(contents, 1, (size_t)length, f) == (size_t)length;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    ok = ok && rename(tmp, path) == 0;
    if (!ok) {
        if (error) {
            *error = new_error("write", path);
        }
        unlink(tmp);
    }
    g_free(tmp);
    return ok;
}

gboolean g_file_test(const gchar *path, int test)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return FALSE;
    }
    return (test & G_FILE_TEST_EXISTS) || ((test & G_FILE_TEST_IS_DIR) && S_ISDIR(st.st_mode)) ||
           ((test & G_FILE_TEST_IS_REGULAR) && S_ISREG(st.st_mode));
}

int g_mkdir_with_parents(const gchar *path, int mode)
{
    char *p = g_strdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, (mode_t)mode);
            *s = '/';
        }
    }
    int r = mkdir(p, (mode_t)mode) == 0 || errno == EEXIST ? 0 : -1;
    g_free(p);
    return r;
}

/* ---------------------------------------------------------- hash tables */

typedef struct Slot {
    gpointer key, value;
    guint hash;
    uint8_t state; /* 0 empty, 1 used, 2 deleted */
} Slot;

struct GHashTable {
    GHashFunc hash;
    GEqualFunc equal;
    GDestroyNotify key_destroy, value_destroy;
    Slot *slots;
    guint cap, used, tomb;
};

GHashTable *g_hash_table_new_full(GHashFunc hash, GEqualFunc equal, GDestroyNotify key_destroy,
                                  GDestroyNotify value_destroy)
{
    GHashTable *t = g_malloc0(sizeof(*t));
    t->hash = hash ? hash : g_direct_hash;
    t->equal = equal;
    t->key_destroy = key_destroy;
    t->value_destroy = value_destroy;
    t->cap = 16;
    t->slots = g_malloc0(sizeof(Slot) * t->cap);
    return t;
}

GHashTable *g_hash_table_new(GHashFunc hash, GEqualFunc equal)
{
    return g_hash_table_new_full(hash, equal, NULL, NULL);
}

static bool same(GHashTable *t, const Slot *s, gconstpointer key, guint h)
{
    return s->hash == h && (t->equal ? t->equal(s->key, key) : s->key == key);
}

static Slot *find(GHashTable *t, gconstpointer key, guint h)
{
    for (guint i = h & (t->cap - 1), n = 0; n < t->cap; i = (i + 1) & (t->cap - 1), n++) {
        Slot *s = &t->slots[i];
        if (s->state == 0) {
            return NULL;
        }
        if (s->state == 1 && same(t, s, key, h)) {
            return s;
        }
    }
    return NULL;
}

static void drop(GHashTable *t, Slot *s)
{
    if (t->key_destroy) {
        t->key_destroy(s->key);
    }
    if (t->value_destroy) {
        t->value_destroy(s->value);
    }
}

static void grow(GHashTable *t)
{
    Slot *old = t->slots;
    guint ocap = t->cap;
    t->cap = (t->used * 4 > ocap) ? ocap * 2 : ocap;
    t->slots = g_malloc0(sizeof(Slot) * t->cap);
    t->tomb = 0;
    for (guint i = 0; i < ocap; i++) {
        if (old[i].state == 1) {
            guint j = old[i].hash & (t->cap - 1);
            while (t->slots[j].state) {
                j = (j + 1) & (t->cap - 1);
            }
            t->slots[j] = old[i];
        }
    }
    g_free(old);
}

static gboolean put(GHashTable *t, gpointer key, gpointer value, bool replace_key)
{
    const guint h = t->hash(key);
    Slot *s = find(t, key, h);
    if (s) {
        if (t->value_destroy && s->value != value) {
            t->value_destroy(s->value);
        }
        if (replace_key) {
            if (t->key_destroy && s->key != key) {
                t->key_destroy(s->key);
            }
            s->key = key;
        } else if (t->key_destroy && s->key != key) {
            t->key_destroy(key);
        }
        s->value = value;
        return FALSE;
    }
    if ((t->used + t->tomb + 1) * 4 > t->cap * 3) {
        grow(t);
    }
    guint i = h & (t->cap - 1);
    while (t->slots[i].state == 1) {
        i = (i + 1) & (t->cap - 1);
    }
    if (t->slots[i].state == 2) {
        t->tomb--;
    }
    t->slots[i] = (Slot){key, value, h, 1};
    t->used++;
    return TRUE;
}

gboolean g_hash_table_insert(GHashTable *t, gpointer key, gpointer value)
{
    return put(t, key, value, false);
}

gboolean g_hash_table_replace(GHashTable *t, gpointer key, gpointer value)
{
    return put(t, key, value, true);
}

gboolean g_hash_table_add(GHashTable *t, gpointer key)
{
    return put(t, key, key, true);
}

gpointer g_hash_table_lookup(GHashTable *t, gconstpointer key)
{
    Slot *s = find(t, key, t->hash(key));
    return s ? s->value : NULL;
}

gboolean g_hash_table_contains(GHashTable *t, gconstpointer key)
{
    return find(t, key, t->hash(key)) != NULL;
}

gboolean g_hash_table_remove(GHashTable *t, gconstpointer key)
{
    Slot *s = find(t, key, t->hash(key));
    if (!s) {
        return FALSE;
    }
    drop(t, s);
    s->state = 2;
    t->used--;
    t->tomb++;
    return TRUE;
}

void g_hash_table_remove_all(GHashTable *t)
{
    for (guint i = 0; i < t->cap; i++) {
        if (t->slots[i].state == 1) {
            drop(t, &t->slots[i]);
        }
        t->slots[i].state = 0;
    }
    t->used = t->tomb = 0;
}

void g_hash_table_destroy(GHashTable *t)
{
    if (t) {
        g_hash_table_remove_all(t);
        g_free(t->slots);
        g_free(t);
    }
}

guint g_hash_table_size(GHashTable *t)
{
    return t->used;
}

void g_hash_table_foreach(GHashTable *t, GHFunc fn, gpointer user_data)
{
    for (guint i = 0; i < t->cap; i++) {
        if (t->slots[i].state == 1) {
            fn(t->slots[i].key, t->slots[i].value, user_data);
        }
    }
}

guint g_direct_hash(gconstpointer v)
{
    uintptr_t x = (uintptr_t)v;
    return (guint)(x ^ (x >> 32)) * 2654435761u;
}

gboolean g_direct_equal(gconstpointer a, gconstpointer b)
{
    return a == b;
}

guint g_int_hash(gconstpointer v)
{
    return (guint)(*(const gint *)v) * 2654435761u;
}

gboolean g_int_equal(gconstpointer a, gconstpointer b)
{
    return *(const gint *)a == *(const gint *)b;
}

guint g_int64_hash(gconstpointer v)
{
    const guint64 x = *(const guint64 *)v;
    return (guint)(x ^ (x >> 32)) * 2654435761u;
}

gboolean g_int64_equal(gconstpointer a, gconstpointer b)
{
    return *(const gint64 *)a == *(const gint64 *)b;
}

guint g_str_hash(gconstpointer v)
{
    guint h = 5381;
    for (const unsigned char *p = v; *p; p++) {
        h = h * 33 + *p;
    }
    return h;
}

gboolean g_str_equal(gconstpointer a, gconstpointer b)
{
    return strcmp(a, b) == 0;
}

void g_assertion_failed(const char *file, int line, const char *expr)
{
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}

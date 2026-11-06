/*
 * hakuX native core — the subset of GLib the shared HLE layer uses, so it
 * builds without GLib (native/compat/glib.c). Behavior follows GLib's
 * documented contracts for these calls.
 */
#ifndef NATIVE_COMPAT_GLIB_H
#define NATIVE_COMPAT_GLIB_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int gboolean;
typedef char gchar;
typedef unsigned char guchar;
typedef int gint;
typedef unsigned int guint;
typedef int8_t gint8;
typedef uint8_t guint8;
typedef int16_t gint16;
typedef uint16_t guint16;
typedef int32_t gint32;
typedef uint32_t guint32;
typedef int64_t gint64;
typedef uint64_t guint64;
typedef long glong;
typedef unsigned long gulong;
typedef size_t gsize;
typedef ptrdiff_t gssize;
typedef void *gpointer;
typedef const void *gconstpointer;
typedef double gdouble;
typedef float gfloat;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define G_GNUC_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define G_GNUC_UNUSED __attribute__((unused))
#define G_GNUC_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#define G_LIKELY(x) __builtin_expect(!!(x), 1)
#define G_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define G_N_ELEMENTS(a) (sizeof(a) / sizeof((a)[0]))
#define G_STRINGIFY(x) G_STRINGIFY_(x)
#define G_STRINGIFY_(x) #x
#define G_USEC_PER_SEC 1000000

void g_free(gpointer p);
gpointer g_malloc(gsize n);
gpointer g_malloc0(gsize n);
gpointer g_realloc(gpointer p, gsize n);
gpointer g_try_malloc(gsize n);
gpointer g_try_malloc0(gsize n);
gpointer g_memdup2(gconstpointer p, gsize n);
gchar *g_strdup(const gchar *s);
gchar *g_strndup(const gchar *s, gsize n);
gchar *g_strdup_printf(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);
gchar *g_strdup_vprintf(const gchar *fmt, va_list ap);
gchar *g_build_filename(const gchar *first, ...);

static inline gpointer g_malloc_n(gsize n, gsize size)
{
    return g_malloc(n * size);
}
static inline gpointer g_malloc0_n(gsize n, gsize size)
{
    return g_malloc0(n * size);
}
#define g_new(T, n) ((T *)g_malloc(sizeof(T) * (gsize)(n)))
#define g_new0(T, n) ((T *)g_malloc0(sizeof(T) * (gsize)(n)))
#define g_renew(T, p, n) ((T *)g_realloc((p), sizeof(T) * (gsize)(n)))

static inline void g_autoptr_cleanup_free(void *p)
{
    g_free(*(void **)p);
}
#define g_autofree __attribute__((cleanup(g_autoptr_cleanup_free)))

gint64 g_get_monotonic_time(void);
gint64 g_get_real_time(void);
void g_usleep(gulong us);

typedef struct GError {
    guint32 domain;
    gint code;
    gchar *message;
} GError;
void g_error_free(GError *e);
void g_clear_error(GError **e);

gboolean g_file_get_contents(const gchar *path, gchar **contents, gsize *length, GError **error);
gboolean g_file_set_contents(const gchar *path, const gchar *contents, gssize length, GError **error);
gboolean g_file_test(const gchar *path, int test);
#define G_FILE_TEST_EXISTS 16
#define G_FILE_TEST_IS_DIR 4
#define G_FILE_TEST_IS_REGULAR 1
int g_mkdir_with_parents(const gchar *path, int mode);

/* Hash tables: open addressing over the given hash and equality. */
typedef guint (*GHashFunc)(gconstpointer key);
typedef gboolean (*GEqualFunc)(gconstpointer a, gconstpointer b);
typedef void (*GDestroyNotify)(gpointer data);
typedef void (*GHFunc)(gpointer key, gpointer value, gpointer user_data);
typedef struct GHashTable GHashTable;

GHashTable *g_hash_table_new(GHashFunc hash, GEqualFunc equal);
GHashTable *g_hash_table_new_full(GHashFunc hash, GEqualFunc equal, GDestroyNotify key_destroy,
                                  GDestroyNotify value_destroy);
void g_hash_table_destroy(GHashTable *t);
gboolean g_hash_table_insert(GHashTable *t, gpointer key, gpointer value);
gboolean g_hash_table_replace(GHashTable *t, gpointer key, gpointer value);
gboolean g_hash_table_add(GHashTable *t, gpointer key);
gpointer g_hash_table_lookup(GHashTable *t, gconstpointer key);
gboolean g_hash_table_contains(GHashTable *t, gconstpointer key);
gboolean g_hash_table_remove(GHashTable *t, gconstpointer key);
void g_hash_table_remove_all(GHashTable *t);
guint g_hash_table_size(GHashTable *t);
void g_hash_table_foreach(GHashTable *t, GHFunc fn, gpointer user_data);

guint g_direct_hash(gconstpointer v);
gboolean g_direct_equal(gconstpointer a, gconstpointer b);
guint g_int_hash(gconstpointer v);
gboolean g_int_equal(gconstpointer a, gconstpointer b);
guint g_int64_hash(gconstpointer v);
gboolean g_int64_equal(gconstpointer a, gconstpointer b);
guint g_str_hash(gconstpointer v);
gboolean g_str_equal(gconstpointer a, gconstpointer b);

#define GINT_TO_POINTER(i) ((gpointer)(intptr_t)(i))
#define GPOINTER_TO_INT(p) ((gint)(intptr_t)(p))
#define GUINT_TO_POINTER(u) ((gpointer)(uintptr_t)(u))
#define GPOINTER_TO_UINT(p) ((guint)(uintptr_t)(p))

void g_assertion_failed(const char *file, int line, const char *expr) __attribute__((noreturn));
#define g_assert(e) ((e) ? (void)0 : g_assertion_failed(__FILE__, __LINE__, #e))
#define g_assert_not_reached() g_assertion_failed(__FILE__, __LINE__, "not reached")

#ifdef __cplusplus
}
#endif

#endif

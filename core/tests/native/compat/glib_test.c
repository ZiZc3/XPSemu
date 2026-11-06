/*
 * Host test of native/compat/glib.c (the GLib subset the HLE layer uses):
 * hash tables against a reference array under inserts, replaces, removes and
 * regrowth; string and file helpers.
 *
 *   gcc -O1 -Inative/compat/glib tests/native/compat/glib_test.c native/compat/glib/glib.c -o glib_test
 */
#include "glib.h"

#include <stdio.h>

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static unsigned g_freed_keys, g_freed_values;
static void free_key(gpointer p)
{
    g_freed_keys++;
    g_free(p);
}
static void free_value(gpointer p)
{
    g_freed_values++;
}

int main(void)
{
    /* Direct keys against a reference table, with removes forcing tombstones. */
    enum { N = 5000 };
    static int ref[N];
    GHashTable *t = g_hash_table_new(g_direct_hash, g_direct_equal);
    uint32_t x = 12345;
    for (int round = 0; round < 200000; round++) {
        x = x * 1103515245u + 12345u;
        const int key = (int)((x >> 8) % N) + 1;
        const int op = (int)(x >> 28) & 3;
        if (op < 2) {
            g_hash_table_replace(t, GINT_TO_POINTER(key), GINT_TO_POINTER(round + 1));
            ref[key - 1] = round + 1;
        } else if (op == 2) {
            const bool had = ref[key - 1] != 0;
            CHECK(g_hash_table_remove(t, GINT_TO_POINTER(key)) == had);
            ref[key - 1] = 0;
        } else {
            CHECK(GPOINTER_TO_INT(g_hash_table_lookup(t, GINT_TO_POINTER(key))) == ref[key - 1]);
        }
    }
    unsigned live = 0;
    for (int k = 0; k < N; k++) {
        live += ref[k] != 0;
        CHECK(g_hash_table_contains(t, GINT_TO_POINTER(k + 1)) == (ref[k] != 0));
    }
    CHECK(g_hash_table_size(t) == live);
    g_hash_table_destroy(t);

    /* String keys with destroy notifiers: replace frees the old key and value. */
    GHashTable *s = g_hash_table_new_full(g_str_hash, g_str_equal, free_key, free_value);
    g_hash_table_insert(s, g_strdup("a"), GINT_TO_POINTER(1));
    g_hash_table_replace(s, g_strdup("a"), GINT_TO_POINTER(2));
    CHECK(g_freed_keys == 1 && g_freed_values == 1);
    CHECK(GPOINTER_TO_INT(g_hash_table_lookup(s, "a")) == 2);
    g_hash_table_insert(s, g_strdup("a"), GINT_TO_POINTER(3)); /* insert keeps the old key */
    CHECK(g_freed_keys == 2 && g_freed_values == 2);
    g_hash_table_destroy(s);
    CHECK(g_freed_keys == 3 && g_freed_values == 3);

    gchar *p = g_strdup_printf("%s-%d", "x", 42);
    CHECK(!strcmp(p, "x-42"));
    g_free(p);
    gchar *b = g_build_filename("a", "b/", "c", NULL);
    CHECK(!strcmp(b, "a/b/c"));
    g_free(b);

    const char *path = "glib_test.tmp";
    CHECK(g_file_set_contents(path, "hello", -1, NULL));
    gchar *c = NULL;
    gsize len = 0;
    CHECK(g_file_get_contents(path, &c, &len, NULL) && len == 5 && !strcmp(c, "hello"));
    g_free(c);
    remove(path);

    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}

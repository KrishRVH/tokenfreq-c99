#include "wordcount.h"

#include <stdio.h>

#define CHECK(expr)                                    \
    do {                                               \
        if (!(expr)) {                                 \
            (void)fprintf(stderr,                      \
                          "CHECK failed: %s:%d: %s\n", \
                          __FILE__,                    \
                          __LINE__,                    \
                          #expr);                      \
            return 1;                                  \
        }                                              \
    } while (0)

#if !WC_BOOL(WC_NO_HEAP)
int main(void)
{
    (void)fprintf(stderr,
                  "tests/noheap_static.c must be built with WC_NO_HEAP=1\n");
    return 1;
}
#else

static int test_tiny_static_fails(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    int rc = WC_OK;
    WC_STATIC_BUFFER(tiny, 32);
    wc *w;

    lim.static_buf = tiny.buf;
    lim.static_size = sizeof tiny.buf;

    w = wc_open_ex(0, &lim, &rc);
    CHECK(w == NULL);
    CHECK(rc == WC_EBADLIMITS || rc == WC_NOMEM);
    return 0;
}

static int test_omitted_handle_uses_static_buffer(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    int rc = WC_ERROR;
    WC_STATIC_BUFFER(pool, 65536);
    wc *w;

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(32, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);
    CHECK(wc_add_norm_n(w, "Alpha", 5) == WC_OK);
    CHECK(wc_add_norm_n(w, "alpha", 5) == WC_OK);
    CHECK(wc_total(w) == 2);
    CHECK(wc_unique(w) == 1);
    wc_close(w);
    return 0;
}

static int test_one_max_word_fits(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    int rc = WC_ERROR;
    char word[WC_MAX_WORD];
    size_t i;
    WC_STATIC_BUFFER(pool, 65536);
    wc *w;

    for (i = 0; i < sizeof word; i++)
        word[i] = (char)('a' + (int)(i % 26u));

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(WC_MAX_WORD, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);
    CHECK(wc_add_n(w, word, sizeof word) == WC_OK);
    CHECK(wc_total(w) == 1);
    CHECK(wc_unique(w) == 1);
    wc_close(w);
    return 0;
}

static int test_result_allocators_validate_args(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    int rc = WC_ERROR;
    WC_STATIC_BUFFER(pool, 65536);
    wc_word *out = (wc_word *)0x1;
    size_t n = 123;
    wc *w;

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(32, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);

    CHECK(wc_results(NULL, &out, &n) == WC_ERROR);
    CHECK(wc_results(w, NULL, &n) == WC_ERROR);
    CHECK(wc_results(w, &out, NULL) == WC_ERROR);
    CHECK(wc_results(w, &out, &n) == WC_NOMEM);
    CHECK(out == NULL);
    CHECK(n == 0);

    out = (wc_word *)0x1;
    n = 123;
    CHECK(wc_topn(NULL, 1, &out, &n) == WC_ERROR);
    CHECK(wc_topn(w, 1, NULL, &n) == WC_ERROR);
    CHECK(wc_topn(w, 1, &out, NULL) == WC_ERROR);
    CHECK(wc_topn(w, 1, &out, &n) == WC_NOMEM);
    CHECK(out == NULL);
    CHECK(n == 0);

    wc_close(w);
    return 0;
}

static int test_cursor_enumerates_without_heap_results(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    int rc = WC_ERROR;
    size_t seen = 0;
    size_t total = 0;
    wc_cursor cursor;
    WC_STATIC_BUFFER(pool, 65536);
    wc *w;

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(32, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);
    CHECK(wc_add(w, "alpha") == WC_OK);
    CHECK(wc_add(w, "beta") == WC_OK);
    CHECK(wc_add(w, "beta") == WC_OK);

    wc_cursor_init(&cursor, w);
    for (;;) {
        const char *word = NULL;
        size_t count = 0;
        if (!wc_cursor_next(&cursor, &word, &count))
            break;
        CHECK(word != NULL);
        CHECK(count > 0);
        CHECK(total <= WC_SIZE_MAX - count);
        total += count;
        seen++;
    }

    CHECK(seen == wc_unique(w));
    CHECK(total == wc_total(w));
    wc_close(w);
    return 0;
}

static int test_inplace_stream_counts_without_heap(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    int rc = WC_ERROR;
    size_t need;
    wc_stream *s;
    WC_STATIC_BUFFER(pool, 65536);
    WC_STATIC_BUFFER(stream_storage, 4096);
    wc *w;

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(32, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);

    need = wc_stream_size(w);
    CHECK(need > 0);
    CHECK(need <= sizeof stream_storage.buf);

#if !WC_BOOL(WC_HAVE_UINTPTR)
    s = wc_stream_open_inplace(w, stream_storage.buf, need - 1u, &rc);
    CHECK(s == NULL);
    CHECK(rc == WC_EBADLIMITS);
    s = wc_stream_open_inplace(
            w, stream_storage.buf, sizeof stream_storage.buf, &rc);
    CHECK(s == NULL);
    CHECK(rc == WC_EBADLIMITS);
    s = wc_stream_open_inplace(w, (void *)w, need, &rc);
    CHECK(s == NULL);
    CHECK(rc == WC_EBADLIMITS);

    wc_close(w);
    return 0;
#else
    s = wc_stream_open_inplace(
            w, stream_storage.buf, sizeof stream_storage.buf, &rc);
    CHECK(s != NULL);
    CHECK(rc == WC_OK);
    CHECK(wc_stream_scan(s, "Alpha ", 6) == WC_OK);
    CHECK(wc_stream_scan(s, "beta alpha", 10) == WC_OK);
    CHECK(wc_stream_finish(s) == WC_OK);
    wc_stream_close(s);

    CHECK(wc_total(w) == 3);
    CHECK(wc_unique(w) == 2);

    wc_close(w);
    return 0;
#endif
}

int main(void)
{
    CHECK(test_tiny_static_fails() == 0);
    CHECK(test_omitted_handle_uses_static_buffer() == 0);
    CHECK(test_one_max_word_fits() == 0);
    CHECK(test_result_allocators_validate_args() == 0);
    CHECK(test_cursor_enumerates_without_heap_results() == 0);
    CHECK(test_inplace_stream_counts_without_heap() == 0);
    return 0;
}
#endif

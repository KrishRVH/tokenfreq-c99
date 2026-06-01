/*
** wc_test.c - Test suite + optional fuzz harness
**
** Public domain.
**
** DESIGN
**
**   - Happy path functionality
**   - Edge cases and boundary conditions
**   - Deterministic regression tests for collision-length hazards
**   - Static-buffer and max_bytes limit behavior
**   - Cursor/invariant checks without allocations (useful after NOMEM)
**
** OOM INJECTION
**
**   Same glibc-specific approach as before (malloc/realloc interpose).
**
** FUZZING (libFuzzer)
**
**   Build example:
**     clang -std=c99 -O1 -g -fsanitize=address,undefined,fuzzer \
**       -DWC_TEST_FUZZ wordcount.c wc_test.c -o wc_fuzz
**
**   Or standalone fuzz runner from stdin:
**     clang -std=c99 -O1 -g -fsanitize=address,undefined \
**       -DWC_TEST_FUZZ -DWC_TEST_FUZZ_STANDALONE \
**       wordcount.c wc_test.c -o wc_fuzz_stdin
**     cat corpus.bin | ./wc_fuzz_stdin
*/
#ifndef WC_NO_TEST_MAIN

#include "wordcount.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define WC_TEST_ALIGNOF(T) _Alignof(T)
#elif defined(__GNUC__) || defined(__clang__)
#define WC_TEST_ALIGNOF(T) __alignof__(T)
#else
#define WC_TEST_ALIGNOF(T) \
    ((size_t)offsetof(     \
            struct {       \
                char c;    \
                T x;       \
            },             \
            x))
#endif

/* Match the public alignment contract (WC_STATIC_BUFFER) to avoid drift. */
typedef struct {
    WC_STATIC_BUFFER(buf, 1);
} wc_test_align_wrapper;

#define WC_TEST_ALIGN WC_TEST_ALIGNOF(wc_test_align_wrapper)

static int g_run, g_pass, g_fail;

#define TEST(name)                \
    do {                          \
        g_run++;                  \
        printf("  %-55s ", name); \
        (void)fflush(stdout);     \
    } while (0)

#define PASS()            \
    do {                  \
        g_pass++;         \
        printf("[OK]\n"); \
    } while (0)

#define FAIL(m)                   \
    do {                          \
        g_fail++;                 \
        printf("[FAIL] %s\n", m); \
    } while (0)

#define ASSERT(c)     \
    do {              \
        if (!(c)) {   \
            FAIL(#c); \
            return 1; \
        }             \
    } while (0)

/* Aligned buffer helper for wc_limits.static_buf */
#define WC_TEST_STATIC_BUF(name, size_) WC_STATIC_BUFFER(name, size_)

/* --- OOM injection framework (glibc-specific) -------------------------- */

#if defined(WC_TEST_OOM) && defined(__GLIBC__)

static int oom_target = 0;
static int oom_count = 0;
static int oom_active = 0;

static void oom_reset(void)
{
    oom_target = 0;
    oom_count = 0;
    oom_active = 0;
}

static void oom_arm(int n)
{
    oom_target = n;
    oom_count = 0;
    oom_active = 1;
}

static int oom_check(void)
{
    if (!oom_active)
        return 0;
    oom_count++;
    if (oom_count == oom_target) {
        oom_active = 0;
        return 1;
    }
    return 0;
}

void *malloc(size_t n)
{
    extern void *__libc_malloc(size_t);
    if (oom_check()) {
        errno = ENOMEM;
        return NULL;
    }
    return __libc_malloc(n);
}

void *calloc(size_t nm, size_t sz)
{
    if (nm != 0 && sz > SIZE_MAX / nm) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(nm * sz);
    if (p)
        memset(p, 0, nm * sz);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    extern void *__libc_realloc(void *, size_t);
    if (oom_check()) {
        errno = ENOMEM;
        return NULL;
    }
    return __libc_realloc(ptr, n);
}

#else

#if defined(__GNUC__) || defined(__clang__)
#define OOM_UNUSED __attribute__((unused))
#else
#define OOM_UNUSED
#endif

OOM_UNUSED static void oom_reset(void) {}
OOM_UNUSED static void oom_arm(int n)
{
    (void)n;
}

#endif /* WC_TEST_OOM && __GLIBC__ */

/* --- Invariant checks -------------------------------------------------- */

static int invariant_cursor_sum_matches_total(const wc *w)
{
    wc_cursor c;
    size_t seen = 0;
    size_t sum = 0;

    wc_cursor_init(&c, w);

    for (;;) {
        const char *word = NULL;
        size_t count = 0;

        if (!wc_cursor_next(&c, &word, &count))
            break;

        /* Basic sanity */
        if (!word || count == 0)
            return 0;

        seen++;
        if (sum > WC_SIZE_MAX - count)
            return 0;
        sum += count;
    }

    return (seen == wc_unique(w)) && (sum == wc_total(w));
}

static int test_static_buf_misaligned_rejected(void)
{
    wc_limits lim;
    /* Over-allocate so we can intentionally misalign by +1 byte. */
    WC_TEST_STATIC_BUF(pool, 4096 + 64);

    TEST("static_buf: misaligned buffer rejected");

#if !(WC_HAVE_UINTPTR + 0) && (WC_TRUST_STATIC_BUFFER_ALIGNMENT + 0)
    (void)lim;
    (void)pool.buf;
    PASS();
    return 0;
#else
    wc_limits_init(&lim);
    lim.static_buf = (void *)(pool.buf + 1); /* misaligned */
    lim.static_size = sizeof pool.buf - 1;   /* still valid range */

    if (WC_TEST_ALIGN > 1) {
        ASSERT(wc_open_ex(0, &lim, NULL) == NULL);
    } else {
        /* Extremely unusual platforms might have alignment==1; accept either. */
        wc *w = wc_open_ex(0, &lim, NULL);
        ASSERT(w != NULL);
        wc_close(w);
    }

    PASS();
    return 0;
#endif
}

static int test_full_table_dynamic_grows_instead_of_error(void)
{
    /* This only truly exercises the "table is completely full" path when the
       compile-time minimum init cap is tiny (1 or 2). For normal builds where
       WC_MIN_INIT_CAP is 8/16+, it becomes a no-op/skip-like pass. */
#if WC_MIN_INIT_CAP <= 2
    wc_limits lim;
    wc *w;
    const char *keys[] = { "a", "b", "c" };
    size_t i;

    TEST("dynamic: full table grows (requires WC_MIN_INIT_CAP<=2)");

    wc_limits_init(&lim);
    lim.init_cap = 1; /* will round/floor to WC_MIN_INIT_CAP (1 or 2) */

    w = wc_open_ex(4, &lim, NULL); /* tiny max_word keeps arena needs small */
    ASSERT(w != NULL);

    /* Fill the initial table completely (cap == WC_MIN_INIT_CAP here). */
    for (i = 0; i < (size_t)WC_MIN_INIT_CAP; i++) {
        ASSERT(wc_add(w, keys[i]) == WC_OK);
    }

    /* One more unique forces the full-table path: must grow + succeed. */
    ASSERT(wc_add(w, keys[WC_MIN_INIT_CAP]) == WC_OK);

    ASSERT(wc_unique(w) == (size_t)WC_MIN_INIT_CAP + 1);
    ASSERT(wc_total(w) == (size_t)WC_MIN_INIT_CAP + 1);
    ASSERT(invariant_cursor_sum_matches_total(w));

    wc_close(w);
    PASS();
    return 0;
#else
    TEST("dynamic: full table grows (requires WC_MIN_INIT_CAP<=2)");
    PASS();
    return 0;
#endif
}

static int test_full_table_static_returns_nomem_not_error(void)
{
#if WC_MIN_INIT_CAP <= 2
    wc_limits lim;
    WC_TEST_STATIC_BUF(pool, 4096);
    wc *w;
    const char *keys[] = { "a", "b", "c" };
    size_t i;
    int rc;

    TEST("static: full table returns NOMEM (requires WC_MIN_INIT_CAP<=2)");

    wc_limits_init(&lim);
    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;
    lim.init_cap = 1; /* floors to WC_MIN_INIT_CAP (1 or 2) */

    w = wc_open_ex(4, &lim, NULL);
    ASSERT(w != NULL);

    /* Fill the initial table (cap == WC_MIN_INIT_CAP). */
    for (i = 0; i < (size_t)WC_MIN_INIT_CAP; i++) {
        ASSERT(wc_add(w, keys[i]) == WC_OK);
    }

    /* Next unique insert: must be WC_NOMEM (not WC_ERROR). */
    rc = wc_add(w, keys[WC_MIN_INIT_CAP]);
    ASSERT(rc == WC_NOMEM);

    /* Must still be valid and allow duplicates to increment. */
    {
        size_t total_before = wc_total(w);
        ASSERT(wc_add(w, keys[0]) == WC_OK);
        ASSERT(wc_total(w) == total_before + 1);
        ASSERT(wc_unique(w) == (size_t)WC_MIN_INIT_CAP);
    }

    /* Duplicates-only scan should also succeed after exhaustion. */
    {
        const char *t = "A a a"; /* wc_scan lowercases -> "a" */
        int rc2 = wc_scan(w, t, strlen(t));
        ASSERT(rc2 == WC_OK);
    }

    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
#else
    TEST("static: full table returns NOMEM (requires WC_MIN_INIT_CAP<=2)");
    PASS();
    return 0;
#endif
}

/* --- Lifecycle / Limits tests ----------------------------------------- */

static int test_open_close(void)
{
    wc *w;
    TEST("open and close");
    w = wc_open(0);
    ASSERT(w != NULL);
    ASSERT(wc_total(w) == 0);
    ASSERT(wc_unique(w) == 0);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_close_null(void)
{
    TEST("close NULL");
    wc_close(NULL);
    PASS();
    return 0;
}

static int test_max_word_clamp(void)
{
    wc *w;
    TEST("max_word clamping");
    w = wc_open(1);
    ASSERT(w != NULL);
    wc_close(w);
    w = wc_open(9999);
    ASSERT(w != NULL);
    wc_close(w);
    PASS();
    return 0;
}

static int test_max_word_clamped_to_wc_max_word(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    size_t i;
    char word[WC_MAX_WORD + 16];

    TEST("max_word clamped to WC_MAX_WORD");

    for (i = 0; i < WC_MAX_WORD + 8; i++)
        word[i] = 'a';
    word[WC_MAX_WORD + 8] = '\0';

    w = wc_open(WC_MAX_WORD + 1000);
    ASSERT(w != NULL);
    ASSERT(wc_add(w, word) == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 1);
    ASSERT(strlen(r[0].word) == WC_MAX_WORD);

    wc_results_free(r);
    wc_close(w);

    PASS();
    return 0;
}

static int test_static_accepts_one_max_word(void)
{
    WC_TEST_STATIC_BUF(pool, 131072 + WC_MAX_WORD + WC_TEST_ALIGN);
    wc_limits lim = WC_LIMITS_INIT();
    wc *w;
    int rc = WC_OK;
    char word[WC_MAX_WORD + 1u];

    TEST("static_buf: one max_word word fits after open");

    for (size_t i = 0; i < WC_MAX_WORD; i++)
        word[i] = 'a';
    word[WC_MAX_WORD] = '\0';

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(WC_MAX_WORD, &lim, &rc);
    ASSERT(w != NULL);
    ASSERT(rc == WC_OK);
    ASSERT(wc_add(w, word) == WC_OK);
    ASSERT(wc_total(w) == 1);
    ASSERT(wc_unique(w) == 1);
    ASSERT(invariant_cursor_sum_matches_total(w));

    wc_close(w);
    PASS();
    return 0;
}

static int test_static_default_arena_uses_remaining_budget(void)
{
    WC_TEST_STATIC_BUF(pool, 8192);
    wc_limits lim = WC_LIMITS_INIT();
    wc *w;
    int rc = WC_OK;
    char word[16];

    TEST("static_buf: default arena uses remaining budget");

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(8, &lim, &rc);
    ASSERT(w != NULL);
    ASSERT(rc == WC_OK);

    for (size_t i = 0; i < 80; i++) {
        (void)snprintf(word, sizeof word, "w%03zu", i);
        ASSERT(wc_add(w, word) == WC_OK);
    }
    ASSERT(wc_unique(w) == 80);
    ASSERT(invariant_cursor_sum_matches_total(w));

    wc_close(w);
    PASS();
    return 0;
}

static int infer_slot_size(size_t *out)
{
    wc_limits lim;
    wc_stats a_st;
    wc_stats b_st;
    wc *a = NULL;
    wc *b = NULL;
    int ok = 0;

    if (!out)
        return 0;

    wc_limits_init(&lim);
    lim.init_cap = 16;
    lim.block_size = 4096;
    a = wc_open_ex(8, &lim, NULL);
    if (!a || wc_get_stats(a, &a_st) != WC_OK)
        goto done;

    lim.init_cap = 32;
    b = wc_open_ex(8, &lim, NULL);
    if (!b || wc_get_stats(b, &b_st) != WC_OK)
        goto done;

    if (b_st.cap <= a_st.cap || b_st.bytes_used <= a_st.bytes_used)
        goto done;
    if ((b_st.bytes_used - a_st.bytes_used) % (b_st.cap - a_st.cap) != 0)
        goto done;

    *out = (b_st.bytes_used - a_st.bytes_used) / (b_st.cap - a_st.cap);
    ok = *out > 0;

done:
    wc_close(a);
    wc_close(b);
    return ok;
}

static int test_static_explicit_block_size_is_honored(void)
{
    WC_TEST_STATIC_BUF(pool, 8192);
    wc_limits lim = WC_LIMITS_INIT();
    wc_stats st;
    wc *w;
    int rc = WC_OK;
    size_t table_bytes;

    TEST("static_buf: explicit block_size is honored");

    ASSERT(infer_slot_size(&table_bytes));

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;
    lim.init_cap = 128;
    lim.block_size = 3000;

    w = wc_open_ex(8, &lim, &rc);
    ASSERT(w != NULL);
    ASSERT(rc == WC_OK);
    ASSERT(wc_get_stats(w, &st) == WC_OK);

    table_bytes *= st.cap;
    ASSERT(st.cap == 128);
    ASSERT(st.bytes_used >= table_bytes + lim.block_size);

    wc_close(w);
    PASS();
    return 0;
}

static int test_grow_preserves_existing_keys(void)
{
    wc_limits lim;
    wc_stats st0, st1;
    wc *w;

    TEST("grow: re-adding an existing key increments (no duplicate entry)");

    wc_limits_init(&lim);
    lim.init_cap = 8;
    lim.block_size = 512;

    w = wc_open_ex(32, &lim, NULL);
    ASSERT(w != NULL);
    ASSERT(wc_get_stats(w, &st0) == WC_OK);
    ASSERT(st0.cap >= 8);

    ASSERT(wc_add(w, "k0") == WC_OK);
    ASSERT(wc_add(w, "k1") == WC_OK);
    ASSERT(wc_add(w, "k2") == WC_OK);
    ASSERT(wc_add(w, "k3") == WC_OK);
    ASSERT(wc_add(w, "k4") == WC_OK);
    ASSERT(wc_add(w, "k5") == WC_OK);

    ASSERT(wc_get_stats(w, &st1) == WC_OK);

    ASSERT(wc_add(w, "k6") == WC_OK);

    {
        wc_stats st2;
        ASSERT(wc_get_stats(w, &st2) == WC_OK);
        ASSERT(st2.cap >= st1.cap);
    }

    {
        const size_t u0 = wc_unique(w);
        const size_t t0 = wc_total(w);
        const int rc = wc_add(w, "k0");
        ASSERT(rc == WC_OK);
        ASSERT(wc_unique(w) == u0);
        ASSERT(wc_total(w) == t0 + 1);
    }

    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_open_ex_null_limits(void)
{
    wc *w;
    TEST("open_ex NULL limits");
    w = wc_open_ex(0, NULL, NULL);
    ASSERT(w != NULL);
    ASSERT(wc_total(w) == 0);
    ASSERT(wc_unique(w) == 0);
    wc_close(w);
    PASS();
    return 0;
}

static int test_limits_struct_size_invalid_rejected(void)
{
    int rc = WC_OK;

    TEST("limits: invalid struct_size rejected");

    /* struct_size = 0 */
    {
        struct {
            size_t struct_size;
        } lim;
        lim.struct_size = 0;
        ASSERT(wc_open_ex(0, (const wc_limits *)&lim, &rc) == NULL);
        ASSERT(rc == WC_EBADLIMITS);
    }

    /* struct_size too small to even contain size_t */
    {
        struct {
            size_t struct_size;
        } lim;
        lim.struct_size = 1;
        ASSERT(wc_open_ex(0, (const wc_limits *)&lim, &rc) == NULL);
        ASSERT(rc == WC_EBADLIMITS);
    }

    PASS();
    return 0;
}

static int test_limits_struct_size_smaller_accepted(void)
{
    /* Simulate an older wc_limits prefix (struct_size + max_bytes). */
    typedef struct {
        size_t struct_size;
        size_t max_bytes;
    } wc_limits_v0;

    int rc = WC_OK;

    TEST("limits: smaller struct_size accepted (prefix copy)");

    {
        wc *w;
        wc_limits_v0 lim;
        lim.struct_size = sizeof lim;
        lim.max_bytes = 0;
        w = wc_open_ex(0, (const wc_limits *)&lim, &rc);
        ASSERT(w != NULL);
        ASSERT(rc == WC_OK);
        wc_close(w);
    }

    {
        wc_limits_v0 lim;
        lim.struct_size = sizeof lim;
        lim.max_bytes = 1; /* deterministically impossible */
        rc = WC_OK;
        ASSERT(wc_open_ex(0, (const wc_limits *)&lim, &rc) == NULL);
        ASSERT(rc == WC_EBADLIMITS);
    }

    PASS();
    return 0;
}

static int test_limits_struct_size_larger_accepted(void)
{
    /* Simulate a newer wc_limits with unknown tail bytes. */
    typedef struct {
        wc_limits base;
        unsigned long long extra_tail;
    } wc_limits_plus;

    wc_limits_plus lim;
    int rc = WC_OK;
    wc *w;

    TEST("limits: larger struct_size accepted (ignore tail)");

    memset(&lim, 0, sizeof lim);
    lim.base.struct_size = sizeof lim; /* larger than sizeof(wc_limits) */
    lim.extra_tail = 0x1122334455667788ULL;

    w = wc_open_ex(0, (const wc_limits *)&lim, &rc);
    ASSERT(w != NULL);
    ASSERT(rc == WC_OK);
    wc_close(w);

    PASS();
    return 0;
}

static int test_open_ex_tiny_budget_fail(void)
{
    wc_limits lim;
    TEST("open_ex tiny max_bytes fails");
    wc_limits_init(&lim);
    lim.max_bytes = 1;
    ASSERT(wc_open_ex(0, &lim, NULL) == NULL);
    PASS();
    return 0;
}

static int test_open_ex_tiny_budget_fail_reason(void)
{
    wc_limits lim;
    int rc = WC_OK;
    TEST("open_ex tiny max_bytes fails with EBADLIMITS");
    wc_limits_init(&lim);
    lim.max_bytes = 1;
    ASSERT(wc_open_ex(0, &lim, &rc) == NULL);
    ASSERT(rc == WC_EBADLIMITS);
    PASS();
    return 0;
}

static int test_limits_invalid_combinations(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    int rc = WC_OK;

    TEST("limits: invalid combinations rejected deterministically");

    /* static_buf without static_size */
    wc_limits_init(&lim);
    {
        static WC_TEST_STATIC_BUF(dummy, WC_TEST_ALIGN);
        lim.static_buf = dummy.buf;
    }
    lim.static_size = 0;
    ASSERT(wc_open_ex(0, &lim, &rc) == NULL);
    ASSERT(rc == WC_EBADLIMITS);

    /* static_size without static_buf */
    wc_limits_init(&lim);
    lim.static_buf = NULL;
    lim.static_size = 64;
    ASSERT(wc_open_ex(0, &lim, &rc) == NULL);
    ASSERT(rc == WC_EBADLIMITS);

    PASS();
    return 0;
}

static int test_open_ex_misaligned_rejected_reason(void)
{
    wc_limits lim;
    int rc = WC_OK;
    WC_TEST_STATIC_BUF(pool, 4096 + 64);

    TEST("open_ex static_buf misalignment returns EALIGN");
#if !(WC_HAVE_UINTPTR + 0) && (WC_TRUST_STATIC_BUFFER_ALIGNMENT + 0)
    (void)lim;
    (void)rc;
    (void)pool.buf;
    PASS();
    return 0;
#else
    wc_limits_init(&lim);
    lim.static_buf = (void *)(pool.buf + 1);
    lim.static_size = sizeof pool.buf - 1;
    ASSERT(wc_open_ex(0, &lim, &rc) == NULL);
    ASSERT(rc == WC_EALIGN || rc == WC_EBADLIMITS);
    PASS();
    return 0;
#endif
}

static int results_equal_sorted(const wc *a, const wc *b)
{
    wc_word *ra = NULL;
    wc_word *rb = NULL;
    size_t na = 0;
    size_t nb = 0;
    size_t i;
    if (wc_results(a, &ra, &na) != WC_OK)
        return 0;
    if (wc_results(b, &rb, &nb) != WC_OK) {
        wc_results_free(ra);
        return 0;
    }
    if (na != nb) {
        wc_results_free(ra);
        wc_results_free(rb);
        return 0;
    }
    for (i = 0; i < na; i++) {
        if (ra[i].count != rb[i].count)
            break;
        if (strcmp(ra[i].word, rb[i].word) != 0)
            break;
    }
    wc_results_free(ra);
    wc_results_free(rb);
    return i == na;
}

static int stream_scan_all(wc_stream *s, const char *text, size_t len)
{
    size_t consumed = 0;
    int rc = wc_stream_scan_ex(s, text, len, &consumed);
    if (rc != WC_OK)
        return rc;
    return consumed == len ? WC_OK : WC_ERROR;
}

static int test_stream_matches_wc_scan_various_chunks(void)
{
    /* Includes separators, mixed case, and embedded NUL. */
    const unsigned char text[] = "Hello worl"
                                 "d!\0"
                                 "HELLO---world  abc123def  it's fine.\n"
                                 "EdgeCaseXYZ";
    const size_t L = sizeof text - 1;
    size_t chunk;

    TEST("stream API matches wc_scan across chunk boundaries");

    for (chunk = 1; chunk <= 97; chunk++) {
        wc *a = wc_open(0);
        wc *b = wc_open(0);
        int rc_stream = WC_OK;
        wc_stream *s = wc_stream_open(b, &rc_stream);
        size_t off = 0;

        ASSERT(a && b && s && rc_stream == WC_OK);
        ASSERT(wc_scan(a, (const char *)text, L) == WC_OK);

        while (off < L) {
            size_t n = L - off;
            if (n > chunk)
                n = chunk;
            ASSERT(stream_scan_all(s, (const char *)text + off, n) == WC_OK);
            off += n;
        }
        ASSERT(wc_stream_finish(s) == WC_OK);

        ASSERT(wc_total(a) == wc_total(b));
        ASSERT(wc_unique(a) == wc_unique(b));
        ASSERT(results_equal_sorted(a, b));

        wc_stream_close(s);
        wc_close(a);
        wc_close(b);
    }

    PASS();
    return 0;
}

static int test_stream_nomem_allows_continue(void)
{
    WC_TEST_STATIC_BUF(pool, 2048);
    wc_limits lim = WC_LIMITS_INIT();
    wc *w;
    wc_stream *s;
    int rc = WC_OK;
    size_t consumed = 0;

    TEST("stream: NOMEM reports partial consumption and stream stays valid");

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;
    lim.init_cap = 16; /* stay at minimum to force NOMEM on new unique */
    w = wc_open_ex(8, &lim, &rc);
    ASSERT(w != NULL && rc == WC_OK);

    /* Pre-fill to the ~0.7 load factor boundary: next unique triggers NOMEM. */
    for (int i = 0; i < 11; i++) {
        char word[3];
        word[0] = (char)('a' + i);
        word[1] = 'a';
        word[2] = '\0';
        ASSERT(wc_add(w, word) == WC_OK);
    }

    s = wc_stream_open(w, &rc);
    ASSERT(s != NULL && rc == WC_OK);

    /* New unique should hit NOMEM; consumed_out includes the separator. */
    rc = wc_stream_scan_ex(s, "zzz aa", 6, &consumed);
    ASSERT(consumed <= 6);
    if (rc == WC_NOMEM) {
        ASSERT(consumed >= 4);
        /* Stream remains valid; duplicate increments even after exhaustion. */
        consumed = 0;
        ASSERT(wc_stream_scan_ex(s, "aa", 2, &consumed) == WC_OK);
        ASSERT(consumed == 2);
    } else {
        ASSERT(rc == WC_OK);
        ASSERT(consumed == 6);
    }

    ASSERT(wc_stream_finish(s) == WC_OK);

    wc_stream_close(s);
    wc_close(w);
    PASS();
    return 0;
}

static int test_stream_allows_multiple_active_streams(void)
{
    wc *w;
    wc_stream *a;
    wc_stream *b;
    int rc = WC_OK;

    TEST("stream: multiple active streams keep independent buffers");

    w = wc_open(0);
    ASSERT(w != NULL);

    a = wc_stream_open(w, &rc);
    ASSERT(a != NULL && rc == WC_OK);

    rc = WC_OK;
    b = wc_stream_open(w, &rc);
    ASSERT(b != NULL && rc == WC_OK);

    ASSERT(stream_scan_all(a, "al", 2) == WC_OK);
    ASSERT(stream_scan_all(b, "be", 2) == WC_OK);
    ASSERT(stream_scan_all(a, "pha ", 4) == WC_OK);
    ASSERT(stream_scan_all(b, "ta ", 3) == WC_OK);
    ASSERT(wc_stream_finish(a) == WC_OK);
    ASSERT(wc_stream_finish(b) == WC_OK);
    ASSERT(wc_total(w) == 2);
    ASSERT(wc_unique(w) == 2);

    wc_stream_close(a);
    wc_stream_close(b);
    wc_close(w);

    PASS();
    return 0;
}

static int test_stream_finish_state_machine(void)
{
    wc *w = NULL;
    wc_stream *s = NULL;
    size_t consumed = 7;
    int ok = 1;

    TEST("stream finish state-machine enforcement");

    w = wc_open(0);
    if (!w) {
        FAIL("wc_open != NULL");
        ok = 0;
        goto done;
    }

    s = wc_stream_open(w, NULL);
    if (!s) {
        FAIL("wc_stream_open != NULL");
        ok = 0;
        goto done;
    }

    int frc = wc_stream_finish(s);
    if (frc != WC_OK) {
        FAIL("wc_stream_finish == WC_OK");
        ok = 0;
        goto done;
    }
    if (wc_stream_finish(s) != frc) {
        FAIL("wc_stream_finish idempotent");
        ok = 0;
        goto done;
    }

    consumed = 5;
    if (wc_stream_scan_ex(s, "alpha", 5, &consumed) != frc || consumed != 0) {
        FAIL("wc_stream_scan_ex after finish returns finish rc and zeroes "
             "consumed_out");
        ok = 0;
        goto done;
    }

done:
    if (ok)
        PASS();
    if (s)
        wc_stream_close(s);
    if (w)
        wc_close(w);
    return ok ? 0 : 1;
}

static int test_stream_parent_close_detaches_stream(void)
{
#if (WC_NO_HEAP + 0)
    TEST("stream: parent close detaches stream");
    PASS();
    return 0;
#else
    wc *w;
    wc_stream *s;
    size_t consumed = 123;
    int rc = WC_OK;

    TEST("stream: parent close detaches stream");

    w = wc_open(8);
    ASSERT(w != NULL);
    s = wc_stream_open(w, &rc);
    ASSERT(s != NULL);
    ASSERT(rc == WC_OK);

    wc_close(w);

    ASSERT(wc_stream_scan_ex(s, "alpha", 5, &consumed) == WC_ERROR);
    ASSERT(consumed == 0);
    ASSERT(wc_stream_finish(s) == WC_ERROR);
    wc_stream_close(s);

    PASS();
    return 0;
#endif
}

static int test_open_ex_tiny_static_fail(void)
{
    wc_limits lim;
    WC_TEST_STATIC_BUF(pool, 32);

    TEST("open_ex tiny static_buf fails");

    wc_limits_init(&lim);
    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    ASSERT(wc_open_ex(0, &lim, NULL) == NULL);

    PASS();
    return 0;
}

static int test_static_limits_enforced(void)
{
    wc_limits lim;
    WC_TEST_STATIC_BUF(pool, 4096);
    wc *w;
    size_t i;
    char word[32];
    int rc;
    wc_word *r = NULL;
    size_t n = 0;

    TEST("static_buf enforces capacity");

    wc_limits_init(&lim);
    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(0, &lim, NULL);
    ASSERT(w != NULL);

    rc = WC_OK;
    for (i = 0; i < 100000 && rc == WC_OK; i++) {
        (void)snprintf(word, sizeof word, "w%zu", i);
        rc = wc_add(w, word);
        ASSERT(rc == WC_OK || rc == WC_NOMEM);
    }

    ASSERT(wc_unique(w) > 0);
    ASSERT(invariant_cursor_sum_matches_total(w));

    {
        const char *t = "alpha beta gamma delta epsilon";
        int rc2 = wc_scan(w, t, strlen(t));
        ASSERT(rc2 == WC_OK || rc2 == WC_NOMEM);
    }

    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == wc_unique(w));
    wc_results_free(r);

    wc_close(w);
    PASS();
    return 0;
}

static int test_static_with_tiny_max_bytes_fails(void)
{
    wc_limits lim;
    WC_TEST_STATIC_BUF(pool, 4096);

    TEST("static_buf + tiny max_bytes fails");

    wc_limits_init(&lim);
    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;
    lim.max_bytes = 1;

    ASSERT(wc_open_ex(0, &lim, NULL) == NULL);

    PASS();
    return 0;
}

static int test_limits_budget_enforced(void)
{
    wc_limits lim;
    size_t i;
    char word[32];
    int rc;

    TEST("limits enforce max_bytes");

    wc_limits_init(&lim);
    lim.max_bytes = 4096;

    wc *const w = wc_open_ex(0, &lim, NULL);
    ASSERT(w != NULL);

    rc = WC_OK;
    for (i = 0; i < 100000 && rc == WC_OK; i++) {
        (void)snprintf(word, sizeof word, "w%zu", i);
        rc = wc_add(w, word);
        ASSERT(rc == WC_OK || rc == WC_NOMEM);
    }

    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_explicit_sizing_not_shrunk_by_budget(void)
{
    wc_limits lim;
    int rc = WC_OK;

    TEST("limits: explicit init_cap/block_size are not shrunk");

    wc_limits_init(&lim);
    lim.init_cap = 4096;
    lim.max_bytes = 4096;
    ASSERT(wc_open_ex(0, &lim, &rc) == NULL);
    ASSERT(rc == WC_EBADLIMITS);

    wc_limits_init(&lim);
    lim.block_size = 8192;
    lim.max_bytes = 4096;
    rc = WC_OK;
    ASSERT(wc_open_ex(8, &lim, &rc) == NULL);
    ASSERT(rc == WC_EBADLIMITS);

    PASS();
    return 0;
}

/* Monotonic boundary test for static_size */
static int test_static_minimum_size_boundary(void)
{
    wc_limits lim;
    WC_TEST_STATIC_BUF(pool, 4096);
    size_t sz;

    TEST("static_buf minimum size boundary");

    wc_limits_init(&lim);
    lim.static_buf = pool.buf;

    for (sz = 1; sz <= sizeof pool.buf; sz++) {
        wc *w;
        lim.static_size = sz;
        w = wc_open_ex(0, &lim, NULL);
        if (w) {
            wc_close(w);
            break;
        }
    }

    ASSERT(sz <= sizeof pool.buf);

    if (sz > 1) {
        lim.static_size = sz - 1;
        ASSERT(wc_open_ex(0, &lim, NULL) == NULL);
    }

    PASS();
    return 0;
}

static int test_duplicates_survive_exhaustion_static(void)
{
    wc_limits lim;
    WC_TEST_STATIC_BUF(pool, 4096);
    wc *w;
    int rc;
    size_t i;
    size_t total_before;

    TEST("static_buf: duplicates succeed after exhaustion");

    wc_limits_init(&lim);
    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;

    w = wc_open_ex(0, &lim, NULL);
    ASSERT(w != NULL);

    /* Ensure at least one known key exists early. */
    ASSERT(wc_add(w, "alpha") == WC_OK);

    /* Drive toward exhaustion with many short uniques. */
    rc = WC_OK;
    for (i = 0; i < 100000 && rc == WC_OK; i++) {
        char tmp[32];
        (void)snprintf(tmp, sizeof tmp, "k%zu", i);
        rc = wc_add(w, tmp);
        ASSERT(rc == WC_OK || rc == WC_NOMEM);
    }

    /* We should eventually hit a limit in a small static pool. */
    ASSERT(rc == WC_NOMEM);

    total_before = wc_total(w);

    /* Must still be able to increment an existing key without allocating. */
    ASSERT(wc_add(w, "alpha") == WC_OK);
    ASSERT(wc_add(w, "alpha") == WC_OK);
    ASSERT(wc_total(w) == total_before + 2);

    /* wc_scan of duplicates-only must also succeed. */
    {
        const char *t = "alpha alpha alpha";
        int rc2 = wc_scan(w, t, strlen(t));
        ASSERT(rc2 == WC_OK);
    }

    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_duplicates_survive_exhaustion_max_bytes(void)
{
    wc_limits lim;
    wc *w;
    int rc;
    size_t i;
    size_t total_before;

    TEST("max_bytes: duplicates succeed after exhaustion");

    wc_limits_init(&lim);
    lim.max_bytes = 4096;

    w = wc_open_ex(0, &lim, NULL);
    ASSERT(w != NULL);

    ASSERT(wc_add(w, "alpha") == WC_OK);

    rc = WC_OK;
    for (i = 0; i < 100000 && rc == WC_OK; i++) {
        char tmp[32];
        (void)snprintf(tmp, sizeof tmp, "k%zu", i);
        rc = wc_add(w, tmp);
        ASSERT(rc == WC_OK || rc == WC_NOMEM);
    }

    ASSERT(rc == WC_NOMEM);

    total_before = wc_total(w);

    ASSERT(wc_add(w, "alpha") == WC_OK);
    ASSERT(wc_add(w, "alpha") == WC_OK);
    ASSERT(wc_total(w) == total_before + 2);

    {
        const char *t = "alpha alpha alpha";
        int rc2 = wc_scan(w, t, strlen(t));
        ASSERT(rc2 == WC_OK);
    }

    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_strict_max_bytes_blocks_peaks(void)
{
    wc_limits base = WC_LIMITS_INIT();
    wc_limits lim;
    wc_stats st;
    wc *probe;
    wc *loose;
    wc *strict;
    size_t table_bytes;
    size_t other_bytes;
    size_t new_table_bytes;
    size_t limit;
    int rc;

    TEST("strict_max_bytes rejects transient peaks");

    ASSERT(infer_slot_size(&table_bytes));
    base.init_cap = 4;

    probe = wc_open_ex(0, &base, &rc);
    ASSERT(probe != NULL && rc == WC_OK);
    ASSERT(wc_get_stats(probe, &st) == WC_OK);
    table_bytes *= st.cap;
    ASSERT(st.bytes_used >= table_bytes);
    other_bytes = st.bytes_used - table_bytes;
    new_table_bytes = table_bytes * 2u;
    limit = other_bytes + new_table_bytes;
    wc_close(probe);

    lim = base;
    lim.max_bytes = limit;
    lim.strict_max_bytes = 0;

    loose = wc_open_ex(0, &lim, &rc);
    ASSERT(loose != NULL && rc == WC_OK);
    ASSERT(wc_add(loose, "a") == WC_OK);
    ASSERT(wc_add(loose, "b") == WC_OK);
    ASSERT(wc_add(loose, "c") == WC_OK);
    ASSERT(wc_add(loose, "d") == WC_OK);
    wc_close(loose);

    lim.strict_max_bytes = 1;
    strict = wc_open_ex(0, &lim, &rc);
    ASSERT(strict != NULL && rc == WC_OK);
    ASSERT(wc_get_stats(strict, &st) == WC_OK);
    {
        size_t cur_table = table_bytes;
        size_t grow_need = st.bytes_used + cur_table * 2u;
        const int expect_fail =
                st.bytes_limit && grow_need > st.bytes_limit ? 1 : 0;

        ASSERT(wc_add(strict, "a") == WC_OK);
        ASSERT(wc_add(strict, "b") == WC_OK);
        rc = wc_add(strict, "c");
        if (rc == WC_OK)
            rc = wc_add(strict, "d");
        if (expect_fail)
            ASSERT(rc == WC_NOMEM || rc == WC_ERROR);
        else
            ASSERT(rc == WC_OK);
    }
    wc_close(strict);

    PASS();
    return 0;
}

/* --- wc_add tests ------------------------------------------------------ */

static int test_add_single(void)
{
    wc *w;
    TEST("add single");
    w = wc_open(0);
    ASSERT(wc_add(w, "hello") == WC_OK);
    ASSERT(wc_total(w) == 1);
    ASSERT(wc_unique(w) == 1);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_dup(void)
{
    wc *w;
    TEST("add duplicate");
    w = wc_open(0);
    ASSERT(wc_add(w, "hello") == WC_OK);
    ASSERT(wc_add(w, "hello") == WC_OK);
    ASSERT(wc_add(w, "hello") == WC_OK);
    ASSERT(wc_total(w) == 3);
    ASSERT(wc_unique(w) == 1);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_empty(void)
{
    wc *w;
    TEST("add empty string");
    w = wc_open(0);
    ASSERT(wc_add(w, "") == WC_OK);
    ASSERT(wc_total(w) == 0);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_null(void)
{
    wc *w;
    TEST("add NULL args");
    ASSERT(wc_add(NULL, "x") == WC_ERROR);
    w = wc_open(0);
    ASSERT(wc_add(w, NULL) == WC_ERROR);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_trunc(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    TEST("add truncation");
    w = wc_open(4);
    ASSERT(wc_add(w, "abcdefghij") == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 1);
    ASSERT(strcmp(r[0].word, "abcd") == 0);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_n_variants(void)
{
    wc *w;
    wc_word *r = NULL;
    size_t n = 0;

    TEST("add_n / add_norm_n");
    w = wc_open(4);
    ASSERT(w != NULL);

    ASSERT(wc_add_norm_n(w, "HELLO", 5) == WC_OK); /* truncates to hell */
    ASSERT(wc_add_norm_n(w, "hellX", 5) == WC_OK); /* same bucket */
    ASSERT(wc_add_n(w, "HELLO", 5) == WC_OK);      /* distinct (case-sens) */

    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 2);
    ASSERT(strcmp(r[0].word, "hell") == 0);
    ASSERT(r[0].count == 2);
    ASSERT(strcmp(r[1].word, "HELL") == 0);
    ASSERT(r[1].count == 1);

    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

static int test_add_n_embedded_nul_truncates(void)
{
    wc *w;
    wc_word *r = NULL;
    size_t n = 0;
    const char raw[] = { 'a', 'b', '\0', 'c', 'd' };

    TEST("add_n: embedded NUL truncates to prefix");

    w = wc_open(0);
    ASSERT(w != NULL);

    ASSERT(wc_add_n(w, raw, sizeof raw) == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 1);
    ASSERT(strcmp(r[0].word, "ab") == 0);
    ASSERT(r[0].count == 1);

    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

/* Deterministic hash collision regression (different lengths).
   Old buggy code could OOB-read under ASan on the second insert. */
static int test_add_hash_collision_different_length(void)
{
    wc *w;
    const char *a = "MXl";    /* 32-bit FNV-1a collides with b */
    const char *b = "QFdzF2"; /* longer */

    TEST("add: hash collision (different lengths) regression");

    w = wc_open(0);
    ASSERT(w != NULL);

    ASSERT(wc_add(w, a) == WC_OK);
    ASSERT(wc_add(w, b) == WC_OK);

    ASSERT(wc_unique(w) == 2);
    ASSERT(wc_total(w) == 2);
    ASSERT(invariant_cursor_sum_matches_total(w));

    wc_close(w);
    PASS();
    return 0;
}

/* --- wc_scan tests ----------------------------------------------------- */

static int test_scan_simple(void)
{
    wc *w;
    const char *t = "Hello World";
    TEST("scan simple");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_total(w) == 2);
    ASSERT(wc_unique(w) == 2);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_empty(void)
{
    wc *w;
    TEST("scan empty");
    w = wc_open(0);
    ASSERT(wc_scan(w, "", 0) == WC_OK);
    ASSERT(wc_scan(w, NULL, 0) == WC_OK);
    ASSERT(wc_total(w) == 0);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_null(void)
{
    wc *w;
    TEST("scan NULL args");
    ASSERT(wc_scan(NULL, "x", 1) == WC_ERROR);
    w = wc_open(0);
    ASSERT(wc_scan(w, NULL, 100) == WC_ERROR);
    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_binary(void)
{
    wc *w;
    const char t[] = "hello\0world\0test";
    TEST("scan with embedded NUL");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, sizeof(t) - 1) == WC_OK);
    ASSERT(wc_total(w) == 3);
    ASSERT(wc_unique(w) == 3);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

/* Hash collision regression via wc_scan using lowercase-only colliders.
   This specifically exercises the wc_scan path. */
static int test_scan_hash_collision_different_length(void)
{
    wc *w;
    const char *a = "svhpy";                 /* collides with b */
    const char *b = "znycrycwqhztadbhsrdok"; /* longer */
    char text[128];

    TEST("scan: hash collision (different lengths) regression");

    (void)snprintf(text, sizeof text, "%s %s", a, b);

    w = wc_open(0);
    ASSERT(w != NULL);

    ASSERT(wc_scan(w, text, strlen(text)) == WC_OK);
    ASSERT(wc_unique(w) == 2);
    ASSERT(wc_total(w) == 2);
    ASSERT(invariant_cursor_sum_matches_total(w));

    wc_close(w);
    PASS();
    return 0;
}

/* --- wc_results tests -------------------------------------------------- */

static int test_results_sorted(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    const char *t = "apple banana apple cherry apple banana";
    TEST("results sorted");
    w = wc_open(0);
    ASSERT(wc_scan(w, t, strlen(t)) == WC_OK);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(n == 3);
    ASSERT(strcmp(r[0].word, "apple") == 0 && r[0].count == 3);
    ASSERT(strcmp(r[1].word, "banana") == 0 && r[1].count == 2);
    ASSERT(strcmp(r[2].word, "cherry") == 0 && r[2].count == 1);

    wc_results_free(r);
    ASSERT(invariant_cursor_sum_matches_total(w));
    wc_close(w);
    PASS();
    return 0;
}

static int test_results_empty(void)
{
    wc *w;
    wc_word *r = (wc_word *)0x1;
    size_t n = 999;

    TEST("results empty");
    w = wc_open(0);
    ASSERT(wc_results(w, &r, &n) == WC_OK);
    ASSERT(r == NULL && n == 0);
    wc_close(w);
    PASS();
    return 0;
}

static int test_results_null(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    TEST("results NULL args");
    w = wc_open(0);
    ASSERT(wc_results(NULL, &r, &n) == WC_ERROR);
    ASSERT(wc_results(w, NULL, &n) == WC_ERROR);
    ASSERT(wc_results(w, &r, NULL) == WC_ERROR);
    wc_close(w);
    PASS();
    return 0;
}

static int test_topn_ordering(void)
{
    wc *w;
    wc_word *r = NULL;
    size_t n = 0;

    TEST("topn ordering + ties");
    w = wc_open(0);
    ASSERT(w != NULL);

    ASSERT(wc_add(w, "beta") == WC_OK);
    ASSERT(wc_add(w, "beta") == WC_OK);
    ASSERT(wc_add(w, "alpha") == WC_OK);
    ASSERT(wc_add(w, "alpha") == WC_OK);
    ASSERT(wc_add(w, "gamma") == WC_OK);

    ASSERT(wc_topn(w, 2, &r, &n) == WC_OK);
    ASSERT(n == 2);
    ASSERT(strcmp(r[0].word, "alpha") == 0);
    ASSERT(strcmp(r[1].word, "beta") == 0);
    ASSERT(r[0].count == 2 && r[1].count == 2);

    wc_results_free(r);
    wc_close(w);
    PASS();
    return 0;
}

/* --- Query / metadata tests ------------------------------------------- */

static int test_query_null(void)
{
    TEST("query NULL");
    ASSERT(wc_total(NULL) == 0);
    ASSERT(wc_unique(NULL) == 0);
    PASS();
    return 0;
}

static int test_validate_gate(void)
{
    wc *w = NULL;
    void *raw = NULL;
    int ok = 1;

    TEST("validate guards invalid handles");

    raw = calloc(1, 4096);
    if (!raw) {
        FAIL("raw != NULL");
        ok = 0;
        goto done;
    }
    if (wc_validate(NULL) != WC_ERROR) {
        FAIL("wc_validate(NULL) == WC_ERROR");
        ok = 0;
        goto done;
    }
    if (wc_validate((wc *)raw) != WC_ERROR) {
        FAIL("wc_validate((wc *)raw) == WC_ERROR");
        ok = 0;
        goto done;
    }

    w = wc_open(0);
    if (!w) {
        FAIL("wc_open != NULL");
        ok = 0;
        goto done;
    }
    if (wc_validate(w) != WC_OK) {
        FAIL("wc_validate(w) == WC_OK");
        ok = 0;
        goto done;
    }

done:
    if (ok)
        PASS();
    if (w)
        wc_close(w);
    free(raw);
    return ok ? 0 : 1;
}

static int test_version(void)
{
    const char *v;
    TEST("version");
    v = wc_version();
    ASSERT(v && strlen(v) > 0);
    ASSERT(strcmp(v, WC_VERSION) == 0);
    PASS();
    return 0;
}

static int test_errstr(void)
{
    const char *s;
    TEST("errstr");
    s = wc_errstr(WC_OK);
    ASSERT(s && strlen(s) > 0);
    s = wc_errstr(WC_ERROR);
    ASSERT(s && strlen(s) > 0);
    s = wc_errstr(WC_NOMEM);
    ASSERT(s && strlen(s) > 0);
    s = wc_errstr(WC_EALIGN);
    ASSERT(s && strlen(s) > 0);
    s = wc_errstr(WC_EBADLIMITS);
    ASSERT(s && strlen(s) > 0);
    s = wc_errstr(9999);
    ASSERT(s && strlen(s) > 0);
    PASS();
    return 0;
}

static int test_build_info(void)
{
    const wc_build_config *cfg;

    TEST("build_info");
    cfg = wc_build_info();
    ASSERT(cfg != NULL);
    ASSERT(cfg->struct_size >= sizeof *cfg);
    ASSERT(cfg->version_number == WC_VERSION_NUMBER);
    ASSERT(cfg->max_word == WC_MAX_WORD);
    ASSERT(cfg->min_init_cap == WC_MIN_INIT_CAP);
    ASSERT(cfg->min_block_sz == WC_MIN_BLOCK_SZ);
    ASSERT((cfg->stack_buffer != 0) == (WC_STACK_BUFFER != 0));
    ASSERT((cfg->hosted != 0) == (WC_STDC_HOSTED != 0));
    ASSERT((cfg->use_libc_string != 0) == (WC_USE_LIBC_STRING != 0));
    ASSERT((cfg->use_libc_qsort != 0) == (WC_USE_LIBC_QSORT != 0));
    ASSERT((cfg->have_errno != 0) == (WC_HAVE_ERRNO != 0));
    ASSERT((cfg->no_heap != 0) == (WC_NO_HEAP != 0));
    ASSERT((cfg->hash_strong != 0) == (WC_HASH_STRONG != 0));
    ASSERT((cfg->have_uintptr != 0) == (WC_HAVE_UINTPTR != 0));
    ASSERT((cfg->trust_static_buffer_alignment != 0) ==
           (WC_TRUST_STATIC_BUFFER_ALIGNMENT != 0));
    PASS();
    return 0;
}

static int test_stats_and_reserve(void)
{
    wc *w;
    wc_stats st;
    size_t cap0;

    TEST("stats + reserve");
    w = wc_open(0);
    ASSERT(w != NULL);
    ASSERT(wc_get_stats(w, &st) == WC_OK);
    cap0 = st.cap;
    ASSERT(cap0 > 0);
    ASSERT(wc_reserve(w, cap0 * 2, 0) == WC_OK);
    ASSERT(wc_get_stats(w, &st) == WC_OK);
    ASSERT(st.cap >= cap0 * 2 || st.cap > cap0);
    wc_close(w);

    {
        WC_TEST_STATIC_BUF(buf, 2048);
        wc_limits lim;
        int rc = WC_OK;
        wc_limits_init(&lim);
        lim.static_buf = buf.buf;
        lim.static_size = sizeof buf.buf;
        lim.init_cap = 8;
        lim.block_size = 256;
        lim.max_bytes = sizeof buf.buf;
        w = wc_open_ex(16, &lim, &rc);
        ASSERT(w != NULL);
        ASSERT(rc == WC_OK);
        ASSERT(wc_get_stats(w, &st) == WC_OK);
        ASSERT(st.static_mode == 1);
        ASSERT(wc_reserve(w, st.cap * 4 + 8, 0) == WC_NOMEM);
        wc_close(w);
    }
    PASS();
    return 0;
}

static int test_arena_grows_blocks_dynamic(void)
{
    wc_limits lim;
    wc *w;
    wc_stats st;
    char word[256];

    TEST("arena: grows blocks in dynamic mode");

    wc_limits_init(&lim);
    lim.init_cap = 64;
    lim.block_size = WC_MIN_BLOCK_SZ; /* minimal block size exercises growth */

    w = wc_open_ex(128, &lim, NULL);
    ASSERT(w != NULL);

    memset(word, 'a', sizeof word);
    word[sizeof word - 1] = '\0';

    /* Make several distinct long-ish words to force multiple blocks. */
    for (int i = 0; i < 8; i++) {
        word[0] = (char)('a' + i);
        ASSERT(wc_add(w, word) == WC_OK);
    }

    ASSERT(wc_get_stats(w, &st) == WC_OK);
    ASSERT(st.arena_blocks > 1);

    wc_close(w);
    PASS();
    return 0;
}

static int test_reserve_dynamic_counts_tail_capacity(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    wc_stats st;
    wc *probe;
    wc *w;
    size_t tight_limit;
    int rc = WC_OK;

    TEST("reserve: dynamic max_bytes counts current arena capacity");

    lim.init_cap = 16;
    lim.block_size = 256;

    probe = wc_open_ex(4, &lim, &rc);
    ASSERT(probe != NULL);
    ASSERT(rc == WC_OK);
    ASSERT(wc_get_stats(probe, &st) == WC_OK);
    ASSERT(st.bytes_used > 0);
    tight_limit = st.bytes_used;
    wc_close(probe);

    lim.max_bytes = tight_limit;

    w = wc_open_ex(4, &lim, &rc);
    ASSERT(w != NULL);
    ASSERT(rc == WC_OK);
    ASSERT(wc_get_stats(w, &st) == WC_OK);
    ASSERT(st.static_mode == 0);
    ASSERT(st.bytes_limit == tight_limit);
    ASSERT(st.bytes_used == tight_limit);

    ASSERT(wc_reserve(w, 0, 1) == WC_OK);
    ASSERT(wc_add(w, "abc") == WC_OK);
    ASSERT(wc_total(w) == 1);
    ASSERT(wc_unique(w) == 1);

    wc_close(w);
    PASS();
    return 0;
}

static int test_reserve_dynamic_rejects_impossible_bytes(void)
{
    wc *w;

    TEST("reserve: dynamic rejects impossible arena bytes");

    w = wc_open(0);
    ASSERT(w != NULL);
    ASSERT(wc_reserve(w, 0, WC_SIZE_MAX) == WC_NOMEM);

    wc_close(w);
    PASS();
    return 0;
}

static int test_reserve_static_respects_max_bytes(void)
{
    WC_TEST_STATIC_BUF(pool, 4096);
    wc_limits lim = WC_LIMITS_INIT();
    wc_stats st;
    wc *w;
    int rc = WC_OK;

    TEST("reserve: static_buf also respects max_bytes guard");

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;
    lim.max_bytes = 2048; /* smaller than static_size */

    w = wc_open_ex(0, &lim, &rc);
    ASSERT(w != NULL);
    ASSERT(rc == WC_OK);
    ASSERT(wc_get_stats(w, &st) == WC_OK);
    ASSERT(st.static_mode == 1);
    ASSERT(st.bytes_limit == 2048);
    ASSERT(st.bytes_used <= st.bytes_limit);

    ASSERT(wc_reserve(w, 0, sizeof pool.buf) == WC_NOMEM);

    wc_close(w);

    wc_limits_init(&lim);
    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;
    lim.max_bytes = 2048; /* smaller than static_size */
    lim.block_size = 4096;

    w = wc_open_ex(0, &lim, &rc);
    ASSERT(w == NULL);
    ASSERT(rc == WC_EBADLIMITS);

    PASS();
    return 0;
}

static int test_reserve_static_checks_arena_capacity(void)
{
    WC_TEST_STATIC_BUF(pool, 16384);
    wc_limits lim = WC_LIMITS_INIT();
    wc *w;
    int rc = WC_OK;

    TEST("reserve: static_buf checks remaining arena capacity");

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;
    lim.init_cap = 128;
    lim.block_size = WC_MIN_BLOCK_SZ;

    w = wc_open_ex(32, &lim, &rc);
    ASSERT(w != NULL);
    ASSERT(rc == WC_OK);

    ASSERT(wc_reserve(w, 0, 1) == WC_OK);
    ASSERT(wc_reserve(w, 0, (size_t)WC_MIN_BLOCK_SZ * 2u) == WC_NOMEM);

    wc_close(w);
    PASS();
    return 0;
}

static int test_results_and_stream_not_counted_in_stats(void)
{
    wc *w;
    wc_stats st0, st1;
    wc_word *r = NULL;
    size_t n = 0;
    wc_stream *s = NULL;
    int rc = WC_OK;

    TEST("stats: results + stream allocations not counted");

    w = wc_open(0);
    ASSERT(w != NULL);

    ASSERT(wc_add(w, "alpha") == WC_OK);
    ASSERT(wc_add(w, "beta") == WC_OK);

    ASSERT(wc_get_stats(w, &st0) == WC_OK);

    ASSERT(wc_results(w, &r, &n) == WC_OK);
    wc_results_free(r);

    ASSERT(wc_get_stats(w, &st1) == WC_OK);
    ASSERT(st1.bytes_used == st0.bytes_used);

    s = wc_stream_open(w, &rc);
    ASSERT(s != NULL && rc == WC_OK);
    wc_stream_close(s);

    ASSERT(wc_get_stats(w, &st1) == WC_OK);
    ASSERT(st1.bytes_used == st0.bytes_used);

    wc_close(w);
    PASS();
    return 0;
}

static int test_scan_len_gt_ptrdiff_max_rejected(void)
{
    wc *w;
    size_t big = (size_t)WC_PTRDIFF_MAX;

    TEST("scan: len > WC_PTRDIFF_MAX rejected");

    w = wc_open(0);
    ASSERT(w != NULL);

    if (big < SIZE_MAX) {
        big++;
        ASSERT(wc_scan(w, "a", big) == WC_ERROR);
    }

    wc_close(w);
    PASS();
    return 0;
}

static int test_stream_len_gt_ptrdiff_max_rejected(void)
{
    wc *w;
    wc_stream *s;
    int rc = WC_OK;
    size_t big = (size_t)WC_PTRDIFF_MAX;

    TEST("stream: len > WC_PTRDIFF_MAX rejected");

    w = wc_open(0);
    ASSERT(w != NULL);

    s = wc_stream_open(w, &rc);
    ASSERT(s != NULL && rc == WC_OK);

    if (big < SIZE_MAX) {
        size_t consumed = 123;
        big++;
        ASSERT(wc_stream_scan_ex(s, "a", big, &consumed) == WC_ERROR);
        ASSERT(consumed == 0);
    }

    wc_stream_close(s);
    wc_close(w);
    PASS();
    return 0;
}

static int test_grow_tight_budget_allows_postgrow_fit(void)
{
    wc_limits lim;
    wc_stats st;
    size_t slot_sz;
    size_t old_alloc, post_other, new_alloc, tight_budget;
    size_t block_overhead;
    size_t table_bytes;
    size_t cap, q, r, threshold;
    size_t arena_need;
    wc *w;
    int i;

    TEST("grow: post-grow fits but peak doesn't (manual alloc path)");

    ASSERT(infer_slot_size(&slot_sz));

    /* First open (no budget) to measure post_other precisely. */
    wc_limits_init(&lim);
    lim.init_cap = 16;
    lim.block_size = 4096;

    w = wc_open_ex(8, &lim, NULL);
    ASSERT(w != NULL);
    ASSERT(wc_get_stats(w, &st) == WC_OK);

    cap = st.cap;
    ASSERT(cap == 16);

    /* threshold == ceil(cap*0.7) */
    q = cap / 10u;
    r = cap % 10u;
    threshold = q * 7u + (r * 7u + 9u) / 10u;

    old_alloc = cap * slot_sz;
    table_bytes = old_alloc;
    ASSERT(st.bytes_used >= old_alloc);
    ASSERT(st.bytes_used >= old_alloc + lim.block_size);

    /* Derive allocator overhead so we can recompute a tight budget. */
    block_overhead = st.bytes_used - old_alloc;
    ASSERT(block_overhead >= lim.block_size);
    block_overhead -= lim.block_size;

    new_alloc = (cap * 2u) * slot_sz;
    ASSERT(threshold < SIZE_MAX);
    ASSERT(threshold + 1u <= SIZE_MAX / WC_TEST_ALIGN);
    arena_need = (threshold + 1u) * WC_TEST_ALIGN;
    ASSERT(arena_need <= lim.block_size);

    tight_budget = block_overhead + lim.block_size + new_alloc;

    wc_close(w);

    /* Reopen with tight budget that cannot hold old+new simultaneously. */
    wc_limits_init(&lim);
    lim.init_cap = 16;
    lim.block_size = 4096;
    lim.max_bytes = tight_budget;

    w = wc_open_ex(8, &lim, NULL);
    ASSERT(w != NULL);
    ASSERT(wc_get_stats(w, &st) == WC_OK);
    ASSERT(st.cap == cap);
    post_other = st.bytes_used - table_bytes;
    ASSERT(st.bytes_limit == tight_budget);

    /* Insert exactly threshold uniques; next unique forces grow. */
    for (i = 0; i < (int)threshold; i++) {
        char word[3];
        word[0] = (char)('a' + (i % 26));
        word[1] = (char)('a' + ((i / 26) % 26));
        word[2] = '\0';
        ASSERT(wc_add(w, word) == WC_OK);
    }

    {
        wc_stats before;
        ASSERT(wc_get_stats(w, &before) == WC_OK);
        ASSERT(before.cap == cap);

        /* Next unique => grow */
        ASSERT(wc_add(w, "zz") == WC_OK);

        ASSERT(wc_get_stats(w, &st) == WC_OK);
        ASSERT(st.cap >= cap * 2u);

        /* Must respect tight budget */
        ASSERT(st.bytes_used == post_other + new_alloc);
        ASSERT(st.bytes_used <= tight_budget);
    }

    wc_close(w);
    PASS();
    return 0;
}

/* --- Stress / cursor tests -------------------------------------------- */

static int test_cursor_iteration(void)
{
    wc *w;
    wc_cursor c;
    size_t seen = 0;
    size_t sum = 0;

    TEST("cursor iterates all entries and sums to total");

    w = wc_open(0);
    ASSERT(w != NULL);

    ASSERT(wc_add(w, "alpha") == WC_OK);
    ASSERT(wc_add(w, "beta") == WC_OK);
    ASSERT(wc_add(w, "beta") == WC_OK);
    ASSERT(wc_add(w, "gamma") == WC_OK);
    ASSERT(wc_add(w, "gamma") == WC_OK);
    ASSERT(wc_add(w, "gamma") == WC_OK);

    wc_cursor_init(&c, w);
    for (;;) {
        const char *word = NULL;
        size_t cnt = 0;
        if (!wc_cursor_next(&c, &word, &cnt))
            break;
        ASSERT(word != NULL);
        ASSERT(cnt > 0);
        seen++;
        ASSERT(sum <= WC_SIZE_MAX - cnt);
        sum += cnt;
    }

    ASSERT(seen == wc_unique(w));
    ASSERT(sum == wc_total(w));
    ASSERT(invariant_cursor_sum_matches_total(w));

    wc_close(w);
    PASS();
    return 0;
}

/* --- OOM injection tests (glibc-specific) ------------------------------ */

#if defined(WC_TEST_OOM) && defined(__GLIBC__)

static int test_oom_open(void)
{
    wc *w;
    int i;
    TEST("OOM in wc_open");
    for (i = 1; i <= 10; i++) {
        oom_arm(i);
        w = wc_open(0);
        if (w)
            wc_close(w);
        oom_reset();
    }
    w = wc_open(0);
    ASSERT(w != NULL);
    wc_close(w);
    PASS();
    return 0;
}

static int test_oom_add(void)
{
    wc *w;
    int i, rc;
    TEST("OOM in wc_add");
    for (i = 1; i <= 20; i++) {
        w = wc_open(0);
        ASSERT(w != NULL);
        oom_arm(i);
        rc = wc_add(w, "testword");
        oom_reset();
        ASSERT(rc == WC_OK || rc == WC_NOMEM);
        ASSERT(invariant_cursor_sum_matches_total(w));
        wc_close(w);
    }
    PASS();
    return 0;
}

static int test_oom_scan(void)
{
    wc *w;
    const char *t = "the quick brown fox jumps over the lazy dog";
    int i, rc;
    TEST("OOM in wc_scan");
    for (i = 1; i <= 30; i++) {
        w = wc_open(0);
        ASSERT(w != NULL);
        oom_arm(i);
        rc = wc_scan(w, t, strlen(t));
        oom_reset();
        ASSERT(rc == WC_OK || rc == WC_NOMEM);
        ASSERT(invariant_cursor_sum_matches_total(w));
        wc_close(w);
    }
    PASS();
    return 0;
}

static int test_oom_results(void)
{
    wc *w;
    wc_word *r;
    size_t n;
    int i, rc;
    TEST("OOM in wc_results");
    for (i = 1; i <= 10; i++) {
        w = wc_open(0);
        ASSERT(wc_add(w, "hello") == WC_OK);
        ASSERT(wc_add(w, "world") == WC_OK);
        oom_arm(i);
        rc = wc_results(w, &r, &n);
        oom_reset();
        if (rc == WC_OK)
            wc_results_free(r);
        else
            ASSERT(rc == WC_NOMEM);
        ASSERT(invariant_cursor_sum_matches_total(w));
        wc_close(w);
    }
    PASS();
    return 0;
}

#endif /* WC_TEST_OOM && __GLIBC__ */

/* --- Fuzz harness (libFuzzer) ----------------------------------------- */

#if defined(WC_TEST_FUZZ) || defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    wc *w;
    wc_limits lim;
    size_t i = 0;

    if (!data || size == 0)
        return 0;

    wc_limits_init(&lim);

    /* Options derived from the input prefix */
    {
        const uint8_t flags = data[i++];
        const size_t maxw = (flags & 1) ? 4u : 0u; /* 0 => default 64 */
        const int use_budget = (flags & 2) != 0;
        const int use_seed = (flags & 4) != 0;

        if (use_budget)
            lim.max_bytes = 4096u;
        if (use_seed) {
            if (size - i >= 4) {
                lim.hash_seed = (unsigned long)rd_u32(&data[i]);
                i += 4;
            } else {
                lim.hash_seed = 0x12345678UL;
            }
        }

        w = wc_open_ex(maxw, &lim, NULL);
        if (!w)
            return 0;
    }

    while (i < size) {
        const uint8_t op = data[i++];
        switch (op & 3u) {
            case 0: { /* wc_add */
                size_t n = 0;
                char word[65];
                if (i >= size)
                    break;
                n = (size_t)(data[i++] % 64u);
                if (size - i < n)
                    n = size - i;
                memcpy(word, &data[i], n);
                word[n] = '\0';
                {
                    int rc = wc_add(w, word);
                    if (rc != WC_OK && rc != WC_NOMEM)
                        abort();
                }
                i += n;
                break;
            }
            case 1: { /* wc_scan */
                size_t n = 0;
                if (i >= size)
                    break;
                n = (size_t)data[i++];
                if (size - i < n)
                    n = size - i;
                {
                    int rc = wc_scan(w, (const char *)&data[i], n);
                    if (rc != WC_OK && rc != WC_NOMEM)
                        abort();
                }
                i += n;
                break;
            }
            case 2: { /* wc_results */
                wc_word *r = NULL;
                size_t n = 0;
                int rc = wc_results(w, &r, &n);
                if (rc == WC_OK && r) {
                    /* basic sortedness check on adjacent entries */
                    size_t k;
                    for (k = 1; k < n; k++) {
                        if (r[k - 1].count < r[k].count) {
                            /* If this ever triggers, it's a correctness bug */
                            abort();
                        }
                    }
                    wc_results_free(r);
                }
                break;
            }
            case 3: { /* cursor invariant */
                if (!invariant_cursor_sum_matches_total(w))
                    abort();
                break;
            }
        }
    }

    if (!invariant_cursor_sum_matches_total(w))
        abort();

    wc_close(w);
    return 0;
}

#if defined(WC_TEST_FUZZ_STANDALONE)
int main(void)
{
    unsigned char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf, stdin);
    (void)LLVMFuzzerTestOneInput(buf, n);
    return 0;
}
#endif /* WC_TEST_FUZZ_STANDALONE */

#else /* normal unit test main */

/* --- Main (unit tests) ------------------------------------------------ */

int main(void)
{
    printf("\n=== Wordcount Tests (v%s) ===\n\n", wc_version());

    printf("Lifecycle / Limits:\n");
    test_open_close();
    test_close_null();
    test_max_word_clamp();
    test_open_ex_null_limits();
    test_limits_struct_size_invalid_rejected();
    test_limits_struct_size_smaller_accepted();
    test_limits_struct_size_larger_accepted();
    test_open_ex_tiny_budget_fail();
    test_open_ex_tiny_budget_fail_reason();
    test_limits_budget_enforced();
    test_explicit_sizing_not_shrunk_by_budget();
    test_limits_invalid_combinations();
    test_open_ex_tiny_static_fail();
    test_static_buf_misaligned_rejected();
    test_open_ex_misaligned_rejected_reason();
    test_full_table_dynamic_grows_instead_of_error();
    test_full_table_static_returns_nomem_not_error();
    test_static_minimum_size_boundary();
    test_static_limits_enforced();
    test_static_with_tiny_max_bytes_fails();
    test_max_word_clamped_to_wc_max_word();
    test_static_accepts_one_max_word();
    test_static_default_arena_uses_remaining_budget();
    test_static_explicit_block_size_is_honored();
    test_grow_preserves_existing_keys();
    test_duplicates_survive_exhaustion_static();
    test_duplicates_survive_exhaustion_max_bytes();
    test_strict_max_bytes_blocks_peaks();
    test_arena_grows_blocks_dynamic();
    test_reserve_dynamic_counts_tail_capacity();
    test_reserve_dynamic_rejects_impossible_bytes();
    test_reserve_static_respects_max_bytes();
    test_reserve_static_checks_arena_capacity();
    test_results_and_stream_not_counted_in_stats();
    test_grow_tight_budget_allows_postgrow_fit();
    test_scan_len_gt_ptrdiff_max_rejected();
    test_stream_len_gt_ptrdiff_max_rejected();

    printf("\nwc_add:\n");
    test_add_single();
    test_add_dup();
    test_add_empty();
    test_add_null();
    test_add_trunc();
    test_add_n_variants();
    test_add_n_embedded_nul_truncates();
    test_add_hash_collision_different_length();

    printf("\nwc_scan:\n");
    test_scan_simple();
    test_scan_empty();
    test_scan_null();
    test_scan_binary();
    test_scan_hash_collision_different_length();

    printf("\nwc_results:\n");
    test_results_sorted();
    test_results_empty();
    test_results_null();
    test_topn_ordering();

    printf("\nQueries:\n");
    test_query_null();
    test_validate_gate();
    test_version();
    test_errstr();
    test_build_info();
    test_stats_and_reserve();

    printf("\nCursor / Invariants:\n");
    test_cursor_iteration();

    printf("\nStreaming:\n");
    test_stream_matches_wc_scan_various_chunks();
    test_stream_nomem_allows_continue();
    test_stream_allows_multiple_active_streams();
    test_stream_finish_state_machine();
    test_stream_parent_close_detaches_stream();

#if defined(WC_TEST_OOM) && defined(__GLIBC__)
    printf("\nOOM Injection (glibc-specific):\n");
    test_oom_open();
    test_oom_add();
    test_oom_scan();
    test_oom_results();
#else
    printf("\nOOM: skipped (build with -DWC_TEST_OOM on glibc)\n");
#endif

    printf("\n=== %d/%d passed", g_pass, g_run);
    if (g_fail)
        printf(", %d FAILED", g_fail);
    printf(" ===\n\n");

    return g_fail ? 1 : 0;
}

#endif /* fuzz vs unit tests */

#endif /* WC_NO_TEST_MAIN */

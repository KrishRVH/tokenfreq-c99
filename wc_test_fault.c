#ifndef WC_NO_TEST_MAIN

#include "wordcount.h"
#include "wc_fault_alloc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Helpers --------------------------------------------------------- */

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
        if (!word || count == 0)
            return 0;
        seen++;
        sum += count;
    }
    return (seen == wc_unique(w)) && (sum == wc_total(w));
}

static void make_word4(size_t i, char out[5])
{
    const size_t base = 26u;

    out[0] = (char)('a' + (i / (base * base * base)) % base);
    out[1] = (char)('a' + (i / (base * base)) % base);
    out[2] = (char)('a' + (i / base) % base);
    out[3] = (char)('a' + i % base);
    out[4] = '\0';
}

/* --- Fault tests ----------------------------------------------------- */

static int run_fault_open(int max_fail)
{
    for (int i = 1; i <= max_fail; i++) {
        int open_rc = WC_OK;
        fault_arm(i);
        wc *w = wc_open_ex(0, NULL, &open_rc);
        fault_reset();
        if (w) {
            if (open_rc != WC_OK) {
                wc_close(w);
                return 1;
            }
            wc_close(w);
        } else if (open_rc != WC_NOMEM) {
            return 1;
        }
    }
    return 0;
}

static int run_fault_add(int max_fail)
{
    for (int i = 1; i <= max_fail; i++) {
        wc *w = wc_open(0);
        if (!w)
            return 1;

        fault_arm(i);
        int rc = wc_add(w, "some_unique_word");
        fault_reset();

        if (!(rc == WC_OK || rc == WC_NOMEM)) {
            wc_close(w);
            return 1;
        }
        if (!invariant_cursor_sum_matches_total(w)) {
            wc_close(w);
            return 1;
        }
        wc_close(w);
    }
    return 0;
}

static int run_fault_scan(int max_fail)
{
    const char *t = "the quick brown fox jumps over the lazy dog "
                    "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG "
                    "alpha beta gamma delta epsilon zeta eta theta iota kappa";

    for (int i = 1; i <= max_fail; i++) {
        wc *w = wc_open(0);
        if (!w)
            return 1;

        fault_arm(i);
        int rc = wc_scan(w, t, strlen(t));
        fault_reset();

        if (!(rc == WC_OK || rc == WC_NOMEM)) {
            wc_close(w);
            return 1;
        }
        if (!invariant_cursor_sum_matches_total(w)) {
            wc_close(w);
            return 1;
        }

        /* Duplicates should still be OK even after exhaustion. */
        rc = wc_scan(w, "the the the", 11);
        if (rc != WC_OK) {
            wc_close(w);
            return 1;
        }
        if (!invariant_cursor_sum_matches_total(w)) {
            wc_close(w);
            return 1;
        }

        wc_close(w);
    }
    return 0;
}

static int run_fault_results_topn_stream(int max_fail)
{
    for (int i = 1; i <= max_fail; i++) {
        wc *w = wc_open(0);
        if (!w)
            return 1;

        {
            int rc = wc_scan(w, "alpha beta beta gamma gamma gamma", 31);
            if (rc != WC_OK) {
                wc_close(w);
                return 1;
            }
        }

        /* wc_results fault */
        {
            wc_word *r = NULL;
            size_t n = 0;

            fault_arm(i);
            int rc = wc_results(w, &r, &n);
            fault_reset();

            if (rc == WC_OK) {
                wc_results_free(r);
            } else if (rc != WC_NOMEM) {
                wc_close(w);
                return 1;
            }
        }

        /* wc_topn fault */
        {
            wc_word *r = NULL;
            size_t n = 0;

            fault_arm(i);
            int rc = wc_topn(w, 3, &r, &n);
            fault_reset();

            if (rc == WC_OK) {
                wc_results_free(r);
            } else if (rc != WC_NOMEM) {
                wc_close(w);
                return 1;
            }
        }

        /* wc_stream_open fault */
        {
            int open_rc = WC_OK;
            fault_arm(i);
            wc_stream *s = wc_stream_open(w, &open_rc);
            fault_reset();

            if (s) {
                wc_stream_close(s);
            } else if (!(open_rc == WC_NOMEM || open_rc == WC_ERROR)) {
                wc_close(w);
                return 1;
            }
        }

        if (!invariant_cursor_sum_matches_total(w)) {
            wc_close(w);
            return 1;
        }
        wc_close(w);
    }
    return 0;
}

static int test_stream_finish_nomem_retry(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    wc *w = NULL;
    wc_stream *s = NULL;
    char pending[5];
    size_t before_total = 0;
    size_t before_unique = 0;
    size_t i;
    int rc = WC_OK;
    int found_pending = 0;
    int ok = 0;

    lim.init_cap = 1024;
    lim.block_size = 1;

    w = wc_open_ex(4, &lim, &rc);
    if (!w || rc != WC_OK)
        goto done;
    if (wc_add(w, "aaaa") != WC_OK)
        goto done;

    /*
    ** Fill the current arena block until the next distinct 4-byte word would
    ** require a fresh block allocation. This keeps the test valid when
    ** WC_MIN_BLOCK_SZ changes.
    */
    for (i = 1; i < 1024u; i++) {
        char word[5];

        make_word4(i, word);
        fault_arm(1);
        rc = wc_add(w, word);
        fault_reset();

        if (rc == WC_OK)
            continue;
        if (rc != WC_NOMEM)
            goto done;

        memcpy(pending, word, sizeof pending);
        found_pending = 1;
        break;
    }
    if (!found_pending)
        goto done;

    before_total = wc_total(w);
    before_unique = wc_unique(w);

    s = wc_stream_open(w, &rc);
    if (!s || rc != WC_OK)
        goto done;
    if (wc_stream_scan_ex(s, pending, 4, NULL) != WC_OK)
        goto done;

    fault_arm(1);
    rc = wc_stream_finish(s);
    fault_reset();
    if (rc != WC_NOMEM)
        goto done;
    if (wc_total(w) != before_total || wc_unique(w) != before_unique)
        goto done;

    rc = wc_stream_finish(s);
    if (rc != WC_OK)
        goto done;
    if (wc_total(w) != before_total + 1u || wc_unique(w) != before_unique + 1u)
        goto done;
    if (!invariant_cursor_sum_matches_total(w))
        goto done;

    ok = 1;

done:
    fault_reset();
    if (s)
        wc_stream_close(s);
    if (w)
        wc_close(w);
    return ok ? 0 : 1;
}

static int test_results_free_null_does_not_call_custom_free(void)
{
    int before = fault_null_free_count();
    wc_results_free(NULL);
    return fault_null_free_count() == before ? 0 : 1;
}

int main(void)
{
    const int max_fail = 120; /* keep runtime reasonable under sanitizers */

    printf("\n=== Wordcount Fault Injection Tests (max_fail=%d) ===\n\n",
           max_fail);

    if (test_results_free_null_does_not_call_custom_free())
        return 1;
    if (run_fault_open(max_fail))
        return 1;
    if (run_fault_add(max_fail))
        return 1;
    if (run_fault_scan(max_fail))
        return 1;
    if (run_fault_results_topn_stream(max_fail))
        return 1;
    if (test_stream_finish_nomem_retry())
        return 1;

    printf("\n=== fault tests passed ===\n\n");
    return 0;
}

#endif /* WC_NO_TEST_MAIN */

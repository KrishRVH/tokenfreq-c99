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

/* --- Fault tests ----------------------------------------------------- */

static void run_fault_open(int max_fail)
{
    for (int i = 1; i <= max_fail; i++) {
        int open_rc = WC_OK;
        fault_arm(i);
        wc *w = wc_open_ex(0, NULL, &open_rc);
        fault_reset();
        if (w)
            wc_close(w);
    }
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

int main(void)
{
    const int max_fail = 120; /* keep runtime reasonable under sanitizers */

    printf("\n=== Wordcount Fault Injection Tests (max_fail=%d) ===\n\n",
           max_fail);

    run_fault_open(max_fail);
    if (run_fault_add(max_fail))
        return 1;
    if (run_fault_scan(max_fail))
        return 1;
    if (run_fault_results_topn_stream(max_fail))
        return 1;

    printf("\n=== fault tests passed ===\n\n");
    return 0;
}

#endif /* WC_NO_TEST_MAIN */

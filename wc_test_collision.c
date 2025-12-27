#include "wordcount.h"

#include <stdio.h>
#include <string.h>

static int expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "%s\n", msg);
        return 1;
    }
    return 0;
}

int main(void)
{
    wc_word out[2];
    size_t n = 0;
    int rc = 0;

    wc *w = wc_open(16);
    if (!w) {
        fprintf(stderr, "wc_open failed\n");
        return 1;
    }

    rc |= expect(wc_add_norm_n(w, "alpha", 5) == WC_OK, "add alpha");
    rc |= expect(wc_add_norm_n(w, "beta", 4) == WC_OK, "add beta");

    rc |= expect(wc_unique(w) == 2, "unique count mismatch");
    rc |= expect(wc_total(w) == 2, "total count mismatch");
    rc |= expect(wc_validate(w) == WC_OK, "validate failed");

    rc |= expect(wc_results_into(w, out, 2, &n) == WC_OK,
                 "results_into failed");
    rc |= expect(n == 2, "results length mismatch");
    rc |= expect(out[0].count == 1 && out[1].count == 1, "counts mismatch");
    rc |= expect(strcmp(out[0].word, "alpha") == 0, "word[0] mismatch");
    rc |= expect(strcmp(out[1].word, "beta") == 0, "word[1] mismatch");

    wc_close(w);
    return rc ? 1 : 0;
}

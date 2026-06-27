#include "wordcount.h"

#include <stdio.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static int test_dynamic_full_table_grows(void)
{
    wc_limits lim = WC_LIMITS_INIT();
    wc *w;
    int rc = WC_OK;

    lim.init_cap = 1;
    w = wc_open_ex(4, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);
    CHECK(wc_add(w, "a") == WC_OK);
    CHECK(wc_add(w, "b") == WC_OK);
    CHECK(wc_unique(w) == 2u);
    CHECK(wc_total(w) == 2u);
    wc_close(w);
    return 0;
}

static int test_static_full_table_returns_nomem(void)
{
    WC_STATIC_BUFFER(pool, 4096);
    wc_limits lim = WC_LIMITS_INIT();
    wc *w;
    int rc = WC_OK;

    lim.static_buf = pool.buf;
    lim.static_size = sizeof pool.buf;
    lim.init_cap = 1;

    w = wc_open_ex(4, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);
    CHECK(wc_add(w, "a") == WC_OK);
    CHECK(wc_add(w, "b") == WC_NOMEM);
    CHECK(wc_add(w, "a") == WC_OK);
    CHECK(wc_unique(w) == 1u);
    CHECK(wc_total(w) == 2u);
    wc_close(w);
    return 0;
}

int main(void)
{
    if (test_dynamic_full_table_grows() != 0)
        return 1;
    if (test_static_full_table_returns_nomem() != 0)
        return 1;
    return 0;
}

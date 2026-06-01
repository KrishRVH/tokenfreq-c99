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

int main(void)
{
    wc_limits lim;
    int rc = WC_ERROR;
    wc *w;
    const size_t too_large = (size_t)WC_PTRDIFF_MAX + 1u;

    wc_limits_init(&lim);
    w = wc_open_ex(0, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);
    CHECK(wc_reserve(w, 0, too_large) == WC_NOMEM);
    wc_close(w);

    wc_limits_init(&lim);
    lim.block_size = too_large;
    rc = WC_OK;
    CHECK(wc_open_ex(0, &lim, &rc) == NULL);
    CHECK(rc == WC_EBADLIMITS);

    wc_limits_init(&lim);
    lim.init_cap = 256u;
    rc = WC_OK;
    CHECK(wc_open_ex(0, &lim, &rc) == NULL);
    CHECK(rc == WC_EBADLIMITS);

    {
        WC_STATIC_BUFFER(pool, 8192);
        wc_limits_init(&lim);
        lim.static_buf = pool.buf;
        lim.static_size = too_large;
        rc = WC_OK;
        CHECK(wc_open_ex(0, &lim, &rc) == NULL);
        CHECK(rc == WC_EBADLIMITS);
    }

    return 0;
}

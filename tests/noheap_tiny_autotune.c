#include "wordcount.h"

#define CHECK(expr)          \
    do {                     \
        if (!(expr))         \
            return __LINE__; \
    } while (0)

#if !((WC_NO_HEAP) != 0)
int main(void)
{
    return 1;
}
#else

static int open_and_count(size_t static_size)
{
    wc_limits lim;
    int rc = WC_ERROR;
    WC_STATIC_BUFFER(pool, 512);
    wc *w;

    wc_limits_init(&lim);
    lim.static_buf = pool.buf;
    lim.static_size = static_size;

    w = wc_open_ex(4, &lim, &rc);
    CHECK(w != NULL);
    CHECK(rc == WC_OK);
    CHECK(wc_add_norm_n(w, "AbCd", 4) == WC_OK);
    CHECK(wc_total(w) == 1);
    CHECK(wc_unique(w) == 1);
    wc_close(w);
    return 0;
}

int main(void)
{
    CHECK(open_and_count(280) == 0);
    CHECK(open_and_count(252) == 0);
    return 0;
}
#endif

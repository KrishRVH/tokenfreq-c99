#include "wordcount.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int results_equal_sorted(const wc *a, const wc *b)
{
    wc_word *ra = NULL;
    wc_word *rb = NULL;
    size_t na = 0, nb = 0;

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
    for (size_t i = 0; i < na; i++) {
        if (ra[i].count != rb[i].count || strcmp(ra[i].word, rb[i].word) != 0) {
            wc_results_free(ra);
            wc_results_free(rb);
            return 0;
        }
    }
    wc_results_free(ra);
    wc_results_free(rb);
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size < 2)
        return 0;

    if (size > (1u << 16))
        size = (1u << 16);

    size_t i = 0;
    uint8_t flags = data[i++];

    size_t maxw = (flags & 1u) ? 4u : 64u;
    if (maxw > WC_MAX_WORD)
        maxw = WC_MAX_WORD;

    wc *a = wc_open(maxw);
    wc *b = wc_open(maxw);
    if (!a || !b) {
        wc_close(a);
        wc_close(b);
        return 0;
    }

    const char *buf = (const char *)&data[i];
    size_t len = size - i;

    (void)wc_scan(a, buf, len);

    int open_rc = WC_OK;
    wc_stream *s = wc_stream_open(b, &open_rc);
    if (!s) {
        wc_close(a);
        wc_close(b);
        return 0;
    }

    size_t off = 0;
    while (off < len) {
        size_t step = 1;
        if (len - off >= 4) {
            step = (size_t)(rd_u32((const uint8_t *)buf + off) % 97u) + 1u;
        }
        if (step > len - off)
            step = len - off;

        (void)wc_stream_scan(s, buf + off, step);
        off += step;
    }
    (void)wc_stream_finish(s);
    wc_stream_close(s);

    if (!results_equal_sorted(a, b))
        abort();

    wc_close(a);
    wc_close(b);
    return 0;
}

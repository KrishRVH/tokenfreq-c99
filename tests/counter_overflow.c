/* NOLINTNEXTLINE(bugprone-suspicious-include): white-box overflow test. */
#include "../wordcount.c"

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

static Slot *find_slot(wc *w, const char *word, size_t len)
{
    wc_hash_t h = wc_hash_bytes(word, len, w->seed);
    return tab_find(w, word, len, h, NULL, 0);
}

static int duplicate_insert_rejects_count_wrap(void)
{
    wc *w = wc_open(0);
    Slot *s;

    CHECK(w != NULL);
    CHECK(wc_add(w, "alpha") == WC_OK);

    s = find_slot(w, "alpha", 5);
    CHECK(s != NULL);
    CHECK(s->word != NULL);

    s->cnt = WC_SIZE_MAX;
    w->tot = WC_SIZE_MAX;

    CHECK(wc_add(w, "alpha") == WC_NOMEM);
    CHECK(s->cnt == WC_SIZE_MAX);
    CHECK(w->tot == WC_SIZE_MAX);
    CHECK(wc_validate(w) == WC_OK);

    wc_close(w);
    return 0;
}

static int new_insert_rejects_total_wrap(void)
{
    wc *w = wc_open(0);
    Slot *s;

    CHECK(w != NULL);
    CHECK(wc_add(w, "alpha") == WC_OK);

    s = find_slot(w, "alpha", 5);
    CHECK(s != NULL);
    CHECK(s->word != NULL);

    s->cnt = WC_SIZE_MAX;
    w->tot = WC_SIZE_MAX;

    CHECK(wc_add(w, "beta") == WC_NOMEM);
    CHECK(wc_unique(w) == 1);
    CHECK(wc_total(w) == WC_SIZE_MAX);
    CHECK(wc_validate(w) == WC_OK);

    wc_close(w);
    return 0;
}

static int validate_rejects_total_sum_wrap(void)
{
    wc *w = wc_open(0);
    Slot *a;
    Slot *b;

    CHECK(w != NULL);
    CHECK(wc_add(w, "alpha") == WC_OK);
    CHECK(wc_add(w, "beta") == WC_OK);

    a = find_slot(w, "alpha", 5);
    b = find_slot(w, "beta", 4);
    CHECK(a != NULL);
    CHECK(b != NULL);

    a->cnt = WC_SIZE_MAX;
    b->cnt = 1;
    w->tot = 0;

    CHECK(wc_validate(w) == WC_ERROR);

    wc_close(w);
    return 0;
}

static int validate_rejects_unreachable_slot(void)
{
    wc *w = wc_open(0);
    Slot *s;
    Slot saved;
    Slot zero = { 0 };
    size_t idx;
    size_t bad_idx;

    CHECK(w != NULL);
    CHECK(wc_add(w, "alpha") == WC_OK);

    s = find_slot(w, "alpha", 5);
    CHECK(s != NULL);
    CHECK(s->word != NULL);

    idx = (size_t)(s - w->tab);
    bad_idx = (idx + 2u) & (w->cap - 1u);
    CHECK(bad_idx != idx);
    CHECK(w->tab[bad_idx].word == NULL);

    saved = *s;
    *s = zero;
    w->tab[bad_idx] = saved;

    CHECK(wc_validate(w) == WC_ERROR);

    wc_close(w);
    return 0;
}

static int validate_rejects_duplicate_key(void)
{
    wc *w = wc_open(0);
    Slot *s;
    size_t idx;
    size_t dup_idx;

    CHECK(w != NULL);
    CHECK(wc_add(w, "alpha") == WC_OK);

    s = find_slot(w, "alpha", 5);
    CHECK(s != NULL);
    CHECK(s->word != NULL);

    idx = (size_t)(s - w->tab);
    dup_idx = (idx + 1u) & (w->cap - 1u);
    CHECK(dup_idx != idx);
    CHECK(w->tab[dup_idx].word == NULL);

    w->tab[dup_idx] = *s;
    w->len = 2;
    w->tot = 2;

    CHECK(wc_validate(w) == WC_ERROR);

    wc_close(w);
    return 0;
}

int main(void)
{
    CHECK(duplicate_insert_rejects_count_wrap() == 0);
    CHECK(new_insert_rejects_total_wrap() == 0);
    CHECK(validate_rejects_total_sum_wrap() == 0);
    CHECK(validate_rejects_unreachable_slot() == 0);
    CHECK(validate_rejects_duplicate_key() == 0);
    return 0;
}

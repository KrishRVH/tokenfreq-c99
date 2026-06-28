/* wordcount.c - Implementation
**
** Public domain.
**
** This file implements the core library (wc_open, wc_scan, wc_add, wc_results, etc.)
** and the library-owned streaming scan API (wc_stream_*), so the CLI and library
** cannot drift on tokenization/normalization behavior.
**
** The CLI (wc_main.c) remains responsible for I/O policy (stdin, file walking,
** argument parsing, output formats), but uses the library streaming API for
** chunked input paths.
**
** Notable robustness properties:
**   - Overflow-checked size arithmetic before allocations
**   - Defined behavior on allocation failure (WC_NOMEM)
**   - Default build: SipHash-2-4 keyed from hash_seed
**   - Optional FNV-1a build: consistent 32-bit hashing across platforms
**   - Collision-safe comparisons (stores key length per slot)
**
**   - All allocations that count against wc_limits budgets (table, arena blocks,
**     heap scan/normalization scratch buffer when enabled) are routed through
**     wc_alloc_state.
**     Allocations that are explicitly outside the wc allocator budget use
**     WC_MALLOC/WC_FREE and are not counted: the wc handle in heap-enabled
**     builds, arrays returned by wc_results()/wc_topn(), and the wc_stream
**     object returned by wc_stream_open().
**     Note: the internal table-growth fast path may allocate via WC_MALLOC when
**     the post-grow table fits the budget, so a steady-state fit is not rejected
**     solely because old and new tables coexist during rehash. It is still
**     accounted after rehash.
**
**   - Supported builds require:
**       * CHAR_BIT == 8
**       * ASCII-compatible execution character set
**         ('A'..'Z' == 65..90, 'a'..'z' == 97..122)
**     These are enforced via compile-time assertions.
**
**   The public function signatures use only standard types (`size_t`, `int`, pointers).
**   The public header accepts WC_U32_T/WC_U64_T build macros so internal hashing
**   integer widths stay explicit and portable without leaking those types into
**   API signatures.
*/

#include "wordcount.h"
#define WC_INTERNAL_BOOL(x) ((x) != 0)

#if WC_INTERNAL_BOOL(WC_HAVE_UINTPTR) && \
        WC_INTERNAL_BOOL(WC_LINEAR_UINTPTR_ALIGNMENT)
#include <stdint.h>
#endif
#ifndef WC_OMIT_ASSERT
#if WC_INTERNAL_BOOL(WC_STDC_HOSTED)
#define WC_OMIT_ASSERT 0
#else
#define WC_OMIT_ASSERT 1
#endif
#endif

#if !WC_INTERNAL_BOOL(WC_OMIT_ASSERT)
#include <assert.h>
#define WC_ASSERT(x) assert(x)
#else
#define WC_ASSERT(x) ((void)0)
#endif

#if WC_INTERNAL_BOOL(WC_HAVE_ERRNO)
#include <errno.h>
#endif
#if WC_INTERNAL_BOOL(WC_USE_LIBC_STRING)
#include <string.h>
#endif

#ifndef WC_INTERNAL_TESTING
#define WC_INTERNAL_TESTING 0
#endif
#ifndef WC_INTERNAL_TEST_FORCE_HASH
#define WC_INTERNAL_TEST_FORCE_HASH 0
#endif
#if !WC_INTERNAL_BOOL(WC_INTERNAL_TESTING)
#undef WC_INTERNAL_TEST_FORCE_HASH
#define WC_INTERNAL_TEST_FORCE_HASH 0
#endif

typedef WC_U32_T wc_u32;
#if WC_INTERNAL_BOOL(WC_HASH_STRONG)
typedef WC_U64_T wc_u64;
#endif

#if WC_INTERNAL_BOOL(WC_HAVE_ERRNO)
#define WC_SET_ERRNO(e) \
    do {                \
        errno = (e);    \
    } while (0)
#else
#define WC_SET_ERRNO(e) \
    do {                \
        (void)(e);      \
    } while (0)
#endif

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
static int wc_strcmp_internal(const char *a, const char *b)
{
#if WC_INTERNAL_BOOL(WC_USE_LIBC_STRING)
    return strcmp(a, b);
#else
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (*pa && *pb && *pa == *pb) {
        pa++;
        pb++;
    }
    return (int)*pa - (int)*pb;
#endif
}
#endif

static void *wc_memset_internal(void *dst, int c, size_t n)
{
#if WC_INTERNAL_BOOL(WC_USE_LIBC_STRING)
    return memset(dst, c, n);
#else
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)c;
    return dst;
#endif
}

static void *wc_memcpy_internal(void *dst, const void *src, size_t n)
{
#if WC_INTERNAL_BOOL(WC_USE_LIBC_STRING)
    return memcpy(dst, src, n);
#else
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
#endif
}

static int wc_memcmp_internal(const void *a, const void *b, size_t n)
{
#if WC_INTERNAL_BOOL(WC_USE_LIBC_STRING)
    return memcmp(a, b, n);
#else
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
#endif
}

static const void *wc_memchr_internal(const void *s, int c, size_t n)
{
#if WC_INTERNAL_BOOL(WC_USE_LIBC_STRING)
    return memchr(s, c, n);
#else
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c)
            return p + i;
    }
    return NULL;
#endif
}

static int wc_struct_has_field(size_t struct_size, size_t offset, size_t size)
{
    return offset <= WC_SIZE_MAX - size && struct_size >= offset + size;
}

static int wc_load_limits(wc_limits *out, const wc_limits *in)
{
    size_t in_size;

    if (!out)
        return WC_ERROR;
    if (!in) {
        wc_limits_init(out);
        return WC_OK;
    }

    in_size = in->struct_size;
    if (in_size < offsetof(wc_limits, struct_size) + sizeof(in->struct_size))
        return WC_ERROR;

    wc_limits_init(out);
#define WC_COPY_FIELD(field)                                        \
    do {                                                            \
        if (wc_struct_has_field(in_size,                            \
                                offsetof(wc_limits, field),         \
                                sizeof(((wc_limits *)0)->field))) { \
            out->field = in->field;                                 \
        }                                                           \
    } while (0)

    WC_COPY_FIELD(max_bytes);
    WC_COPY_FIELD(strict_max_bytes);
    WC_COPY_FIELD(init_cap);
    WC_COPY_FIELD(block_size);
    WC_COPY_FIELD(static_buf);
    WC_COPY_FIELD(static_size);
    WC_COPY_FIELD(hash_seed);

#undef WC_COPY_FIELD
    out->struct_size = sizeof(*out);
    return WC_OK;
}

/* --- Compile-time verification of platform requirements ---------------- */
/*
** Prefer C11 _Static_assert when available; otherwise fall back to a
** negative-array-size trick that is widely supported on C99
** toolchains. This is the same style used in many production C
** libraries (e.g., SQLite).
*/
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define WC_STATIC_ASSERT(cond, msg) _Static_assert(cond, #msg)
#else
#define WC_STATIC_ASSERT(cond, msg) \
    typedef char wc_static_assert_##msg[(cond) ? 1 : -1]
#endif

WC_STATIC_ASSERT('A' == 65 && 'Z' == 90 && 'a' == 97 && 'z' == 122 &&
                         ('a' ^ 'A') == 32,
                 ascii_charset_required);

WC_STATIC_ASSERT(CHAR_BIT == 8, char_bit_must_be_8);
WC_STATIC_ASSERT((WC_PTRDIFF_MAX) > 0, wc_ptrdiff_max_must_be_positive);
WC_STATIC_ASSERT(WC_SIZE_MAX == (size_t)-1, wc_size_max_must_match_size_t);
WC_STATIC_ASSERT(sizeof(wc_u32) * CHAR_BIT == 32, wc_u32_must_be_32_bits);
WC_STATIC_ASSERT(((wc_u32)0 - (wc_u32)1) > (wc_u32)0, wc_u32_must_be_unsigned);

WC_STATIC_ASSERT((WC_MAX_WORD) >= 4, wc_max_word_must_be_at_least_4);
WC_STATIC_ASSERT((WC_MAX_WORD) <= (size_t)WC_PTRDIFF_MAX,
                 wc_max_word_must_fit_ptrdiff);
#if WC_INTERNAL_BOOL(WC_STACK_BUFFER)
WC_STATIC_ASSERT((WC_STACK_MAX_WORD) >= 4,
                 wc_stack_max_word_must_be_at_least_4);
WC_STATIC_ASSERT((WC_MAX_WORD) <= (WC_STACK_MAX_WORD),
                 wc_max_word_too_large_for_stack_buffer);
#endif
WC_STATIC_ASSERT((WC_MIN_INIT_CAP) >= 1, wc_min_init_cap_must_be_positive);
WC_STATIC_ASSERT((WC_MIN_BLOCK_SZ) >= 1, wc_min_block_sz_must_be_positive);
WC_STATIC_ASSERT((WC_DEFAULT_INIT_CAP) >= 1,
                 wc_default_init_cap_must_be_positive);
WC_STATIC_ASSERT((WC_DEFAULT_BLOCK_SZ) >= 1,
                 wc_default_block_sz_must_be_positive);
WC_STATIC_ASSERT(((WC_MIN_INIT_CAP) & ((WC_MIN_INIT_CAP)-1u)) == 0,
                 wc_min_init_cap_must_be_power_of_two);
WC_STATIC_ASSERT(((WC_DEFAULT_INIT_CAP) & ((WC_DEFAULT_INIT_CAP)-1u)) == 0,
                 wc_default_init_cap_must_be_power_of_two);
WC_STATIC_ASSERT((WC_DEFAULT_INIT_CAP) >= (WC_MIN_INIT_CAP),
                 wc_default_init_cap_too_small);
WC_STATIC_ASSERT((WC_DEFAULT_BLOCK_SZ) >= (WC_MIN_BLOCK_SZ),
                 wc_default_block_sz_too_small);

#if defined(WC_HAVE_UINTPTR)
/* allow caller override */
#elif defined(UINTPTR_MAX)
#define WC_HAVE_UINTPTR 1
#else
#define WC_HAVE_UINTPTR 0
#endif

/*
** Internal alignment type.
**
** Any object allocated from the internal bump allocator will be
** aligned to WC_ALIGN, which is based on this union. It must cover the
** scalar alignment requirements that malloc is required to satisfy in
** supported builds. Caller-configured hash integer types must not require
** stricter alignment than this baseline.
*/
typedef union {
    void *p;
    size_t sz;
    unsigned long ul;
    long double ld;
} wc_internal_align;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define WC_HAVE_NATIVE_ALIGNOF 1
#define WC_ALIGNOF_TYPE(T) _Alignof(T)
#elif defined(_MSC_VER)
#define WC_HAVE_NATIVE_ALIGNOF 1
#define WC_ALIGNOF_TYPE(T) __alignof(T)
#elif defined(__GNUC__) || defined(__clang__)
#define WC_HAVE_NATIVE_ALIGNOF 1
#define WC_ALIGNOF_TYPE(T) __alignof__(T)
#else
#define WC_HAVE_NATIVE_ALIGNOF 0
#endif

#if WC_HAVE_NATIVE_ALIGNOF
#define WC_ALIGNOF_INTERNAL_ALIGN WC_ALIGNOF_TYPE(wc_internal_align)
#define WC_ALIGNOF_U32 WC_ALIGNOF_TYPE(wc_u32)
#if WC_INTERNAL_BOOL(WC_HASH_STRONG)
#define WC_ALIGNOF_U64 WC_ALIGNOF_TYPE(wc_u64)
#endif
#else
typedef struct {
    char c;
    wc_internal_align x;
} wc_alignof_internal_align_t;
typedef struct {
    char c;
    wc_u32 x;
} wc_alignof_u32_t;
#if WC_INTERNAL_BOOL(WC_HASH_STRONG)
typedef struct {
    char c;
    wc_u64 x;
} wc_alignof_u64_t;
#endif
#define WC_ALIGNOF_INTERNAL_ALIGN \
    ((size_t)offsetof(wc_alignof_internal_align_t, x))
#define WC_ALIGNOF_U32 ((size_t)offsetof(wc_alignof_u32_t, x))
#if WC_INTERNAL_BOOL(WC_HASH_STRONG)
#define WC_ALIGNOF_U64 ((size_t)offsetof(wc_alignof_u64_t, x))
#endif
#endif

#define WC_ALIGN WC_ALIGNOF_INTERNAL_ALIGN
WC_STATIC_ASSERT(WC_ALIGN >= 1u, wc_align_must_be_positive);
WC_STATIC_ASSERT(WC_ALIGN >= WC_ALIGNOF_U32, wc_u32_alignment_too_strict);
#if WC_INTERNAL_BOOL(WC_HASH_STRONG)
WC_STATIC_ASSERT(WC_ALIGN >= WC_ALIGNOF_U64, wc_u64_alignment_too_strict);
#endif

#if defined(_MSC_VER)
#define WC_FLEX_ARRAY 1
#else
#define WC_FLEX_ARRAY
#endif

/*
** Dummy use of wc_internal_align members to satisfy static analyzers
** that warn about unused union members. This generates no code.
*/
static void wc_internal_align_sanity(void)
{
    (void)sizeof(((wc_internal_align *)0)->p);
    (void)sizeof(((wc_internal_align *)0)->sz);
    (void)sizeof(((wc_internal_align *)0)->ul);
    (void)sizeof(((wc_internal_align *)0)->ld);
}

/* --- Implementation-local defaults ------------------------------------ */
/*
** Runtime max_word is clamped into [MIN_WORD, WC_MAX_WORD].
** WC_MAX_WORD and the *_INIT_* / *_BLOCK_* macros come from the header.
*/
#define MIN_WORD 4u
#define DEF_WORD 64u

/* --- 32-bit hash ------------------------------------------------------ */

typedef wc_u32 wc_hash_t;

#define FNV_OFF_32 ((wc_hash_t)2166136261u)
#define FNV_MUL_32 ((wc_hash_t)16777619u)

/* With a 32-bit hash, indexing beyond 2^32 buckets is undefined. */
#define WC_CAP_LIMIT \
    ((WC_SIZE_MAX > 0xffffffffu) ? (((size_t)1u) << 32) : (WC_SIZE_MAX))

static int wc_cap_valid(size_t cap)
{
    if (cap == 0)
        return 0;
    if ((cap & (cap - 1)) != 0)
        return 0;
    if (cap > WC_CAP_LIMIT)
        return 0;
    return 1;
}

/* --- Overflow-safe arithmetic helpers --------------------------------- */

static int add_overflows(size_t a, size_t b)
{
    return a > WC_SIZE_MAX - b;
}

static int mul_overflows(size_t a, size_t b)
{
    return b != 0 && a > WC_SIZE_MAX / b;
}

static int wc_object_span_valid(size_t n)
{
    return n <= (size_t)WC_PTRDIFF_MAX;
}

static int wc_add_object_span(size_t a, size_t b, size_t *out)
{
    size_t n;

    if (!out || add_overflows(a, b))
        return 0;
    n = a + b;
    if (!wc_object_span_valid(n))
        return 0;
    *out = n;
    return 1;
}

static int wc_mul_object_span(size_t a, size_t b, size_t *out)
{
    size_t n;

    if (!out || mul_overflows(a, b))
        return 0;
    n = a * b;
    if (!wc_object_span_valid(n))
        return 0;
    *out = n;
    return 1;
}

static size_t wc_pow2_round_up(size_t n)
{
    size_t p = 1;

    while (p < n && p <= (WC_SIZE_MAX / 2u))
        p <<= 1;
    return p;
}

static size_t wc_pow2_round_down(size_t n)
{
    size_t p = 1;

    while (p <= n / 2u)
        p <<= 1;
    return p;
}

static int wc_min_word_block_size(size_t runtime_max_word, size_t *out)
{
    size_t need;

    if (!out)
        return 0;
    if (!wc_add_object_span(runtime_max_word, 1u, &need))
        return 0;
    if (!wc_add_object_span(need, WC_ALIGN - 1u, &need))
        return 0;
    if (need < WC_MIN_BLOCK_SZ)
        need = WC_MIN_BLOCK_SZ;
    if (!wc_object_span_valid(need))
        return 0;
    *out = need;
    return 1;
}

#if (WC_INTERNAL_BOOL(WC_HAVE_UINTPTR) &&              \
     WC_INTERNAL_BOOL(WC_LINEAR_UINTPTR_ALIGNMENT)) || \
        WC_INTERNAL_BOOL(WC_TRUST_STATIC_BUFFER_ALIGNMENT)
static int wc_ptr_aligned(const void *p, size_t align)
{
    if (!p || align == 0)
        return 0;
#if WC_INTERNAL_BOOL(WC_HAVE_UINTPTR) && \
        WC_INTERNAL_BOOL(WC_LINEAR_UINTPTR_ALIGNMENT)
    return ((uintptr_t)p % align) == 0;
#else
    (void)align;
#if WC_INTERNAL_BOOL(WC_TRUST_STATIC_BUFFER_ALIGNMENT)
    return 1;
#else
    return 0;
#endif
#endif
}
#endif

#if WC_INTERNAL_BOOL(WC_NO_HEAP)
static int wc_align_size(size_t n, size_t align, size_t *out)
{
    size_t pad;

    if (!out || align == 0)
        return 0;
    pad = (align - (n % align)) % align;
    if (add_overflows(n, pad))
        return 0;
    *out = n + pad;
    return 1;
}
#endif

/* --- Arena allocator --------------------------------------------------- */

typedef struct Block Block;
struct Block {
    Block *next;
    char *cur;
    char *end;
    char buf[WC_FLEX_ARRAY];
};

typedef struct {
    Block *head;
    Block *tail;
    size_t block_sz;
    size_t blocks;
} Arena;

/* --- Hash table slot --------------------------------------------------- */

/* Store key length to make comparisons collision-safe and OOB-proof. */
typedef struct {
    char *word;
    size_t n; /* stored key length (excluding NUL) */
    size_t cnt;
    wc_hash_t hash; /* exact 32-bit hash */
} Slot;

/* --- Internal allocation state ---------------------------------------- */
/*
** All internal allocations (hash table, arena blocks, optional scan
** buffer when WC_STACK_BUFFER == 0) are tracked through this state.
**
** Dynamic mode:
**   - static_mode == 0
**   - allocations use WC_MALLOC / WC_FREE
**   - bytes_used is kept in sync and enforced against bytes_limit
**
** Static-buffer mode:
**   - static_mode == 1
**   - allocations are carved from [sbuf, sbuf + sbuf_size) via
**     a bump allocator with WC_ALIGN alignment
**   - allocations are zero-initialized
**   - bytes_used and sbuf_used grow monotonically; there is no
**     reuse or free inside the static buffer
*/
typedef struct {
    /* bytes_used/bytes_limit represent bytes consumed from the internal
       allocator (requested bytes). In static-buffer mode, this includes
       alignment padding. */
    size_t bytes_used;
    size_t bytes_limit; /* 0 = unlimited */
    int static_mode;    /* 0 = dynamic, 1 = static-buffer */

    unsigned char *sbuf;
    size_t sbuf_size;
    size_t sbuf_used;
} wc_alloc_state;

struct wc_stream {
    wc *w;
    wc_stream *prev;
    wc_stream *next;
    size_t cap;    /* == w->maxw while attached */
    size_t len;    /* current length of in-progress word (<= cap) */
    int finished;  /* 1 after wc_stream_finish */
    int finish_rc; /* rc from finish for idempotence */
    int owns_self; /* 1 if allocated by library */
    char *buf;
    char storage[WC_FLEX_ARRAY]; /* cap bytes + 1 NUL (allocated) */
};

/* --- wc object --------------------------------------------------------- */

struct wc {
    Slot *tab;
    size_t cap;  /* power-of-two capacity */
    size_t len;  /* unique words */
    size_t tot;  /* total words */
    size_t maxw; /* maximum stored word length */
    int strict_max_bytes;

    Arena arena;
    wc_alloc_state alloc;
    wc_hash_t seed; /* 32-bit seed */
    int owns_self;

#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
    char *scanbuf; /* per-instance scan/normalization scratch buffer */
    size_t scanbuf_size;
#endif
    wc_stream *streams;
};

#if WC_HAVE_NATIVE_ALIGNOF
#define WC_ALIGNOF_SLOT WC_ALIGNOF_TYPE(Slot)
#define WC_ALIGNOF_WC WC_ALIGNOF_TYPE(struct wc)
#else
typedef struct {
    char c;
    Slot x;
} wc_alignof_slot_t;
typedef struct {
    char c;
    struct wc x;
} wc_alignof_wc_t;
#define WC_ALIGNOF_SLOT ((size_t)offsetof(wc_alignof_slot_t, x))
#define WC_ALIGNOF_WC ((size_t)offsetof(wc_alignof_wc_t, x))
#endif

WC_STATIC_ASSERT(WC_ALIGN >= WC_ALIGNOF_SLOT, wc_align_must_cover_slot);
WC_STATIC_ASSERT(WC_ALIGN >= WC_ALIGNOF_WC, wc_align_must_cover_wc);
WC_STATIC_ASSERT(sizeof(Block) <= (size_t)WC_PTRDIFF_MAX,
                 block_must_fit_ptrdiff);
WC_STATIC_ASSERT((size_t)WC_PTRDIFF_MAX >=
                         sizeof(Block) + WC_ALIGN + (size_t)MIN_WORD,
                 wc_ptrdiff_max_too_small_for_min_word_block);
WC_STATIC_ASSERT((WC_MAX_WORD) <=
                         (size_t)WC_PTRDIFF_MAX - sizeof(Block) - WC_ALIGN,
                 wc_max_word_must_leave_block_overhead);
WC_STATIC_ASSERT((WC_MIN_BLOCK_SZ) <= (size_t)WC_PTRDIFF_MAX - sizeof(Block),
                 wc_min_block_sz_must_leave_block_overhead);
WC_STATIC_ASSERT((WC_DEFAULT_BLOCK_SZ) <=
                         (size_t)WC_PTRDIFF_MAX - sizeof(Block),
                 wc_default_block_sz_must_leave_block_overhead);
WC_STATIC_ASSERT(sizeof(Slot) <= (size_t)WC_PTRDIFF_MAX, slot_must_fit_ptrdiff);
WC_STATIC_ASSERT((WC_MIN_INIT_CAP) <= WC_CAP_LIMIT,
                 wc_min_init_cap_exceeds_hash_cap_limit);
WC_STATIC_ASSERT((WC_DEFAULT_INIT_CAP) <= WC_CAP_LIMIT,
                 wc_default_init_cap_exceeds_hash_cap_limit);
WC_STATIC_ASSERT((WC_MIN_INIT_CAP) <= (size_t)WC_PTRDIFF_MAX / sizeof(Slot),
                 wc_min_init_cap_table_must_fit_ptrdiff);
WC_STATIC_ASSERT((WC_DEFAULT_INIT_CAP) <= (size_t)WC_PTRDIFF_MAX / sizeof(Slot),
                 wc_default_init_cap_table_must_fit_ptrdiff);
WC_STATIC_ASSERT(sizeof(struct wc) <= (size_t)WC_PTRDIFF_MAX,
                 wc_must_fit_ptrdiff);
WC_STATIC_ASSERT(sizeof(struct wc_stream) <= (size_t)WC_PTRDIFF_MAX,
                 wc_stream_must_fit_ptrdiff);
WC_STATIC_ASSERT((size_t)WC_PTRDIFF_MAX >=
                         sizeof(struct wc_stream) + (size_t)MIN_WORD + 1u,
                 wc_ptrdiff_max_too_small_for_min_word_stream);
WC_STATIC_ASSERT((WC_MAX_WORD) <=
                         (size_t)WC_PTRDIFF_MAX - sizeof(struct wc_stream) - 1u,
                 wc_max_word_must_leave_stream_overhead);

static void wc_alloc_state_init(wc_alloc_state *st)
{
    if (!st)
        return;
    st->bytes_used = 0;
    st->bytes_limit = 0;
    st->static_mode = 0;
    st->sbuf = NULL;
    st->sbuf_size = 0;
    st->sbuf_used = 0;
}

static void wc_arena_init_empty(Arena *a)
{
    if (!a)
        return;
    a->head = NULL;
    a->tail = NULL;
    a->block_sz = 0;
    a->blocks = 0;
}

static void wc_handle_init(wc *w)
{
    if (!w)
        return;
    w->tab = NULL;
    w->cap = 0;
    w->len = 0;
    w->tot = 0;
    w->maxw = 0;
    w->strict_max_bytes = 0;
    wc_arena_init_empty(&w->arena);
    wc_alloc_state_init(&w->alloc);
    w->seed = (wc_hash_t)0;
    w->owns_self = 0;
#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
    w->scanbuf = NULL;
    w->scanbuf_size = 0;
#endif
    w->streams = NULL;
}

static void wc_slot_init(Slot *s)
{
    if (!s)
        return;
    s->word = NULL;
    s->n = 0;
    s->cnt = 0;
    s->hash = (wc_hash_t)0;
}

static void wc_slots_init(Slot *tab, size_t cap)
{
    size_t i;

    if (!tab)
        return;
    for (i = 0; i < cap; i++)
        wc_slot_init(&tab[i]);
}

static void wc_stream_mark_detached(wc_stream *s)
{
    if (!s)
        return;
    s->w = NULL;
    s->prev = NULL;
    s->next = NULL;
    s->cap = 0;
    s->len = 0;
    s->finished = 1;
    s->finish_rc = WC_ERROR;
    s->buf = NULL;
}

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
static void wc_stream_link(wc *w, wc_stream *s)
{
    if (!w || !s)
        return;

    s->w = w;
    s->prev = NULL;
    s->next = w->streams;
    if (w->streams)
        w->streams->prev = s;
    w->streams = s;
}
#endif

static void wc_stream_unlink(wc_stream *s)
{
    wc *w;

    if (!s || !s->w)
        return;

    w = s->w;
    if (s->prev)
        s->prev->next = s->next;
    else if (w->streams == s)
        w->streams = s->next;

    if (s->next)
        s->next->prev = s->prev;

    s->w = NULL;
    s->prev = NULL;
    s->next = NULL;
}

static void wc_stream_detach_all(wc *w)
{
    wc_stream *s;

    if (!w)
        return;

    s = w->streams;
    while (s) {
        wc_stream *next = s->next;
        wc_stream_mark_detached(s);
        s = next;
    }
    w->streams = NULL;
}

/* Shallow sanity check: prevent obvious UB if state is inconsistent. */
static int wc_ready(const wc *w)
{
    if (!w)
        return 0;
    if (!w->tab)
        return 0;
    if (!wc_cap_valid(w->cap))
        return 0;
    if (w->len > w->cap)
        return 0;
    if (w->maxw < MIN_WORD || w->maxw > WC_MAX_WORD)
        return 0;
    if (!w->arena.head || !w->arena.tail)
        return 0;
#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
    if (!w->scanbuf)
        return 0;
    if (w->scanbuf_size < w->maxw)
        return 0;
#endif
    return 1;
}

/* --- Forward declarations --------------------------------------------- */

static void *wc_xmalloc_state(wc_alloc_state *st, size_t n);
static void *wc_xmalloc(wc *w, size_t n);
static void wc_xfree(wc *w, void *p, size_t n);

static Block *block_new(wc *w, size_t cap);
static int arena_init(wc *w, Arena *a, size_t block_sz);
static void arena_free(wc *w);
static void *arena_alloc(wc *w, size_t sz);

#if !WC_INTERNAL_BOOL(WC_HASH_STRONG) && \
        !WC_INTERNAL_BOOL(WC_INTERNAL_TEST_FORCE_HASH)
static wc_hash_t fnv32(const char *s, size_t n, wc_hash_t seed_basis);
#endif

static int tab_grow(wc *w);
static Slot *tab_find(wc *w, const char *word, size_t n, wc_hash_t h);
static int tab_insert(wc *w, const char *word, size_t n, wc_hash_t h);

static void tune_params(
        const wc_limits *lim,
        size_t *init_cap,
        size_t *block_sz); /* NOLINT(bugprone-easily-swappable-parameters) */
static int wc_is_word_byte(unsigned char c);
static unsigned char wc_tolower_byte(unsigned char c);

/* --- Allocation helpers ------------------------------------------------ */
/*
** Central allocation helper on a raw state object.
**
** In dynamic mode this wraps WC_MALLOC and enforces bytes_limit.
** In static-buffer mode it carves from sbuf via a bump allocator with
** WC_ALIGN alignment and enforces both sbuf_size and bytes_limit.
**
** On any arithmetic overflow or limit violation, this function fails
** cleanly (returns NULL) rather than risking UB.
*/
static void *wc_xmalloc_state(wc_alloc_state *st, size_t n)
{
    void *p;

    if (!st || n == 0)
        return NULL;
    if (!wc_object_span_valid(n))
        return NULL;

#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    /* No-heap builds support only static-buffer mode. */
    if (!st->static_mode)
        return NULL;
#else
    if (!st->static_mode) {
        size_t new_used;

        if (add_overflows(st->bytes_used, n))
            return NULL;

        new_used = st->bytes_used + n;
        if (st->bytes_limit && new_used > st->bytes_limit)
            return NULL;

        p = WC_MALLOC(n);
        if (!p)
            return NULL;

        st->bytes_used = new_used;
        return p;
    }
#endif

    /* Static-buffer mode block */
    {
        const size_t align = WC_ALIGN;
        const size_t off = st->sbuf_used;
        const size_t pad = (align - (off % align)) % align;
        size_t real;
        size_t new_used;

        if (!wc_object_span_valid(st->sbuf_size) ||
            !wc_object_span_valid(st->sbuf_used))
            return NULL;
        if (add_overflows(pad, n))
            return NULL;
        real = pad + n;
        if (!wc_object_span_valid(real))
            return NULL;

        if (add_overflows(st->sbuf_used, real))
            return NULL;
        if (!wc_object_span_valid(st->sbuf_used + real))
            return NULL;
        if (st->sbuf_used + real > st->sbuf_size)
            return NULL;

        if (add_overflows(st->bytes_used, real))
            return NULL;
        new_used = st->bytes_used + real;
        if (st->bytes_limit && new_used > st->bytes_limit)
            return NULL;

        p = (void *)(st->sbuf + st->sbuf_used + pad);
        st->sbuf_used += real;
        st->bytes_used = new_used;
        return wc_memset_internal(p, 0, n);
    }
}

/*
** Dry-run allocator: updates accounting exactly like wc_xmalloc_state()
** but does not touch memory (no malloc, no memset). Used to guarantee
** that static-buffer "preflight" is side-effect free.
**
** Returns 1 if the allocation would succeed, 0 otherwise.
*/
static int wc_xdryrun_state(wc_alloc_state *st, size_t n)
{
    if (!st || n == 0)
        return 0;
    if (!wc_object_span_valid(n))
        return 0;

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (!st->static_mode) {
        size_t new_used;
        if (add_overflows(st->bytes_used, n))
            return 0;
        new_used = st->bytes_used + n;
        if (st->bytes_limit && new_used > st->bytes_limit)
            return 0;
        st->bytes_used = new_used;
        return 1;
    }
#else
    /* In no-heap builds, dynamic mode is unsupported. */
    if (!st->static_mode)
        return 0;
#endif

    /* Static-buffer mode: include alignment padding in budget. */
    {
        const size_t align = WC_ALIGN;
        const size_t off = st->sbuf_used;
        const size_t pad = (align - (off % align)) % align;
        size_t real;
        size_t new_used;

        if (!wc_object_span_valid(st->sbuf_size) ||
            !wc_object_span_valid(st->sbuf_used))
            return 0;
        if (add_overflows(pad, n))
            return 0;
        real = pad + n;
        if (!wc_object_span_valid(real))
            return 0;

        if (add_overflows(st->sbuf_used, real))
            return 0;
        if (!wc_object_span_valid(st->sbuf_used + real))
            return 0;
        if (st->sbuf_used + real > st->sbuf_size)
            return 0;

        if (add_overflows(st->bytes_used, real))
            return 0;
        new_used = st->bytes_used + real;
        if (st->bytes_limit && new_used > st->bytes_limit)
            return 0;

        st->sbuf_used += real;
        st->bytes_used = new_used;
        return 1;
    }
}

static int wc_static_layout_fits(size_t static_size,
                                 size_t static_reserve,
                                 size_t bytes_limit,
                                 size_t table_bytes,
                                 size_t block_sz,
                                 size_t scan_bytes)
{
    size_t arena_bytes;
    wc_alloc_state scratch;

    if (!wc_add_object_span(sizeof(Block), block_sz, &arena_bytes))
        return 0;

    wc_alloc_state_init(&scratch);
    scratch.bytes_used = static_reserve;
    scratch.bytes_limit = bytes_limit;
    scratch.static_mode = 1;
    scratch.sbuf_size = static_size;
    scratch.sbuf_used = static_reserve;

    if (!wc_xdryrun_state(&scratch, table_bytes))
        return 0;
    if (!wc_xdryrun_state(&scratch, arena_bytes))
        return 0;
    if (scan_bytes && !wc_xdryrun_state(&scratch, scan_bytes))
        return 0;

    return 1;
}

static size_t wc_static_max_block_size(size_t static_size,
                                       size_t static_reserve,
                                       size_t bytes_limit,
                                       size_t table_bytes,
                                       size_t min_block_sz,
                                       size_t scan_bytes)
{
    size_t lo = min_block_sz;
    size_t hi;

    if (static_size <= static_reserve)
        return min_block_sz;

    hi = static_size - static_reserve;
    if (hi > (size_t)WC_PTRDIFF_MAX - sizeof(Block))
        hi = (size_t)WC_PTRDIFF_MAX - sizeof(Block);

    if (hi < lo || !wc_static_layout_fits(static_size,
                                          static_reserve,
                                          bytes_limit,
                                          table_bytes,
                                          lo,
                                          scan_bytes)) {
        return min_block_sz;
    }

    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1u) / 2u;
        if (wc_static_layout_fits(static_size,
                                  static_reserve,
                                  bytes_limit,
                                  table_bytes,
                                  mid,
                                  scan_bytes)) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }

    return lo;
}

static int wc_static_choose_layout(size_t static_size,
                                   size_t static_reserve,
                                   size_t bytes_limit,
                                   size_t min_block_sz,
                                   size_t scan_bytes,
                                   int explicit_init_cap,
                                   int explicit_block_size,
                                   size_t *init_cap,
                                   size_t *block_sz,
                                   size_t *table_bytes)
{
    size_t cap;
    size_t min_cap;

    if (!init_cap || !block_sz || !table_bytes)
        return 0;

    cap = *init_cap;
    min_cap = explicit_init_cap ? cap : wc_pow2_round_up(WC_MIN_INIT_CAP);
    if (!wc_cap_valid(cap) || !wc_cap_valid(min_cap) || cap < min_cap)
        return 0;

    for (;;) {
        size_t candidate_block = *block_sz;
        size_t candidate_table_bytes;

        if (!wc_mul_object_span(cap, sizeof(Slot), &candidate_table_bytes))
            return 0;

        if (!explicit_block_size) {
            candidate_block = wc_static_max_block_size(static_size,
                                                       static_reserve,
                                                       bytes_limit,
                                                       candidate_table_bytes,
                                                       min_block_sz,
                                                       scan_bytes);
        }

        if (wc_object_span_valid(candidate_block) &&
            wc_static_layout_fits(static_size,
                                  static_reserve,
                                  bytes_limit,
                                  candidate_table_bytes,
                                  candidate_block,
                                  scan_bytes)) {
            *init_cap = cap;
            *block_sz = candidate_block;
            *table_bytes = candidate_table_bytes;
            return 1;
        }

        if (explicit_init_cap || cap == min_cap)
            break;

        cap >>= 1;
        if (cap < min_cap)
            cap = min_cap;
    }

    return 0;
}

static int wc_dynamic_layout_fits(size_t bytes_limit,
                                  size_t table_bytes,
                                  size_t block_sz,
                                  size_t scan_bytes)
{
    size_t arena_bytes;
    size_t need;

    if (!bytes_limit)
        return 1;
    if (!wc_add_object_span(sizeof(Block), block_sz, &arena_bytes))
        return 0;

    need = table_bytes;
    if (add_overflows(need, arena_bytes))
        return 0;
    need += arena_bytes;
    if (add_overflows(need, scan_bytes))
        return 0;
    need += scan_bytes;

    return need <= bytes_limit;
}

static int wc_dynamic_max_block_size(size_t bytes_limit,
                                     size_t table_bytes,
                                     size_t min_block_sz,
                                     size_t scan_bytes,
                                     size_t *out)
{
    size_t fixed;
    size_t max_block;
    const size_t max_object_block = (size_t)WC_PTRDIFF_MAX - sizeof(Block);

    if (!out || !bytes_limit)
        return 0;

    fixed = table_bytes;
    if (add_overflows(fixed, sizeof(Block)))
        return 0;
    fixed += sizeof(Block);
    if (add_overflows(fixed, scan_bytes))
        return 0;
    fixed += scan_bytes;

    if (fixed > bytes_limit)
        return 0;

    max_block = bytes_limit - fixed;
    if (max_block > max_object_block)
        max_block = max_object_block;
    if (max_block < min_block_sz)
        return 0;

    *out = max_block;
    return 1;
}

static int wc_dynamic_choose_layout(size_t bytes_limit,
                                    size_t min_block_sz,
                                    size_t scan_bytes,
                                    int explicit_init_cap,
                                    int explicit_block_size,
                                    size_t *init_cap,
                                    size_t *block_sz,
                                    size_t *table_bytes)
{
    size_t cap;
    size_t min_cap;

    if (!init_cap || !block_sz || !table_bytes)
        return 0;

    cap = *init_cap;
    min_cap = explicit_init_cap ? cap : wc_pow2_round_up(WC_MIN_INIT_CAP);
    if (!wc_cap_valid(cap) || !wc_cap_valid(min_cap) || cap < min_cap)
        return 0;

    for (;;) {
        size_t candidate_block = *block_sz;
        size_t candidate_table_bytes;

        if (!wc_mul_object_span(cap, sizeof(Slot), &candidate_table_bytes))
            return 0;

        if (!explicit_block_size) {
            size_t max_block;
            if (wc_dynamic_max_block_size(bytes_limit,
                                          candidate_table_bytes,
                                          min_block_sz,
                                          scan_bytes,
                                          &max_block) &&
                candidate_block > max_block) {
                candidate_block = max_block;
            }
        }

        if (candidate_block < min_block_sz)
            candidate_block = min_block_sz;

        if (wc_object_span_valid(candidate_block) &&
            wc_dynamic_layout_fits(bytes_limit,
                                   candidate_table_bytes,
                                   candidate_block,
                                   scan_bytes)) {
            *init_cap = cap;
            *block_sz = candidate_block;
            *table_bytes = candidate_table_bytes;
            return 1;
        }

        if (explicit_init_cap || cap == min_cap)
            break;

        cap >>= 1;
        if (cap < min_cap)
            cap = min_cap;
    }

    return 0;
}

/*
** Convenience wrapper for the wc object.
*/
static void *wc_xmalloc(wc *w, size_t n)
{
    return w ? wc_xmalloc_state(&w->alloc, n) : NULL;
}

/*
** Internal free helper.
**
** In dynamic mode it calls WC_FREE and decrements bytes_used.
** In static-buffer mode it is a no-op; memory is never recycled
** inside the static buffer.
*/
/* cppcheck-suppress constParameterPointer
** Heap builds mutate w->alloc, and p must remain mutable for WC_FREE.
*/
static void wc_xfree(wc *w, void *p, size_t n)
{
    if (!w || !p)
        return;

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (!w->alloc.static_mode) {
        WC_FREE(p);
#if !WC_INTERNAL_BOOL(WC_OMIT_ASSERT)
        WC_ASSERT(w->alloc.bytes_used >= n);
#endif
        if (w->alloc.bytes_used >= n)
            w->alloc.bytes_used -= n;
        else
            w->alloc.bytes_used = 0;
        return;
    }
#endif
    (void)n; /* static-buffer mode: no-op */
}

/* --- Arena implementation ---------------------------------------------- */

static Block *block_new(wc *w, size_t cap)
{
    Block *b;
    size_t total;

    if (!wc_add_object_span(sizeof(Block), cap, &total))
        return NULL;

    b = (Block *)wc_xmalloc(w, total);
    if (!b)
        return NULL;

    b->next = NULL;
    b->cur = b->buf;
    b->end = b->buf + cap;

    return b;
}

static int arena_init(wc *w, Arena *a, size_t block_sz)
{
    Block *b;

    WC_ASSERT(w != NULL);
    WC_ASSERT(a != NULL);

    a->head = a->tail = NULL;
    a->block_sz = block_sz;
    a->blocks = 0;

    b = block_new(w, block_sz);
    if (!b)
        return -1;

    a->head = a->tail = b;
    a->blocks = 1;
    return 0;
}

static void arena_free(wc *w)
{
    Block *b;

    if (!w)
        return;

    b = w->arena.head;
    while (b) {
        Block *n = b->next;
        size_t cap = (size_t)(b->end - b->buf);
        size_t total = sizeof(Block) + cap;
        wc_xfree(w, b, total);
        b = n;
    }

    w->arena.head = w->arena.tail = NULL;
    w->arena.blocks = 0;
}

static int arena_tail_available(const Arena *a, size_t *out)
{
    const size_t align = WC_ALIGN;
    const char *cur;
    size_t pad;
    size_t avail;

    if (!a || !a->tail || !out)
        return 0;

    cur = a->tail->cur;
#if WC_INTERNAL_BOOL(WC_HAVE_UINTPTR) && \
        WC_INTERNAL_BOOL(WC_LINEAR_UINTPTR_ALIGNMENT)
    {
        const uintptr_t cur_addr = (uintptr_t)cur;
        pad = (size_t)((align - (cur_addr % align)) % align);
    }
#else
    {
        size_t offset = (size_t)(cur - a->tail->buf);
        if (add_overflows(offsetof(Block, buf), offset))
            return 0;
        offset += offsetof(Block, buf);
        pad = (align - (offset % align)) % align;
    }
#endif

    avail = (size_t)(a->tail->end - a->tail->cur);
    *out = (avail < pad) ? 0u : (avail - pad);
    return 1;
}

static int
arena_new_block_allocation_size(const Arena *a, size_t sz, size_t *out)
{
    size_t need;
    size_t cap;

    if (!a || !out)
        return 0;
    if (!wc_add_object_span(sz, WC_ALIGN, &need))
        return 0;
    cap = need > a->block_sz ? need : a->block_sz;
    if (!wc_add_object_span(sizeof(Block), cap, out))
        return 0;
    return 1;
}

/*
** Arena allocation with WC_ALIGN alignment.
**
** In static-buffer mode, the arena never extends beyond the initial
** block; further requests fail with NULL and are mapped to WC_NOMEM
** by callers.
*/
static void *arena_alloc(wc *w, size_t sz)
{
    const size_t align = WC_ALIGN;
    size_t pad;
    size_t avail;
    size_t cap;
    size_t need;
    char *p;
    Block *b;
    Arena *a;

    WC_ASSERT(w != NULL);
    a = &w->arena;
    WC_ASSERT(a->tail != NULL);
    if (!wc_object_span_valid(sz))
        return NULL;

    {
        const char *cur = a->tail->cur;
#if WC_INTERNAL_BOOL(WC_HAVE_UINTPTR) && \
        WC_INTERNAL_BOOL(WC_LINEAR_UINTPTR_ALIGNMENT)
        const uintptr_t cur_addr = (uintptr_t)cur;
        pad = (size_t)((align - (cur_addr % align)) % align);
#else
        size_t offset = (size_t)(cur - a->tail->buf);
        if (add_overflows(offsetof(Block, buf), offset))
            return NULL;
        offset += offsetof(Block, buf);
        pad = (align - (offset % align)) % align;
#endif
    }

    avail = (size_t)(a->tail->end - a->tail->cur);
    if (avail >= pad && avail - pad >= sz) {
        char *cur = a->tail->cur;
        p = cur + pad;
        a->tail->cur = p + sz;
        WC_ASSERT(a->tail->cur <= a->tail->end);
        return wc_memset_internal(p, 0, sz);
    }

    if (w->alloc.static_mode)
        return NULL;

    if (!wc_add_object_span(sz, align, &need))
        return NULL;
    cap = need > a->block_sz ? need : a->block_sz;

    b = block_new(w, cap);
    if (!b)
        return NULL;

    a->tail->next = b;
    a->tail = b;
    a->blocks++;

    /* Fresh block starts at allocator alignment; pad from the address to
       preserve WC_ALIGN even if the header offset is not aligned. */
    {
        char *cur = b->cur;
#if WC_INTERNAL_BOOL(WC_HAVE_UINTPTR) && \
        WC_INTERNAL_BOOL(WC_LINEAR_UINTPTR_ALIGNMENT)
        const uintptr_t cur_addr = (uintptr_t)cur;
        pad = (size_t)((align - (cur_addr % align)) % align);
#else
        size_t offset = offsetof(Block, buf);
        pad = (align - (offset % align)) % align;
#endif
        p = cur + pad;
    }
    b->cur = p + sz;
    WC_ASSERT(b->cur <= b->end);

    return wc_memset_internal(p, 0, sz);
}

/* --- Hash table -------------------------------------------------------- */
/* Equivalent to: (len * 10 >= cap * 7), but without risky wide multiplies.
   cap is always > 0 (power of two). */
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static int tab_load_factor_hit(
        size_t len,
        size_t cap) /* NOLINT(bugprone-easily-swappable-parameters) */
{
    /* Trigger point is ceil(cap * 0.7) == ceil(cap*7/10). */
    const size_t q = cap / 10u;
    const size_t r = cap % 10u;
    const size_t threshold = q * 7u + (r * 7u + 9u) / 10u; /* ceil(r*7/10) */
    return len >= threshold;
}

#if !WC_INTERNAL_BOOL(WC_HASH_STRONG) && \
        !WC_INTERNAL_BOOL(WC_INTERNAL_TEST_FORCE_HASH)
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static wc_hash_t
fnv32(const char *s,
      size_t n,
      wc_hash_t seed_basis) /* NOLINT(bugprone-easily-swappable-parameters) */
{
    wc_hash_t h = seed_basis;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (wc_hash_t)(unsigned char)s[i];
        h *= FNV_MUL_32;
    }

    return h;
}
#endif /* !WC_HASH_STRONG && !WC_INTERNAL_TEST_FORCE_HASH */

#if WC_INTERNAL_BOOL(WC_HASH_STRONG) && \
        !WC_INTERNAL_BOOL(WC_INTERNAL_TEST_FORCE_HASH)
WC_STATIC_ASSERT(sizeof(wc_u64) * CHAR_BIT == 64, wc_u64_must_be_64_bits);
WC_STATIC_ASSERT(((wc_u64)0 - (wc_u64)1) > (wc_u64)0, wc_u64_must_be_unsigned);
static wc_u64 sip_rotl(wc_u64 v, int b)
{
    return (wc_u64)((v << b) | (v >> (wc_u64)(64 - b)));
}

#define SIPROUND()             \
    do {                       \
        v0 += v1;              \
        v1 = sip_rotl(v1, 13); \
        v1 ^= v0;              \
        v0 = sip_rotl(v0, 32); \
        v2 += v3;              \
        v3 = sip_rotl(v3, 16); \
        v3 ^= v2;              \
        v0 += v3;              \
        v3 = sip_rotl(v3, 21); \
        v3 ^= v0;              \
        v2 += v1;              \
        v1 = sip_rotl(v1, 17); \
        v1 ^= v2;              \
        v2 = sip_rotl(v2, 32); \
    } while (0)

static wc_u64
siphash24(const unsigned char *in, size_t inlen, wc_u64 k0, wc_u64 k1)
{
    wc_u64 v0 = (wc_u64)0x736f6d6570736575ULL ^ k0;
    wc_u64 v1 = (wc_u64)0x646f72616e646f6dULL ^ k1;
    wc_u64 v2 = (wc_u64)0x6c7967656e657261ULL ^ k0;
    wc_u64 v3 = (wc_u64)0x7465646279746573ULL ^ k1;
    wc_u64 b = ((wc_u64)inlen) << 56;
    size_t i;

    for (i = 0; i + 7 < inlen; i += 8) {
        wc_u64 m = ((wc_u64)in[i]) | ((wc_u64)in[i + 1] << 8) |
                   ((wc_u64)in[i + 2] << 16) | ((wc_u64)in[i + 3] << 24) |
                   ((wc_u64)in[i + 4] << 32) | ((wc_u64)in[i + 5] << 40) |
                   ((wc_u64)in[i + 6] << 48) | ((wc_u64)in[i + 7] << 56);

        v3 ^= m;
        SIPROUND();
        SIPROUND();
        v0 ^= m;
    }

    switch (inlen & 7u) {
        case 7:
            b |= ((wc_u64)in[i + 6]) << 48;
            /* fall through */
        case 6:
            b |= ((wc_u64)in[i + 5]) << 40;
            /* fall through */
        case 5:
            b |= ((wc_u64)in[i + 4]) << 32;
            /* fall through */
        case 4:
            b |= ((wc_u64)in[i + 3]) << 24;
            /* fall through */
        case 3:
            b |= ((wc_u64)in[i + 2]) << 16;
            /* fall through */
        case 2:
            b |= ((wc_u64)in[i + 1]) << 8;
            /* fall through */
        case 1:
            b |= ((wc_u64)in[i + 0]);
            /* fall through */
        default:
            break;
    }

    v3 ^= b;
    SIPROUND();
    SIPROUND();
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND();
    SIPROUND();
    SIPROUND();
    SIPROUND();
    return v0 ^ v1 ^ v2 ^ v3;
}
#undef SIPROUND
#endif

static wc_hash_t wc_hash_bytes(const char *s, size_t n, wc_hash_t seed_basis)
{
#if WC_INTERNAL_BOOL(WC_INTERNAL_TEST_FORCE_HASH)
    (void)s;
    (void)n;
    (void)seed_basis;
    return (wc_hash_t)0xdeadbeefu;
#elif WC_INTERNAL_BOOL(WC_HASH_STRONG)
    wc_u64 k0 = (wc_u64)0x9ae16a3b2f90404fULL ^ (wc_u64)seed_basis;
    wc_u64 k1 = (wc_u64)0xc3a5c85c97cb3127ULL ^ ((wc_u64)seed_basis << 1);
    wc_u64 h = siphash24((const unsigned char *)s, n, k0, k1);
    return (wc_hash_t)h;
#else
    return fnv32(s, n, seed_basis);
#endif
}

static int tab_grow(wc *w)
{
    size_t nc;
    size_t i;
    size_t idx;
    size_t alloc;
    Slot *ns = NULL;
    size_t old_alloc;
    Slot *old_tab;
    /* Manual allocation path avoids transient budget spikes; accounted post-rehash. */
#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
    int manual_alloc = 0;
    size_t post_other = 0;
#endif

    WC_ASSERT(w != NULL);
    WC_ASSERT(w->tab != NULL);
    WC_ASSERT(w->cap > 0);

    if (!wc_cap_valid(w->cap))
        return -2;
    if (w->cap > WC_CAP_LIMIT / 2)
        return -1;

    if (mul_overflows(w->cap, 2))
        return -1;
    nc = w->cap * 2;
    if (!wc_cap_valid(nc))
        return -1;

    if (!wc_mul_object_span(nc, sizeof(Slot), &alloc))
        return -1;

    /* NOTE: open-addressing depends on (cap-1) masking. Changing cap requires
       a full rehash into a new table. Do NOT "grow in place" unless you
       rehash (otherwise lookups break). */

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
    /*
    ** Dynamic mode + bytes_limit: allow growth if the *post-grow* accounting
    ** would fit, even if the rehash temporarily peaks.
    **
    ** We allocate the new table via WC_MALLOC (not the tracked allocator)
    ** and then set bytes_used to the post-rehash steady-state total.
    */
    if (!w->strict_max_bytes && !w->alloc.static_mode && w->alloc.bytes_limit) {
        const size_t cur_alloc = w->cap * sizeof(Slot);

#if !WC_INTERNAL_BOOL(WC_OMIT_ASSERT)
        WC_ASSERT(w->alloc.bytes_used >= cur_alloc);
#endif
        if (w->alloc.bytes_used < cur_alloc)
            return -2;

        post_other = w->alloc.bytes_used - cur_alloc;

        if (!add_overflows(post_other, alloc) &&
            post_other + alloc <= w->alloc.bytes_limit) {
            ns = (Slot *)WC_MALLOC(alloc);
            if (!ns)
                return -1;
            wc_slots_init(ns, nc);
            manual_alloc = 1;
        }
    }
#endif /* !WC_NO_HEAP */

    if (!ns) {
        ns = (Slot *)wc_xmalloc(w, alloc);
        if (!ns)
            return -1;
        wc_slots_init(ns, nc);
    }

    for (i = 0; i < w->cap; i++) {
        const Slot *s = &w->tab[i];
        if (!s->word)
            continue;

        idx = ((size_t)s->hash) & (nc - 1);
        {
            size_t probes = 0;
            while (ns[idx].word) {
                if (++probes >= nc) {
#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
                    if (manual_alloc)
                        WC_FREE(ns);
                    else
                        wc_xfree(w, ns, alloc);
#else
                    wc_xfree(w, ns, alloc);
#endif
                    return -2;
                }
                idx = (idx + 1) & (nc - 1);
            }
        }

        ns[idx] = *s;
    }

    old_tab = w->tab;
    old_alloc = w->cap * sizeof(Slot);

    w->tab = ns;
    w->cap = nc;

    wc_xfree(w, old_tab, old_alloc);

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (manual_alloc) {
        w->alloc.bytes_used = post_other + alloc;
    }
#endif

#if !WC_INTERNAL_BOOL(WC_OMIT_ASSERT)
    {
        size_t seen = 0;
        for (size_t k = 0; k < w->cap; k++) {
            if (w->tab[k].word)
                seen++;
        }
        WC_ASSERT(seen == w->len);
    }
#endif
    return 0;
}

static Slot *tab_find(wc *w, const char *word, size_t n, wc_hash_t h)
{
    size_t idx;
    size_t start;

    WC_ASSERT(w != NULL);
    WC_ASSERT(w->tab != NULL);
    WC_ASSERT(w->cap > 0);
    WC_ASSERT(word != NULL || n == 0);

    idx = ((size_t)h) & (w->cap - 1);
    start = idx;

    do {
        Slot *s = &w->tab[idx];

        if (!s->word)
            return s;

        /* Collision-safe and OOB-proof: compare lengths first. */
        if (s->hash == h && s->n == n &&
            wc_memcmp_internal(s->word, word, n) == 0) {
            return s;
        }

        idx = (idx + 1) & (w->cap - 1);
    } while (idx != start);

    return NULL; /* Full table (corruption or pathological static config). */
}

static int tab_insert(wc *w, const char *word, size_t n, wc_hash_t h)
{
    char *copy;
    size_t alloc;

    WC_ASSERT(w != NULL);
    WC_ASSERT(word != NULL);
    WC_ASSERT(n > 0);

    for (;;) {
        Slot *s = tab_find(w, word, n, h);

        /*
        ** tab_find() returns NULL only when it wraps the table without
        ** finding a match or empty slot.
        ** With no deletions, a wrap means full table unless corrupted (len < cap).
        */
        if (!s) {
            if (w->len < w->cap)
                return -2; /* corrupted/inconsistent state */
            if (w->alloc.static_mode)
                return -1; /* capacity exhausted in static-buffer mode */
            {
                int grc = tab_grow(w);
                if (grc < 0)
                    return grc; /* couldn't grow (OOM / budget/corrupt) */
            }
            continue; /* retry after grow */
        }

        /*
        ** Existing key: increment without allocating. This preserves the
        ** "duplicates succeed after exhaustion" guarantee.
        */
        if (s->word) {
            if (s->cnt == WC_SIZE_MAX || w->tot == WC_SIZE_MAX)
                return -1;
            s->cnt++;
            w->tot++;
            return 0;
        }

        /*
        ** New key: enforce load-factor policy for new unique insertions only.
        ** Keep semantics matching the documented check: (len * 10 >= cap * 7).
        */
        if (tab_load_factor_hit(w->len, w->cap)) {
            if (w->alloc.static_mode)
                return -1;
            {
                int grc = tab_grow(w);
                if (grc < 0)
                    return grc;
            }
            continue; /* table moved; re-probe on grown table */
        }

        /* Allocate and insert. */
        if (!wc_add_object_span(n, 1u, &alloc))
            return -1;

        if (w->len == WC_SIZE_MAX || w->tot == WC_SIZE_MAX)
            return -1;

        copy = (char *)arena_alloc(w, alloc);
        if (!copy)
            return -1;

        wc_memcpy_internal(copy, word, n);
        /* copy[n] is already NUL due to arena_alloc() zero-init. */

        s->word = copy;
        s->n = n;
        s->hash = h;
        s->cnt = 1;

        w->len++;
        w->tot++;
        return 0;
    }
}

/* --- Parameter tuning -------------------------------------------------- */

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static void
tune_params(const wc_limits *lim, size_t *init_cap, size_t *block_sz)
{
    size_t cap = WC_DEFAULT_INIT_CAP;
    size_t blk = WC_DEFAULT_BLOCK_SZ;

    if (lim) {
        size_t budget = 0;
        int explicit_init_cap = 0;
        int explicit_block_size = 0;

        if (lim->init_cap) {
            cap = lim->init_cap;
            explicit_init_cap = 1;
        }
        if (lim->block_size) {
            blk = lim->block_size;
            explicit_block_size = 1;
        }

        /*
        ** Derive an overall memory budget if one is available.
        ** Prefer the smaller of max_bytes and static_size when both
        ** are provided, since both constrain internal heap usage.
        */
        if (lim->max_bytes)
            budget = lim->max_bytes;
        if (lim->static_buf && lim->static_size) {
            if (budget == 0 || lim->static_size < budget)
                budget = lim->static_size;
        }

        if (budget) {
            const size_t b = budget;
            const size_t table_budget = b / 2;

            if (!explicit_init_cap && !mul_overflows(cap, sizeof(Slot)) &&
                cap * sizeof(Slot) > table_budget && table_budget > 0) {
                size_t max_cap = table_budget / sizeof(Slot);

                if (max_cap < WC_MIN_INIT_CAP)
                    max_cap = WC_MIN_INIT_CAP;

                cap = wc_pow2_round_down(max_cap);
            }
            /*
            ** Use up to one quarter of the arena budget for the
            ** first block. For very small budgets this will pull
            ** blk down; the final floor is applied below via
            ** WC_MIN_BLOCK_SZ.
            */
            /* Cap initial block size heuristically. */
            {
                const size_t arena_budget = b - table_budget;
                const size_t max_blk = arena_budget / 4;
                if (!explicit_block_size && max_blk > 0 && blk > max_blk)
                    blk = max_blk;
            }
        }
    }

    if (cap < WC_MIN_INIT_CAP)
        cap = WC_MIN_INIT_CAP;

    /* Round up to power of two. */
    cap = wc_pow2_round_up(cap);

    if (blk < WC_MIN_BLOCK_SZ)
        blk = WC_MIN_BLOCK_SZ;

    *init_cap = cap;
    *block_sz = blk;
}

/* --- Public API -------------------------------------------------------- */

wc *wc_open_ex(size_t max_word, const wc_limits *limits, int *err_out)
{
    wc *w = NULL;
    wc_limits lim_local;
    int have_limits = 0;
    size_t init_cap = 0;
    size_t block_sz = 0;
    size_t table_bytes = 0;
    size_t runtime_max_word = 0;
    size_t min_block_sz = 0;
    size_t bytes_limit = 0;
    int static_mode = 0;
#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
    size_t scan_bytes = 0;
#endif

    /* Extracted limits (defaults = 0/NULL) */
    size_t lim_max_bytes = 0;
    int lim_strict_max_bytes = 0;
    size_t lim_init_cap = 0;
    size_t lim_block_size = 0;
    void *lim_static_buf = NULL;
    size_t lim_static_size = 0;
    unsigned long lim_hash_seed = 0;
    size_t static_reserve = 0;
    int owns_self = 1;

    int err = WC_OK;

    wc_limits_init(&lim_local);

#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    /* In no-heap builds, limits must be provided and must include static storage. */
    if (!limits) {
        if (err_out)
            *err_out = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        return NULL;
    }
#endif

#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (wc_load_limits(&lim_local, limits) != WC_OK) {
        err = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        goto fail;
    }
    have_limits = 1;
#else
    if (limits) {
        if (wc_load_limits(&lim_local, limits) != WC_OK) {
            err = WC_EBADLIMITS;
            WC_SET_ERRNO(EINVAL);
            goto fail;
        }
        have_limits = 1;
#endif
    lim_max_bytes = lim_local.max_bytes;
    lim_init_cap = lim_local.init_cap;
    lim_block_size = lim_local.block_size;
    lim_static_buf = lim_local.static_buf;
    lim_static_size = lim_local.static_size;
    lim_hash_seed = lim_local.hash_seed;
    lim_strict_max_bytes = lim_local.strict_max_bytes;

    /* Enforce consistency: static mode requires BOTH buf and size. */
    if ((lim_static_buf && lim_static_size == 0) ||
        (!lim_static_buf && lim_static_size != 0)) {
        err = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        goto fail;
    }
    if (lim_static_buf && lim_static_size) {
        if (!wc_object_span_valid(lim_static_size)) {
            err = WC_EBADLIMITS;
            WC_SET_ERRNO(EINVAL);
            goto fail;
        }
#if !(WC_INTERNAL_BOOL(WC_HAVE_UINTPTR) &&              \
      WC_INTERNAL_BOOL(WC_LINEAR_UINTPTR_ALIGNMENT)) && \
        !WC_INTERNAL_BOOL(WC_TRUST_STATIC_BUFFER_ALIGNMENT)
        err = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        goto fail;
#else
            if (!wc_ptr_aligned(lim_static_buf, WC_ALIGN)) {
                err = WC_EALIGN;
                WC_SET_ERRNO(EINVAL);
                goto fail;
            }
#endif
    }

#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (!lim_static_buf || lim_static_size == 0) {
        err = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        goto fail;
    }
#else
    }
#endif

    {
        wc_limits lim_tune;
        wc_limits_init(&lim_tune);
        lim_tune.max_bytes = lim_max_bytes;
        lim_tune.init_cap = lim_init_cap;
        lim_tune.block_size = lim_block_size;
        lim_tune.static_buf = lim_static_buf;
        lim_tune.static_size = lim_static_size;
        tune_params(have_limits ? &lim_tune : NULL, &init_cap, &block_sz);
    }

    if (!wc_cap_valid(init_cap)) {
        err = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        goto fail;
    }

    static_mode = (lim_static_buf && lim_static_size > 0) ? 1 : 0;

#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (static_mode) {
        if (!wc_align_size(sizeof(struct wc), WC_ALIGN, &static_reserve) ||
            static_reserve > lim_static_size) {
            err = WC_EBADLIMITS;
            WC_SET_ERRNO(EINVAL);
            goto fail;
        }
    }
#endif

    /*
    ** Set overall memory budget (0 = unlimited). In static mode,
    ** max_bytes, if non-zero, is clamped to static_size and acts
    ** as an additional guard; otherwise the static buffer alone
    ** bounds internal allocations.
    */
    if (lim_max_bytes) {
        bytes_limit = lim_max_bytes;
        if (static_mode && bytes_limit > lim_static_size)
            bytes_limit = lim_static_size;
    }
    if (bytes_limit && static_reserve > bytes_limit) {
        err = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        goto fail;
    }

    /* Clamp max_word into [MIN_WORD, WC_MAX_WORD]. */
    runtime_max_word = max_word;
    if (runtime_max_word == 0)
        runtime_max_word = DEF_WORD;
    if (runtime_max_word < MIN_WORD)
        runtime_max_word = MIN_WORD;
    if (runtime_max_word > WC_MAX_WORD)
        runtime_max_word = WC_MAX_WORD;

    /* Ensure first arena block can store at least one max_word word (+NUL). */
    if (!wc_min_word_block_size(runtime_max_word, &min_block_sz)) {
        err = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        goto fail;
    }
    if (block_sz < min_block_sz)
        block_sz = min_block_sz;

    /*
    ** Deterministic validation of "impossible budgets":
    ** - In static mode: dry-run exact allocator accounting (no writes).
    ** - In dynamic mode: if max_bytes is set, ensure it can cover minimal
    **   internal structures (table + first arena block + optional scanbuf).
    */
    if (!wc_mul_object_span(init_cap, sizeof(Slot), &table_bytes)) {
        err = WC_EBADLIMITS;
        WC_SET_ERRNO(EINVAL);
        goto fail;
    }

    {
        size_t arena_bytes = 0;
        size_t layout_scan_bytes = 0;
        if (!wc_add_object_span(sizeof(Block), block_sz, &arena_bytes)) {
            err = WC_EBADLIMITS;
            WC_SET_ERRNO(EINVAL);
            goto fail;
        }
#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
        scan_bytes = runtime_max_word;
        if (!wc_object_span_valid(scan_bytes)) {
            err = WC_EBADLIMITS;
            WC_SET_ERRNO(EINVAL);
            goto fail;
        }
        layout_scan_bytes = scan_bytes;
#endif

        if (static_mode) {
            wc_alloc_state scratch;
            if (!wc_static_choose_layout(lim_static_size,
                                         static_reserve,
                                         bytes_limit,
                                         min_block_sz,
                                         layout_scan_bytes,
                                         lim_init_cap != 0,
                                         lim_block_size != 0,
                                         &init_cap,
                                         &block_sz,
                                         &table_bytes)) {
                err = WC_EBADLIMITS;
                WC_SET_ERRNO(EINVAL);
                goto fail;
            }
            if (!wc_add_object_span(sizeof(Block), block_sz, &arena_bytes)) {
                err = WC_EBADLIMITS;
                WC_SET_ERRNO(EINVAL);
                goto fail;
            }
            wc_alloc_state_init(&scratch);
            scratch.bytes_used = static_reserve;
            scratch.bytes_limit = bytes_limit;
            scratch.static_mode = 1;
            scratch.sbuf = (unsigned char *)lim_static_buf;
            scratch.sbuf_size = lim_static_size;
            scratch.sbuf_used = static_reserve;
            if (!wc_xdryrun_state(&scratch, table_bytes) ||
                !wc_xdryrun_state(&scratch, arena_bytes) ||
#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
                !wc_xdryrun_state(&scratch, scan_bytes) ||
#endif
                0) {
                err = WC_EBADLIMITS;
                WC_SET_ERRNO(EINVAL);
                goto fail;
            }
        } else if (bytes_limit) {
            size_t need;

            if (!wc_dynamic_choose_layout(bytes_limit,
                                          min_block_sz,
                                          layout_scan_bytes,
                                          lim_init_cap != 0,
                                          lim_block_size != 0,
                                          &init_cap,
                                          &block_sz,
                                          &table_bytes)) {
                err = WC_EBADLIMITS;
                WC_SET_ERRNO(EINVAL);
                goto fail;
            }
            if (!wc_add_object_span(sizeof(Block), block_sz, &arena_bytes)) {
                err = WC_EBADLIMITS;
                WC_SET_ERRNO(EINVAL);
                goto fail;
            }

            need = table_bytes;
            if (add_overflows(need, arena_bytes))
                goto badlimits;
            need += arena_bytes;

#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
            if (add_overflows(need, scan_bytes))
                goto badlimits;
            need += scan_bytes;
#endif

            if (need > bytes_limit) {
badlimits:
                err = WC_EBADLIMITS;
                WC_SET_ERRNO(EINVAL);
                goto fail;
            }
        }
    }

#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    w = (wc *)lim_static_buf;
    owns_self = 0;
#else
    w = (wc *)WC_MALLOC(sizeof *w);
    if (!w) {
        err = WC_NOMEM;
        WC_SET_ERRNO(ENOMEM);
        goto fail;
    }
#endif

    wc_handle_init(w);
    w->owns_self = owns_self;
    w->maxw = runtime_max_word;
    w->strict_max_bytes = lim_strict_max_bytes ? 1 : 0;
    w->alloc.bytes_limit = bytes_limit;

    if (static_mode) {
        w->alloc.static_mode = 1;
        w->alloc.sbuf = (unsigned char *)lim_static_buf;
        w->alloc.sbuf_size = lim_static_size;
        w->alloc.sbuf_used = static_reserve;
        w->alloc.bytes_used = static_reserve;
    }

    /* Seed material: fold hash_seed into the 32-bit stored seed. */
    {
        wc_hash_t basis = FNV_OFF_32;
        if (lim_hash_seed) {
            unsigned long hs = lim_hash_seed;
#if ULONG_MAX > 0xffffffffUL
            hs ^= (hs >> 32);
#endif
            basis ^= (wc_hash_t)hs;
        }
        w->seed = basis;
    }

    /* Allocate initial hash table. */
    w->tab = (Slot *)wc_xmalloc(w, table_bytes);
    if (!w->tab) {
        err = WC_NOMEM;
        WC_SET_ERRNO(ENOMEM);
        goto fail;
    }

    wc_slots_init(w->tab, init_cap);
    w->cap = init_cap;

    /* Initialize arena. */
    if (arena_init(w, &w->arena, block_sz) < 0) {
        wc_xfree(w, w->tab, table_bytes);
        w->tab = NULL;
        w->cap = 0;
        err = WC_NOMEM;
        WC_SET_ERRNO(ENOMEM);
        goto fail;
    }

#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
    /* scanbuf holds raw bytes for length-based adds; no implicit NUL required. */
    w->scanbuf = (char *)wc_xmalloc(w, scan_bytes);
    if (!w->scanbuf) {
        arena_free(w);
        if (w->tab && w->cap) {
            wc_xfree(w, w->tab, table_bytes);
            w->tab = NULL;
            w->cap = 0;
        }
        err = WC_NOMEM;
        WC_SET_ERRNO(ENOMEM);
        goto fail;
    }
    w->scanbuf_size = scan_bytes;
#endif

    if (err_out)
        *err_out = err;
    return w;

fail:
    if (err_out)
        *err_out = err;
    if (w) {
        /* Best-effort cleanup if partially initialized */
#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
        if (w->scanbuf)
            wc_xfree(w, w->scanbuf, w->scanbuf_size);
#endif
        if (w->tab && w->cap) {
            size_t tb = w->cap * sizeof(Slot);
            wc_xfree(w, w->tab, tb);
        }
        arena_free(w);
#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
        if (w->owns_self)
            WC_FREE(w);
#endif
    }
    return NULL;
}

wc *wc_open(size_t max_word)
{
#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    (void)max_word;
    WC_SET_ERRNO(ENOMEM);
    return NULL;
#else
    return wc_open_ex(max_word, NULL, NULL);
#endif
}

void wc_close(wc *w)
{
    size_t table_bytes;

    if (!w)
        return;

    wc_stream_detach_all(w);

#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
    if (w->scanbuf)
        wc_xfree(w, w->scanbuf, w->scanbuf_size);
#endif

    if (w->tab && w->cap) {
        table_bytes = w->cap * sizeof(Slot);
        wc_xfree(w, w->tab, table_bytes);
    }

    arena_free(w);

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (w->owns_self)
        WC_FREE(w);
#endif
}

/* --- Word insertion and scanning -------------------------------------- */

static int wc_insert_prepared(wc *w, const char *word, size_t n)
{
    wc_hash_t h = wc_hash_bytes(word, n, w->seed);
    const int trc = tab_insert(w, word, n, h);

    if (trc == 0)
        return WC_OK;
    if (trc == -2)
        return WC_ERROR;
    return WC_NOMEM;
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static int
wc_add_impl(wc *w,
            const char *word,
            size_t len,
            int normalize) /* NOLINT(bugprone-easily-swappable-parameters) */
{
    size_t n;

    if (!wc_ready(w) || (!word && len > 0))
        return WC_ERROR;
    if (len == 0)
        return WC_OK;

    if (len > w->maxw)
        len = w->maxw;
    n = len;

    /*
    ** Length-based APIs treat embedded NUL as a terminator.
    ** This keeps wc_word->word a well-formed C string and makes strcmp/qsort safe.
    */
    {
        const char *nul = (const char *)wc_memchr_internal(word, '\0', n);
        if (nul) {
            n = (size_t)(nul - word);
            if (n == 0)
                return WC_OK;
        }
    }
    if (n == 0)
        return WC_OK;

    if (normalize) {
        size_t i;
#if WC_INTERNAL_BOOL(WC_STACK_BUFFER)
        char local[WC_MAX_WORD];
        char *norm_buf = local;
#else
        char *norm_buf = w->scanbuf;
        WC_ASSERT(norm_buf != NULL);
#endif

        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)word[i];
            if (wc_is_word_byte(c))
                c = wc_tolower_byte(c);
            norm_buf[i] = (char)c;
        }
        return wc_insert_prepared(w, norm_buf, n);
    }

    return wc_insert_prepared(w, word, n);
}

int wc_add(wc *w, const char *word)
{
    size_t n;

    if (!wc_ready(w) || !word)
        return WC_ERROR;

    for (n = 0; n < w->maxw && word[n]; n++)
        ;
    return wc_add_impl(w, word, n, 0);
}

int wc_add_n(wc *w, const char *word, size_t len)
{
    return wc_add_impl(w, word, len, 0);
}

int wc_add_norm_n(wc *w, const char *word, size_t len)
{
    return wc_add_impl(w, word, len, 1);
}

static int wc_is_word_byte(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static unsigned char wc_tolower_byte(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c + ('a' - 'A'));
    return c;
}

int wc_scan(wc *w, const char *text, size_t len)
{
    const unsigned char *p;
    const unsigned char *end;

#if WC_INTERNAL_BOOL(WC_STACK_BUFFER)
    char buf[WC_MAX_WORD];
#else
    char *buf;
#endif

    if (!wc_ready(w))
        return WC_ERROR;
    if (len == 0)
        return WC_OK;
    if (!text)
        return WC_ERROR;
    if (len > (size_t)WC_PTRDIFF_MAX)
        return WC_ERROR;

#if !WC_INTERNAL_BOOL(WC_STACK_BUFFER)
    buf = w->scanbuf;
    WC_ASSERT(buf != NULL);
#endif

    p = (const unsigned char *)text;
    end = p + len;

    while (p < end) {
        size_t n = 0;

        while (p < end && !wc_is_word_byte(*p))
            p++;
        if (p >= end)
            break;

        while (p < end && wc_is_word_byte(*p)) {
            unsigned char c = wc_tolower_byte(*p++);
            if (n < w->maxw)
                buf[n++] = (char)c;
        }

        WC_ASSERT(n > 0);
        WC_ASSERT(n <= w->maxw);

        {
            wc_hash_t h = wc_hash_bytes(buf, n, w->seed);
            const int trc = tab_insert(w, buf, n, h);
            if (trc == -2)
                return WC_ERROR;
            if (trc != 0)
                return WC_NOMEM;
        }
    }

    return WC_OK;
}

/* --- Queries ---------------------------------------------------------- */

size_t wc_total(const wc *w)
{
    return wc_ready(w) ? w->tot : 0;
}

size_t wc_unique(const wc *w)
{
    return wc_ready(w) ? w->len : 0;
}

int wc_get_stats(const wc *w, wc_stats *out)
{
    if (!out || !wc_ready(w))
        return WC_ERROR;

    out->bytes_used = w->alloc.bytes_used;
    out->bytes_limit = w->alloc.bytes_limit;
    out->static_mode = w->alloc.static_mode;
    out->cap = w->cap;
    out->arena_blocks = w->arena.blocks;
    return WC_OK;
}

/* --- Results enumeration ---------------------------------------------- */

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
#if WC_INTERNAL_BOOL(WC_USE_LIBC_QSORT)
/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
static int wc_qsort_cmp_words(
        const void *a,
        const void *b) /* NOLINT(bugprone-easily-swappable-parameters) */
{
    const wc_word *x = (const wc_word *)a;
    const wc_word *y = (const wc_word *)b;

    if (x->count > y->count)
        return -1;
    if (x->count < y->count)
        return 1;
    return wc_strcmp_internal(x->word, y->word);
}
#endif

/* For min-heap: returns 1 if a is strictly worse (smaller count or same count
   but bytewise larger). */
static int wc_word_is_worse(const wc_word *a, const wc_word *b)
{
    if (a->count != b->count)
        return a->count < b->count;
    return wc_strcmp_internal(a->word, b->word) > 0;
}

/* Avoid 2*i+1 overflow by only computing children when i <= (n-2)/2 */
static void wc_sift_down_words(wc_word *arr, size_t n, size_t i)
{
    for (;;) {
        if (n < 2)
            return;
        if (i > (n - 2u) / 2u)
            return; /* leaf */

        size_t left = i * 2u + 1u;
        size_t right = left + 1u;
        size_t worst = i;

        if (left < n && wc_word_is_worse(&arr[left], &arr[worst]))
            worst = left;
        if (right < n && wc_word_is_worse(&arr[right], &arr[worst]))
            worst = right;

        if (worst == i)
            return;

        wc_word tmp = arr[i];
        arr[i] = arr[worst];
        arr[worst] = tmp;
        i = worst;
    }
}

#if !WC_INTERNAL_BOOL(WC_USE_LIBC_QSORT)
static void wc_heapsort_words(wc_word *arr, size_t n)
{
    if (!arr || n < 2)
        return;

    for (size_t i = n / 2u; i > 0; i--)
        wc_sift_down_words(arr, n, i - 1u);

    for (size_t end = n; end > 1; end--) {
        wc_word tmp = arr[0];
        arr[0] = arr[end - 1u];
        arr[end - 1u] = tmp;
        wc_sift_down_words(arr, end - 1u, 0);
    }
}
#endif

static void wc_sort_words(wc_word *arr, size_t n)
{
#if WC_INTERNAL_BOOL(WC_USE_LIBC_QSORT)
    qsort(arr, n, sizeof(*arr), wc_qsort_cmp_words);
#else
    wc_heapsort_words(arr, n);
#endif
}

static void wc_heap_sift_up(wc_word *heap, size_t idx)
{
    while (idx > 0) {
        size_t parent = (idx - 1u) / 2u;
        if (!wc_word_is_worse(&heap[idx], &heap[parent]))
            break;
        {
            wc_word tmp = heap[parent];
            heap[parent] = heap[idx];
            heap[idx] = tmp;
        }
        idx = parent;
    }
}

static int
wc_results_fill(const wc *w, wc_word *out, size_t out_cap, size_t *out_n)
{
    size_t cnt = 0;
    size_t filled = 0;

    if (!out_n)
        return WC_ERROR;

    *out_n = 0;

    if (!wc_ready(w))
        return WC_ERROR;

    if (w->len == 0)
        return WC_OK;

    if (!out || out_cap == 0) {
        *out_n = w->len;
        return WC_OK;
    }
    if (out_cap < w->len) {
        *out_n = w->len;
        return WC_NOMEM;
    }

    /* Two-pass: verify table occupancy matches w->len to detect corruption. */
    for (size_t i = 0; i < w->cap; i++) {
        if (w->tab[i].word)
            cnt++;
    }

    if (cnt != w->len)
        return WC_ERROR;

    for (size_t i = 0; i < w->cap; i++) {
        if (w->tab[i].word) {
            out[filled].word = w->tab[i].word;
            out[filled].count = w->tab[i].cnt;
            filled++;
        }
    }

    if (filled != w->len)
        return WC_ERROR;

    wc_sort_words(out, w->len);
    *out_n = w->len;

    return WC_OK;
}
#endif

int wc_results(const wc *w, wc_word **out, size_t *n)
{
#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (!out || !n)
        return WC_ERROR;
    *out = NULL;
    *n = 0;
    if (!wc_ready(w))
        return WC_ERROR;
    return WC_NOMEM;
#else
    wc_word *arr;
    size_t alloc;
    size_t filled = 0;
    int rc;

    if (!out || !n)
        return WC_ERROR;
    *out = NULL;
    *n = 0;

    if (!wc_ready(w))
        return WC_ERROR;
    if (w->len == 0)
        return WC_OK;

    if (!wc_mul_object_span(w->len, sizeof *arr, &alloc))
        return WC_NOMEM;
    arr = (wc_word *)WC_MALLOC(alloc);
    if (!arr)
        return WC_NOMEM;

    rc = wc_results_fill(w, arr, w->len, &filled);
    if (rc != WC_OK) {
        WC_FREE(arr);
        if (rc == WC_NOMEM)
            *n = filled;
        return rc;
    }

    *out = arr;
    *n = filled;
    return WC_OK;
#endif
}

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
static int
wc_topn_fill(const wc *w, size_t n, wc_word *out, size_t out_cap, size_t *out_n)
{
    size_t target;
    size_t heap_sz = 0;
    size_t count = 0;

    if (!out_n)
        return WC_ERROR;
    *out_n = 0;

    if (!wc_ready(w))
        return WC_ERROR;
    if (n == 0 || w->len == 0)
        return WC_OK;

    target = n < w->len ? n : w->len;

    if (!out || out_cap == 0) {
        *out_n = target;
        return WC_OK;
    }
    if (out_cap < target) {
        *out_n = target;
        return WC_NOMEM;
    }

    for (size_t i = 0; i < w->cap; i++) {
        if (w->tab[i].word)
            count++;
    }
    if (count != w->len)
        return WC_ERROR;

    for (size_t i = 0; i < w->cap; i++) {
        const Slot *s = &w->tab[i];
        wc_word cand;
        if (!s->word)
            continue;
        cand.word = s->word;
        cand.count = s->cnt;

        if (heap_sz < target) {
            out[heap_sz++] = cand;
            wc_heap_sift_up(out, heap_sz - 1u);
        } else if (wc_word_is_worse(&out[0], &cand)) {
            out[0] = cand;
            wc_sift_down_words(out, heap_sz, 0);
        }
    }
    WC_ASSERT(heap_sz <= target);
    /* Sort for deterministic output (count desc, bytewise word asc). */
    wc_sort_words(out, heap_sz);
    *out_n = heap_sz;

    return WC_OK;
}
#endif

int wc_topn(const wc *w, size_t n, wc_word **out, size_t *out_n)
{
#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    (void)n;
    if (!out || !out_n)
        return WC_ERROR;
    *out = NULL;
    *out_n = 0;
    if (!wc_ready(w))
        return WC_ERROR;
    return WC_NOMEM;
#else
    wc_word *heap;
    size_t alloc;
    size_t target;
    size_t filled = 0;
    int rc;

    if (!out || !out_n)
        return WC_ERROR;
    *out = NULL;
    *out_n = 0;

    if (!wc_ready(w))
        return WC_ERROR;
    if (n == 0 || w->len == 0)
        return WC_OK;

    target = n < w->len ? n : w->len;

    if (!wc_mul_object_span(target, sizeof *heap, &alloc))
        return WC_NOMEM;
    heap = (wc_word *)WC_MALLOC(alloc);
    if (!heap)
        return WC_NOMEM;

    rc = wc_topn_fill(w, n, heap, target, &filled);
    if (rc != WC_OK) {
        WC_FREE(heap);
        if (rc == WC_NOMEM)
            *out_n = filled;
        return rc;
    }

    *out = heap;
    *out_n = filled;
    return WC_OK;
#endif
}

void wc_results_free(wc_word *r)
{
#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (r)
        WC_FREE(r);
#else
    (void)r;
#endif
}

/* NOLINTNEXTLINE(bugprone-easily-swappable-parameters) */
int wc_reserve(
        wc *w,
        size_t expected_unique,
        size_t expected_bytes) /* NOLINT(bugprone-easily-swappable-parameters) */
{
    if (!wc_ready(w))
        return WC_ERROR;

    if (expected_unique > 0) {
        size_t target_cap = w->cap;
        while (tab_load_factor_hit(expected_unique - 1u, target_cap)) {
            if (target_cap > WC_CAP_LIMIT / 2)
                return WC_NOMEM;
            if (mul_overflows(target_cap, 2))
                return WC_NOMEM;
            target_cap *= 2;
        }

        if (!wc_cap_valid(target_cap))
            return WC_ERROR;

        while (w->cap < target_cap) {
            if (w->alloc.static_mode)
                return WC_NOMEM;
            {
                int grc = tab_grow(w);
                if (grc == -2)
                    return WC_ERROR;
                if (grc < 0)
                    return WC_NOMEM;
            }
        }
    }

    if (expected_bytes > 0) {
        size_t tail_avail;

        if (!arena_tail_available(&w->arena, &tail_avail))
            return WC_ERROR;
        if (expected_bytes <= tail_avail)
            return WC_OK;

        if (w->alloc.static_mode) {
            return WC_NOMEM;
        } else {
            size_t alloc;

            if (!arena_new_block_allocation_size(
                        &w->arena, expected_bytes, &alloc))
                return WC_NOMEM;
            if (w->alloc.bytes_limit) {
                if (w->alloc.bytes_used > w->alloc.bytes_limit)
                    return WC_ERROR;
                if (alloc > w->alloc.bytes_limit - w->alloc.bytes_used)
                    return WC_NOMEM;
            }
        }
    }

    return WC_OK;
}

/* --- Cursor API -------------------------------------------------------- */

void wc_cursor_init(wc_cursor *c, const wc *w)
{
    if (c) {
        c->w = w;
        c->index = 0;
    }
}

int wc_cursor_next(wc_cursor *c, const char **word, size_t *count)
{
    if (!c || !wc_ready(c->w))
        return 0;
    /* Linear scan of the open-addressed hash table */
    while (c->index < c->w->cap) {
        const Slot *s = &c->w->tab[c->index++];
        if (s->word) {
            if (word)
                *word = s->word;
            if (count)
                *count = s->cnt;
            return 1;
        }
    }
    return 0;
}

#if WC_INTERNAL_BOOL(WC_ENABLE_VALIDATE)
static int wc_validate_probe_path(const wc *w, size_t slot_index)
{
    const Slot *target;
    size_t idx;
    size_t start;

    if (!w || !w->tab || slot_index >= w->cap)
        return 0;

    target = &w->tab[slot_index];
    if (!target->word)
        return 0;

    idx = ((size_t)target->hash) & (w->cap - 1u);
    start = idx;

    do {
        const Slot *s = &w->tab[idx];

        if (!s->word)
            return 0;
        if (s->hash == target->hash && s->n == target->n &&
            wc_memcmp_internal(s->word, target->word, target->n) == 0) {
            return idx == slot_index;
        }
        idx = (idx + 1u) & (w->cap - 1u);
    } while (idx != start);

    return 0;
}
#endif

int wc_validate(const wc *w)
{
    size_t seen = 0;
    size_t tot = 0;

    if (!wc_ready(w))
        return WC_ERROR;

#if !WC_INTERNAL_BOOL(WC_ENABLE_VALIDATE)
    (void)seen;
    (void)tot;
    return WC_OK;
#else
    if (!wc_cap_valid(w->cap))
        return WC_ERROR;
    if (w->len > w->cap)
        return WC_ERROR;

    for (size_t i = 0; i < w->cap; i++) {
        const Slot *s = &w->tab[i];
        if (!s->word)
            continue;
        seen++;
        if (s->n == 0 || s->n > w->maxw)
            return WC_ERROR;
        if (s->cnt == 0)
            return WC_ERROR;

        /* Ensure word is exactly n bytes with no embedded NUL and is terminated at [n]. */
        if (wc_memchr_internal(s->word, '\0', s->n) != NULL)
            return WC_ERROR;
        if (s->word[s->n] != '\0')
            return WC_ERROR;

        /* Hash must match stored key (corruption detector). */
        if (s->hash != wc_hash_bytes(s->word, s->n, w->seed)) {
            return WC_ERROR;
        }

        if (tot > WC_SIZE_MAX - s->cnt)
            return WC_ERROR;
        tot += s->cnt;
    }

    for (size_t i = 0; i < w->cap; i++) {
        if (w->tab[i].word && !wc_validate_probe_path(w, i))
            return WC_ERROR;
    }

    if (seen != w->len)
        return WC_ERROR;
    if (tot != w->tot)
        return WC_ERROR;
    return WC_OK;
#endif
}

/* --- Utility functions ------------------------------------------------- */

const char *wc_errstr(int rc)
{
    switch (rc) {
        case WC_OK:
            return "ok";
        case WC_ERROR:
            return "invalid argument";
        case WC_NOMEM:
            return "resource exhausted";
        case WC_EALIGN:
            return "misaligned buffer";
        case WC_EBADLIMITS:
            return "invalid or unsatisfiable limits";
        default:
            return "unknown error";
    }
}

const char *wc_version(void)
{
    return WC_VERSION;
}

/* --- Build configuration introspection -------------------------------- */

static const wc_build_config wc_build_cfg = {
    .struct_size = sizeof(wc_build_config),
    .version_number = WC_VERSION_NUMBER,
    .max_word = WC_MAX_WORD,
    .min_init_cap = WC_MIN_INIT_CAP,
    .min_block_sz = WC_MIN_BLOCK_SZ,
    .stack_buffer = WC_STACK_BUFFER ? 1 : 0,
    .hosted = WC_STDC_HOSTED ? 1 : 0,
    .use_libc_string = WC_USE_LIBC_STRING ? 1 : 0,
    .use_libc_qsort = WC_USE_LIBC_QSORT ? 1 : 0,
    .have_errno = WC_HAVE_ERRNO ? 1 : 0,
    .no_heap = WC_NO_HEAP ? 1 : 0,
    .hash_strong = WC_HASH_STRONG ? 1 : 0,
    .have_uintptr = WC_HAVE_UINTPTR ? 1 : 0,
    .trust_static_buffer_alignment = WC_TRUST_STATIC_BUFFER_ALIGNMENT ? 1 : 0,
    .linear_uintptr_alignment = WC_LINEAR_UINTPTR_ALIGNMENT ? 1 : 0,
    .stack_max_word = WC_STACK_MAX_WORD
};

const wc_build_config *wc_build_info(void)
{
    /* Reference internal alignment helpers so analyzers
       see wc_internal_align as "used". */
    wc_internal_align_sanity();
    return &wc_build_cfg;
}

/* --------------------------------------------------------------------- */
/* Streaming scan implementation                                         */
/* --------------------------------------------------------------------- */

#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
static int
wc_stream_requirements(const wc *w, size_t *cap_out, size_t *bytes_out)
{
    size_t cap;
    size_t buf_bytes;
    size_t total;

    if (!cap_out || !bytes_out || !wc_ready(w))
        return 0;

    cap = w->maxw;
    if (cap < 4)
        cap = 4;

    if (!wc_add_object_span(cap, 1, &buf_bytes))
        return 0;

    if (!wc_add_object_span(sizeof(struct wc_stream), buf_bytes, &total))
        return 0;

    *cap_out = cap;
    *bytes_out = total;
    return 1;
}
#endif

wc_stream *wc_stream_open(wc *w, int *err_out)
{
#if WC_INTERNAL_BOOL(WC_NO_HEAP)
    (void)w;
    if (err_out)
        *err_out = WC_NOMEM;
    WC_SET_ERRNO(ENOMEM);
    return NULL;
#else
    wc_stream *s;
    size_t cap = 0;
    size_t need = 0;

    if (err_out)
        *err_out = WC_OK;

    if (!wc_ready(w)) {
        if (err_out)
            *err_out = WC_ERROR;
        WC_SET_ERRNO(EINVAL);
        return NULL;
    }

    if (!wc_stream_requirements(w, &cap, &need)) {
        if (err_out)
            *err_out = WC_ERROR;
        WC_SET_ERRNO(EINVAL);
        return NULL;
    }
    s = (wc_stream *)WC_MALLOC(need);
    if (!s) {
        WC_SET_ERRNO(ENOMEM);
        if (err_out)
            *err_out = WC_NOMEM;
        return NULL;
    }

    s->w = NULL;
    s->prev = NULL;
    s->next = NULL;
    s->cap = cap;
    s->len = 0;
    s->finished = 0;
    s->finish_rc = WC_OK;
    s->owns_self = 1;
    s->buf = s->storage;
    wc_memset_internal(s->storage, 0, cap + 1u);
    s->buf[0] = '\0';
    wc_stream_link(w, s);
    return s;
#endif /* !WC_NO_HEAP */
}

static void wc_stream_clear_word(wc_stream *s)
{
    s->len = 0;
    s->buf[0] = '\0';
}

static int wc_stream_flush_word(wc_stream *s, int discard_on_nomem)
{
    int trc;
    if (!s || !wc_ready(s->w))
        return WC_ERROR;
    if (s->len == 0)
        return WC_OK;

    {
        wc_hash_t h = wc_hash_bytes(s->buf, s->len, s->w->seed);
        trc = tab_insert(s->w, s->buf, s->len, h);
    }

    if (trc == 0) {
        wc_stream_clear_word(s);
        return WC_OK;
    }
    if (trc == -2) {
        wc_stream_clear_word(s);
        return WC_ERROR;
    }
    if (discard_on_nomem)
        wc_stream_clear_word(s);
    return WC_NOMEM;
}

int wc_stream_scan_ex(wc_stream *s,
                      const char *buf,
                      size_t len,
                      size_t *consumed_out)
{
    const unsigned char *p;
    const unsigned char *end;
    const unsigned char *start;

    if (consumed_out)
        *consumed_out = 0;

    if (!s || !s->w)
        return WC_ERROR;
    if (s->finished) {
        return s->finish_rc;
    }
    if (len == 0) {
        if (consumed_out)
            *consumed_out = 0;
        return WC_OK;
    }
    if (!buf)
        return WC_ERROR;
    if (len > (size_t)WC_PTRDIFF_MAX)
        return WC_ERROR;

    start = (const unsigned char *)buf;
    p = start;
    end = p + len;

    while (p < end) {
        unsigned char c = *p;

        if (wc_is_word_byte(c)) {
            c = wc_tolower_byte(c);
            if (s->len < s->cap)
                s->buf[s->len++] = (char)c;
            p++;
        } else {
            if (s->len > 0) {
                int rc = wc_stream_flush_word(s, 1);
                if (rc != WC_OK) {
                    /* consume the separator for forward progress */
                    p++;
                    if (consumed_out)
                        *consumed_out = (size_t)(p - start);
                    return rc;
                }
            }
            p++;
        }
    }

    if (consumed_out)
        *consumed_out = (size_t)(p - start);
    return WC_OK;
}

int wc_stream_finish(wc_stream *s)
{
    int rc;

    if (!s || !wc_ready(s->w))
        return WC_ERROR;
    if (s->finished)
        return s->finish_rc;

    rc = wc_stream_flush_word(s, 0);
    if (rc != WC_NOMEM) {
        s->finished = 1;
        s->finish_rc = rc;
    }
    return rc;
}

void wc_stream_close(wc_stream *s)
{
    if (!s)
        return;
    wc_stream_unlink(s);
#if !WC_INTERNAL_BOOL(WC_NO_HEAP)
    if (s->owns_self)
        WC_FREE(s);
#else
    (void)s;
#endif
}

/* wordcount.h - Word frequency counter
**
** CONTRACT
**
**   Language: C99. Defaults to hosted libc; freestanding/exotic builds
**   are supported via WC_STDC_HOSTED, WC_USE_LIBC_STRING, WC_USE_LIBC_QSORT,
**   WC_HAVE_ERRNO, WC_NO_HEAP, and WC_TRUST_STATIC_BUFFER_ALIGNMENT.
**
**   Threading/reentrancy: No global mutable state. Separate wc instances
**   may be used concurrently. A single wc or wc_stream must not be used
**   from multiple threads without external synchronization.
**
**   Encoding/word definition: Assumes 8-bit chars and ASCII-compatible
**   execution character set. Default build (WC_ASCII_ONLY=1) treats a
**   "word" as a maximal run of ASCII letters [A-Za-z]; other bytes are
**   separators and case folding is ASCII-only. With WC_ASCII_ONLY=0,
**   classification uses ctype (isalnum + apostrophe) under the caller's
**   current locale; the library itself does not set locale. wc_add/wc_add_n
**   accept arbitrary byte sequences and do not fold case.
**
**   Maximum length: max_word is clamped into [4, WC_MAX_WORD]. Bytes
**   beyond max_word are truncated (still counted); truncation never
**   triggers an error.
**
**   Error model: int-returning APIs use WC_OK (0) for success and
**   WC_ERROR / WC_NOMEM / WC_EALIGN / WC_EBADLIMITS / WC_EBUSY as
**   documented per function. err_out (when provided) is written on
**   success and failure. errno is only touched on failures when
**   WC_HAVE_ERRNO != 0. wc_errstr() yields stable, human-readable strings.
**
**   Ownership/lifetime: wc_word.word pointers returned by wc_results*()
**   are owned by the wc instance and become invalid after wc_close().
**   Arrays from wc_results* are caller-owned and must be freed with
**   wc_results_free(). Streams are closed with wc_stream_close().
**
**   Determinism/sorting: Results are sorted by count descending then
**   bytewise word ascending; behavior is locale-independent in the
**   default ASCII build. With WC_ASCII_ONLY=0, tokenization follows the
**   caller's C locale.
**
**   Public API functions: wc_open_ex, wc_open, wc_close, wc_handle_size, wc_add,
**     wc_add_n, wc_add_norm_n, wc_scan, wc_total, wc_unique, wc_results,
**     wc_results_into, wc_results_free, wc_topn, wc_topn_into, wc_reserve,
**     wc_cursor_init, wc_cursor_next, wc_errstr, wc_version, wc_build_info,
**     wc_get_stats, wc_validate, wc_stream_open, wc_stream_size,
**     wc_stream_open_inplace, wc_stream_scan_ex, wc_stream_scan,
**     wc_stream_finish, wc_stream_close.
**
**   Public API helpers/macros: wc_limits_init, WC_LIMITS_INIT,
**     WC_BUILD_CONFIG_INIT, WC_STATIC_BUFFER.
**
** STABILITY
**
**   API is stable. Functions will not be removed or change signature.
**   New functions may be added in minor/patch releases.
*/
#ifndef WORDCOUNT_H
#define WORDCOUNT_H

#ifndef WC_STDC_HOSTED
#ifdef __STDC_HOSTED__
#define WC_STDC_HOSTED __STDC_HOSTED__
#else
#define WC_STDC_HOSTED 1
#endif
#endif

#ifndef WC_HASH_STRONG
#define WC_HASH_STRONG 0
#endif

#ifndef WC_USE_LIBC_STRING
#if WC_STDC_HOSTED
#define WC_USE_LIBC_STRING 1
#else
#define WC_USE_LIBC_STRING 0
#endif
#endif

#ifndef WC_USE_LIBC_QSORT
#if WC_STDC_HOSTED
#define WC_USE_LIBC_QSORT 1
#else
#define WC_USE_LIBC_QSORT 0
#endif
#endif

#ifndef WC_HAVE_ERRNO
#if WC_STDC_HOSTED
#define WC_HAVE_ERRNO 1
#else
#define WC_HAVE_ERRNO 0
#endif
#endif

#ifndef WC_ENABLE_VALIDATE
#define WC_ENABLE_VALIDATE 0
#endif

/*
** WC_NO_HEAP:
**   When 1, the library is built in "no-heap" mode:
**     - The compiled object does not reference WC_MALLOC/WC_FREE/WC_REALLOC.
**     - wc_open() is unavailable; wc_open_ex() requires caller-provided storage via limits.
**     - wc_results()/wc_topn()/wc_stream_open() are unavailable (return WC_NOMEM / NULL);
**       use *_into, wc_cursor, and wc_stream_open_inplace().
*/
#ifndef WC_NO_HEAP
#define WC_NO_HEAP 0
#endif

#ifndef WC_TRUST_STATIC_BUFFER_ALIGNMENT
#define WC_TRUST_STATIC_BUFFER_ALIGNMENT 0
#endif

#ifndef WC_STREAM_REUSE_SCANBUF
#define WC_STREAM_REUSE_SCANBUF 0
#endif

#ifndef WC_ASCII_ONLY
#define WC_ASCII_ONLY 1
#endif

#ifndef WC_BOOL
#define WC_BOOL(x) ((x) != 0)
#endif

/* Normalize feature toggles to numeric 0/1 even if defined without a value. */
#ifdef WC_HASH_STRONG
#if (WC_HASH_STRONG + 0)
#undef WC_HASH_STRONG
#define WC_HASH_STRONG 1
#else
#undef WC_HASH_STRONG
#define WC_HASH_STRONG 0
#endif
#else
#define WC_HASH_STRONG 0
#endif

#ifdef WC_USE_LIBC_STRING
#if (WC_USE_LIBC_STRING + 0)
#undef WC_USE_LIBC_STRING
#define WC_USE_LIBC_STRING 1
#else
#undef WC_USE_LIBC_STRING
#define WC_USE_LIBC_STRING 0
#endif
#else
#define WC_USE_LIBC_STRING 0
#endif

#ifdef WC_USE_LIBC_QSORT
#if (WC_USE_LIBC_QSORT + 0)
#undef WC_USE_LIBC_QSORT
#define WC_USE_LIBC_QSORT 1
#else
#undef WC_USE_LIBC_QSORT
#define WC_USE_LIBC_QSORT 0
#endif
#else
#define WC_USE_LIBC_QSORT 0
#endif

#ifdef WC_HAVE_ERRNO
#if (WC_HAVE_ERRNO + 0)
#undef WC_HAVE_ERRNO
#define WC_HAVE_ERRNO 1
#else
#undef WC_HAVE_ERRNO
#define WC_HAVE_ERRNO 0
#endif
#else
#define WC_HAVE_ERRNO 0
#endif

#ifdef WC_ENABLE_VALIDATE
#if (WC_ENABLE_VALIDATE + 0)
#undef WC_ENABLE_VALIDATE
#define WC_ENABLE_VALIDATE 1
#else
#undef WC_ENABLE_VALIDATE
#define WC_ENABLE_VALIDATE 0
#endif
#else
#define WC_ENABLE_VALIDATE 0
#endif

#ifdef WC_NO_HEAP
#if (WC_NO_HEAP + 0)
#undef WC_NO_HEAP
#define WC_NO_HEAP 1
#else
#undef WC_NO_HEAP
#define WC_NO_HEAP 0
#endif
#else
#define WC_NO_HEAP 0
#endif

/*
** WC_STREAM_REUSE_SCANBUF:
**   When 1, wc_stream reuses the per-instance scan buffer instead of
**   allocating its own storage. Reduces memory footprint but:
**     - Requires WC_STACK_BUFFER=0 (compile-time enforced).
**     - Only one stream may be active per wc instance; concurrent
**       wc_stream_open or wc_add/wc_scan calls return WC_EBUSY.
*/
#ifdef WC_TRUST_STATIC_BUFFER_ALIGNMENT
#if (WC_TRUST_STATIC_BUFFER_ALIGNMENT + 0)
#undef WC_TRUST_STATIC_BUFFER_ALIGNMENT
#define WC_TRUST_STATIC_BUFFER_ALIGNMENT 1
#else
#undef WC_TRUST_STATIC_BUFFER_ALIGNMENT
#define WC_TRUST_STATIC_BUFFER_ALIGNMENT 0
#endif
#else
#define WC_TRUST_STATIC_BUFFER_ALIGNMENT 0
#endif

#ifdef WC_STREAM_REUSE_SCANBUF
#if (WC_STREAM_REUSE_SCANBUF + 0)
#undef WC_STREAM_REUSE_SCANBUF
#define WC_STREAM_REUSE_SCANBUF 1
#else
#undef WC_STREAM_REUSE_SCANBUF
#define WC_STREAM_REUSE_SCANBUF 0
#endif
#else
#define WC_STREAM_REUSE_SCANBUF 0
#endif

#ifdef WC_ASCII_ONLY
#if (WC_ASCII_ONLY + 0)
#undef WC_ASCII_ONLY
#define WC_ASCII_ONLY 1
#else
#undef WC_ASCII_ONLY
#define WC_ASCII_ONLY 0
#endif
#else
#define WC_ASCII_ONLY 1
#endif

#ifdef WC_STACK_BUFFER
#if (WC_STACK_BUFFER + 0)
#undef WC_STACK_BUFFER
#define WC_STACK_BUFFER 1
#else
#undef WC_STACK_BUFFER
#define WC_STACK_BUFFER 0
#endif
#else
#define WC_STACK_BUFFER 1
#endif

#include <limits.h>
#include <stddef.h>

#if !WC_BOOL(WC_NO_HEAP) || WC_BOOL(WC_USE_LIBC_QSORT)
#include <stdlib.h>
#endif

#ifndef WC_SIZE_MAX
#if defined(SIZE_MAX)
#define WC_SIZE_MAX SIZE_MAX
#else
#define WC_SIZE_MAX (~(size_t)0)
#endif
#endif

/* WC_PTRDIFF_MAX: override only on non-standard platforms missing PTRDIFF_MAX. */
#ifndef WC_PTRDIFF_MAX
/* Prefer the standard PTRDIFF_MAX from <stdint.h> (C99). */
#if !defined(PTRDIFF_MAX)
#include <stdint.h>
#endif
#if defined(PTRDIFF_MAX)
#define WC_PTRDIFF_MAX PTRDIFF_MAX
#else
#error "PTRDIFF_MAX unavailable; define WC_PTRDIFF_MAX for this platform."
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
** Restrict qualifier abstraction.
**
**   - In C99 or later, WC_RESTRICT expands to 'restrict'.
**   - In C++, it uses compiler-specific extensions (__restrict) where available,
**     or expands to nothing if not supported.
*/
#ifndef WC_RESTRICT
#if defined(__cplusplus)
#if defined(_MSC_VER)
#define WC_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define WC_RESTRICT __restrict__
#else
#define WC_RESTRICT
#endif
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define WC_RESTRICT restrict
#else
#define WC_RESTRICT
#endif
#endif

/*
** Export/import decoration for shared builds.
*/
#ifndef WC_API
#if defined(_WIN32) && defined(WC_SHARED)
#ifdef WC_BUILD_SHARED
#define WC_API __declspec(dllexport)
#else
#define WC_API __declspec(dllimport)
#endif
#elif defined(WC_SHARED) && (defined(__clang__) || defined(__GNUC__))
#if defined(__has_attribute)
#if __has_attribute(visibility)
#define WC_API __attribute__((visibility("default")))
#else
#define WC_API
#endif
#else
#define WC_API __attribute__((visibility("default")))
#endif
#else
#define WC_API
#endif
#endif

/* Annotation macros:
   WC_RESTRICT  - expands to C99 restrict when available, else empty.
   WC_NODISCARD - expands to a warn-unused-result attribute when supported.
   WC_WUR       - internal shorthand used on public declarations. */
#ifndef WC_NODISCARD
#if defined(__GNUC__) || defined(__clang__)
#define WC_NODISCARD __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#ifdef _Check_return_
#define WC_NODISCARD _Check_return_
#else
#define WC_NODISCARD
#endif
#else
#define WC_NODISCARD
#endif
#endif

#ifndef WC_WUR
#define WC_WUR WC_NODISCARD
#endif

/*
** Version information. The version number is encoded as:
**   (MAJOR * 1000000) + (MINOR * 1000) + PATCH
** 5.0.0: canonical struct_size-based limits/build info; single open/stream API
** --- Versioning ------------------------------------------------------- */

#define WC_VERSION "5.0.0"
#define WC_VERSION_NUMBER 5000000UL

/* --- Result codes ----------------------------------------------------- */
/*
** Result codes for int-returning functions.
*/
#define WC_OK 0
#define WC_ERROR 1
#define WC_NOMEM 2
/*
** More specific error codes used by wc_open_ex / streaming APIs.
** (Other functions return only WC_OK/WC_ERROR/WC_NOMEM, except
** wc_scan/wc_add* may return WC_EBUSY when WC_STREAM_REUSE_SCANBUF=1
** and a stream is active.)
*/
#define WC_EALIGN 3     /* misaligned caller-provided buffer */
#define WC_EBADLIMITS 4 /* invalid/unsatisfiable limits (deterministic) */
#define WC_EBUSY 5      /* stream/static buffer already in use */

/*
** Memory allocator configuration. Define these before including
** wordcount.h to use a custom allocator.
*/

#ifndef WC_MALLOC
#define WC_MALLOC(n) malloc(n)
#endif
#ifndef WC_REALLOC
#define WC_REALLOC(p, n) realloc((p), (n))
#endif
#ifndef WC_FREE
#define WC_FREE(p) free(p)
#endif

/*
** Stack buffer configuration. Set to 0 for heap allocation.
**
** On tiny MCUs or deeply recursive call stacks, defining
** WC_STACK_BUFFER as 0 is recommended to avoid large fixed-size
** arrays on the stack. In that mode, scan buffers are allocated
** from the same internal pools that store words and hash slots.
*/

#ifndef WC_STACK_BUFFER
#define WC_STACK_BUFFER 1
#endif

/*
** Optional compile-time tuning for tiny/embedded targets.
**
** WC_MAX_WORD:
**   Upper bound on max_word accepted by wc_open/wc_open_ex.
**   Defaults to 1024. Lowering this reduces worst-case stack or
**   heap usage for scan buffers. The implementation will clamp
**   the runtime max_word argument into [4, WC_MAX_WORD].
**
** WC_MIN_INIT_CAP:
**   Lower bound on the initial hash table capacity (number of
**   slots) chosen by the internal tuner. Defaults to 16. May be
**   lowered for very small memory configurations; values must be
**   > 0 and are rounded up to a power of two internally.
**
** WC_MIN_BLOCK_SZ:
**   Lower bound on the first arena block size in bytes. Defaults
**   to 256. May be lowered for tiny static buffers. Reducing this
**   too far will limit how many distinct words can be stored
**   before WC_NOMEM is returned.
*/
#ifndef WC_MAX_WORD
#define WC_MAX_WORD 1024u
#endif
#ifndef WC_MIN_INIT_CAP
#define WC_MIN_INIT_CAP 16u
#endif
#ifndef WC_MIN_BLOCK_SZ
#define WC_MIN_BLOCK_SZ 256u
#endif
/*
** Default sizing for initial hash table capacity and arena block
** size. These can be overridden at compile time by defining
** WC_DEFAULT_INIT_CAP and/or WC_DEFAULT_BLOCK_SZ before including
** this header. If not defined, they are derived from WC_SIZE_MAX.
*/
#ifndef WC_DEFAULT_INIT_CAP
#define WC_DEFAULT_INIT_CAP         \
    ((WC_SIZE_MAX <= 65535u) ? 128u \
                             : ((WC_SIZE_MAX <= 4294967295u) ? 1024u : 4096u))
#endif

#ifndef WC_DEFAULT_BLOCK_SZ
#define WC_DEFAULT_BLOCK_SZ  \
    ((WC_SIZE_MAX <= 65535u) \
             ? 1024u         \
             : ((WC_SIZE_MAX <= 4294967295u) ? 16384u : 65536u))
#endif

/* --- Public types ----------------------------------------------------- */

/*
** WC_U32_T / WC_U64_T (advanced):
**   Override the internal hashing integer types.
**   Requirements (enforced by compile-time asserts in wordcount.c):
**     - WC_U32_T must be an unsigned integer type of exactly 32 bits.
**     - If WC_HASH_STRONG=1, WC_U64_T must be an unsigned integer type of exactly 64 bits.
**   Defaults use <stdint.h> uint32_t/uint64_t.
*/
#ifndef WC_U32_T
#include <stdint.h>
#define WC_U32_T uint32_t
#endif

typedef WC_U32_T wc_u32;

#if WC_BOOL(WC_HASH_STRONG)
#ifndef WC_U64_T
#include <stdint.h>
#define WC_U64_T uint64_t
#endif
typedef WC_U64_T wc_u64;
#endif

/*
** Opaque word counter handle.
*/
typedef struct wc wc;
/*
** Optional per-instance memory and sizing limits.
**
**   max_bytes:
**     Hard cap on total internal allocations for this wc object when
**     using the default dynamic allocator. The following pools are
**     counted against this limit:
**       - the hash table (Slot array and its growth)
**       - the arena blocks used for word storage
**       - the optional heap scan buffer when WC_STACK_BUFFER==0
**
**     Not counted against this limit:
**       - the wc handle itself when allocated outside the internal allocator
**         (e.g., via WC_MALLOC, or placed by the caller in handle_buf),
**       - arrays returned by wc_results() and wc_topn(),
**       - the wc_stream object returned by wc_stream_open().
**     These allocations are outside the wc allocator budget. 0 = unlimited.
**
**   init_cap:
**     Initial hash table capacity (number of slots). Must be > 0.
**     Rounded up to a power of two internally. 0 = library default
**     chosen from WC_DEFAULT_INIT_CAP based on platform.
**
**   block_size:
**     Arena block size in bytes. Acts as the typical allocation
**     quantum for word storage. 0 = library default chosen from
**     WC_DEFAULT_BLOCK_SZ based on platform.
**
**   static_buf / static_size:
**     Optional caller-supplied memory region for all internal
**     allocations. When static_buf is non-NULL and static_size > 0:
**
**       - The library does NOT call WC_MALLOC/WC_FREE for internal
**         structures (hash table, arena blocks, heap scan buffer).
**
**       - All such objects are carved out of [static_buf,
**         static_buf + static_size) using a simple bump allocator,
**         with alignment chosen to be safe for the internal types
**         used by the library.
**
**       - The buffer must be suitably aligned; on hosted
**         implementations, alignment at least as strict as that of
**         void*, size_t, unsigned long, and long double is sufficient.
**         Static-buffer mode requires uintptr_t; misaligned buffers are
**         rejected at runtime.
**
**       - The buffer must remain valid and must not be shared with
**         another wc instance for its entire lifetime.
**
**       - max_bytes, if non-zero, is treated as an additional guard
**         and is clamped to static_size when computing initial
**         sizing.
**
**     The wc handle itself is allocated via WC_MALLOC/WC_FREE unless the caller
**     provides limits.handle_buf. When WC_NO_HEAP=0, arrays returned by
**     wc_results()/wc_topn() and stream objects returned by wc_stream_open()
**     are allocated via WC_MALLOC/WC_FREE.
**
** On small systems, set max_bytes or static_size to a fixed budget
** and leave the others at 0 to let the library derive conservative
** values. On larger systems, you can tune init_cap/block_size
** directly.
**
** Portable helper to declare a suitably-aligned static buffer for
** wc_limits.static_buf in C99.
**
** Example:
**   WC_STATIC_BUFFER(pool, 2048);
**   wc_limits lim = WC_LIMITS_INIT();
**   lim.static_buf = pool.buf;
**   lim.static_size = sizeof pool.buf;
*/
#ifndef WC_STATIC_BUFFER
#define WC_STATIC_BUFFER(name, size_) \
    union {                           \
        void *p;                      \
        size_t sz;                    \
        unsigned long ul;             \
        long double ld;               \
        unsigned char buf[(size_)];   \
    } name
#endif

typedef struct wc_limits {
    /*
     * Size of this struct instance in bytes.
     *
     * Compatibility contract (append-only struct):
     *   - If struct_size < sizeof(wc_limits), missing tail fields are treated as 0.
     *   - If struct_size > sizeof(wc_limits), extra tail bytes are ignored.
     *   - struct_size must be non-zero.
     *
     * wc_limits_init()/WC_LIMITS_INIT() set struct_size to sizeof(wc_limits)
     * for this header.
     */
    size_t struct_size;

    size_t max_bytes;
    int strict_max_bytes;
    size_t init_cap;
    size_t block_size;
    void *static_buf;
    size_t static_size;
    unsigned long hash_seed;

    /* Optional upper bound on probe length during open addressing.
       0 = no bound (default). max_probe is the maximum slots examined per
       insertion/lookup, including the initial slot. In dynamic mode, hitting the bound triggers at
       most one grow attempt before failing with WC_NOMEM.
       Note: duplicates still increment even when the bound is hit; the bound
       primarily limits insertion of new unique keys under probe storms. */
    size_t max_probe;

    /* Optional caller-provided storage for the wc handle itself. If both
       handle_buf and handle_size are non-zero, the library will place the
       wc struct there instead of calling WC_MALLOC. If handle_buf aliases
       static_buf, it must start at static_buf and its footprint counts
       against the static budget. */
    void *handle_buf;
    size_t handle_size;
} wc_limits;

/* If you append fields to wc_limits, update the C++ initializer list here. */
#ifdef __cplusplus
/* C++ aggregate init: fully initialize to avoid -Wmissing-field-initializers. */
#define WC_LIMITS_INIT()                          \
    {                                             \
        sizeof(wc_limits), /* struct_size */      \
                0,         /* max_bytes */        \
                0,         /* strict_max_bytes */ \
                0,         /* init_cap */         \
                0,         /* block_size */       \
                0,         /* static_buf */       \
                0,         /* static_size */      \
                0ul,       /* hash_seed */        \
                0,         /* max_probe */        \
                0,         /* handle_buf */       \
                0          /* handle_size */      \
    }
#else
/* C99 constant initializer (valid for static storage). */
#define WC_LIMITS_INIT()                 \
    {                                    \
        .struct_size = sizeof(wc_limits) \
    }
#endif

/* Recommended initializer (sets struct_size and zeroes all fields). */
static inline void wc_limits_init(wc_limits *limits)
{
    if (!limits)
        return;
#ifdef __cplusplus
    *limits = wc_limits();
#else
    *limits = (wc_limits){ 0 };
#endif
    limits->struct_size = sizeof(*limits);
}
/*
** Result entry returned by wc_results().
*/
typedef struct wc_word {
    const char *word; /* owned by wc instance; invalid after wc_close */
    size_t count;
} wc_word;
/*
** Build-time configuration introspection.
**
**   struct_size:
**     Size of this struct instance (set to sizeof(wc_build_config)).
**
**   version_number:
**     Equal to WC_VERSION_NUMBER.
**
**   max_word:
**     Compile-time WC_MAX_WORD used when building this library.
**
**   min_init_cap / min_block_sz:
**     Compile-time WC_MIN_INIT_CAP / WC_MIN_BLOCK_SZ used when
**     building this library.
**
**   stack_buffer:
**     1 if WC_STACK_BUFFER was non-zero at build time, 0 otherwise.
**
**   sizeof_wc / sizeof_slot / sizeof_wc_limits:
**     Sizes of key internal types used when this library was built,
**     useful for diagnosing header/library mismatches.
*/
typedef struct wc_build_config {
    size_t struct_size;
    unsigned long version_number;
    size_t max_word;
    size_t min_init_cap;
    size_t min_block_sz;
    int stack_buffer;

    /* Extra introspection for diagnosing mismatches and ABI expectations */
    size_t sizeof_wc;
    size_t sizeof_slot;
    size_t sizeof_wc_limits;
} wc_build_config;

/* WC_BUILD_CONFIG_INIT: constant initializer for static storage in C;
   in C++ we fully initialize to avoid -Wmissing-field-initializers.
   If you append fields to wc_build_config, update the C++ initializer list. */
#ifdef __cplusplus
#define WC_BUILD_CONFIG_INIT()                          \
    {                                                   \
        sizeof(wc_build_config), /* struct_size */      \
                0ul,             /* version_number */   \
                0,               /* max_word */         \
                0,               /* min_init_cap */     \
                0,               /* min_block_sz */     \
                0,               /* stack_buffer */     \
                0,               /* sizeof_wc */        \
                0,               /* sizeof_slot */      \
                0                /* sizeof_wc_limits */ \
    }
#else
#define WC_BUILD_CONFIG_INIT()                 \
    {                                          \
        .struct_size = sizeof(wc_build_config) \
    }
#endif

/*
** Minimal runtime stats for observability and budgeting.
*/
typedef struct wc_stats {
    size_t bytes_used;
    size_t bytes_limit;
    int static_mode; /* 1 if using caller-provided static buffer */
    size_t cap;
    size_t arena_blocks;
} wc_stats;

/*
** wc_rc:
**   Convenience enum alias for result codes. Values match WC_OK/WC_ERROR/WC_NOMEM
**   and the extended codes (WC_EBADLIMITS/WC_EALIGN/WC_EBUSY).
**   You may use either the macros or this enum; they are equivalent.
*/
typedef enum wc_rc {
    WC_RC_OK = WC_OK,
    WC_RC_ERROR = WC_ERROR,
    WC_RC_NOMEM = WC_NOMEM,
    WC_RC_EALIGN = WC_EALIGN,
    WC_RC_EBADLIMITS = WC_EBADLIMITS,
    WC_RC_EBUSY = WC_EBUSY
} wc_rc;

/* --- Lifecycle -------------------------------------------------------- */
/*
** Create a new word counter with optional limits.
**
**   max_word: Maximum word length to store. 0 = default (64).
**             Clamped to range [4, WC_MAX_WORD].
**
**   limits:   Optional pointer to a wc_limits struct. May be NULL.
**             struct_size follows the append-only contract (wc_limits_init sets it).
**   err_out:  Optional pointer to receive detailed status:
**             WC_OK, WC_NOMEM, WC_ERROR, WC_EALIGN, WC_EBADLIMITS
**
** Returns NULL on allocation failure or if the supplied limits are
** impossible to satisfy (e.g., max_bytes/static_size too small for
** even minimal internal structures). err_out, if provided, is always set.
*/
WC_API wc *
wc_open_ex(size_t max_word, const wc_limits *limits, int *err_out) WC_WUR;
/*
** Create a new word counter with default limits (no explicit memory
** cap, platform-tuned defaults for table and arena sizes).
**
**   max_word: Maximum word length to store. 0 = default (64).
**             Clamped to range [4, WC_MAX_WORD].
**
** Returns NULL on allocation failure.
*/
WC_API wc *wc_open(size_t max_word);
/*
** Destroy a word counter. NULL-safe.
*/
WC_API void wc_close(wc *w);

/* Size helper for callers providing handle storage. */
WC_API size_t wc_handle_size(void);

/* --- Word insertion and scanning ------------------------------------- */

WC_API WC_WUR int wc_add(wc *w, const char *WC_RESTRICT word);
WC_API WC_WUR int wc_add_n(wc *w, const char *WC_RESTRICT word, size_t len);
WC_API WC_WUR int
wc_add_norm_n(wc *w, const char *WC_RESTRICT word, size_t len);
/* Note: for length-based adds, embedded '\0' terminates the word (prefix is used). */
WC_API WC_WUR int wc_scan(wc *w, const char *WC_RESTRICT text, size_t len);

/*
** Query total word count (including duplicates). Returns 0 if w is NULL.
*/
WC_API size_t wc_total(const wc *w);

/*
** Query unique word count. Returns 0 if w is NULL.
*/
WC_API size_t wc_unique(const wc *w);

/*
** Get sorted results (by count desc, then alphabetically).
**
**   out: Receives pointer to array (caller must free via
**        wc_results_free)
**   n:   Receives array length
**
** Returns WC_OK, WC_ERROR (bad args), or WC_NOMEM.
** On empty results, *out=NULL and *n=0 with WC_OK return.
**
** Note: The temporary results array is allocated via WC_MALLOC and
** is not counted against max_bytes in wc_limits, nor against any
** static buffer provided via wc_limits.static_buf, since its
** lifetime is entirely under the caller's control.
**
** When WC_NO_HEAP=1, this function returns WC_NOMEM unconditionally;
** use wc_results_into with caller-supplied storage instead.
*/
WC_API WC_WUR int
wc_results(const wc *w, wc_word **WC_RESTRICT out, size_t *WC_RESTRICT n);

/* Zero-allocation variants. Query mode: out==NULL or out_cap==0 sets out_n to
   required length and returns WC_OK. */
WC_API WC_WUR int
wc_results_into(const wc *w, wc_word *out, size_t out_cap, size_t *out_n);
/*
** Free results array. NULL-safe.
*/
WC_API void wc_results_free(wc_word *r);

/* Top-N helper (count desc, then lexicographic).
**
** Note: The output array returned via *out is allocated via WC_MALLOC and is
** not counted against max_bytes in wc_limits, nor against any static buffer
** provided via wc_limits.static_buf, since its lifetime is under the caller's
** control.
**
** When WC_NO_HEAP=1, returns WC_NOMEM unconditionally; use
** wc_topn_into with caller-supplied storage instead.
*/
WC_API WC_WUR int wc_topn(const wc *w,
                          size_t n,
                          wc_word **WC_RESTRICT out,
                          size_t *WC_RESTRICT out_n);

WC_API WC_WUR int wc_topn_into(
        const wc *w, size_t n, wc_word *out, size_t out_cap, size_t *out_n);

/*
** -------------------------------------------------------------------------
** Zero-allocation Iterator API
** -------------------------------------------------------------------------
**
** Allows iterating over all words without allocating a results array.
** Useful for strict memory budgets or streaming processing.
**
** Note: Iteration order is arbitrary (based on hash table layout) and
** is NOT sorted.
*/

typedef struct wc_cursor {
    const wc *w;
    size_t index;
} wc_cursor;
/*
** Initialize a cursor for iteration.
** w must remain valid during iteration.
*/
WC_API void wc_cursor_init(wc_cursor *c, const wc *w);
/*
** Advance to the next word.
**
**   word:  Receives pointer to the stored word string.
**   count: Receives the occurrence count.
**
** Returns 1 if a word was found, 0 if iteration is complete.
*/
WC_API WC_WUR int wc_cursor_next(wc_cursor *c,
                                 const char **WC_RESTRICT word,
                                 size_t *WC_RESTRICT count);

/* --- Utility ---------------------------------------------------------- */
/*
** Return human-readable error description.
** The returned string is static and must not be freed.
*/
WC_API const char *wc_errstr(int rc);
/*
** Return version string.
*/
WC_API const char *wc_version(void);

/*
** Return build-time configuration.
**
** The returned pointer refers to a static, immutable struct. It
** remains valid for the lifetime of the program.
*/
WC_API const wc_build_config *wc_build_info(void);

/* Runtime stats for allocator/capacity observability. */
WC_API WC_WUR int wc_get_stats(const wc *w, wc_stats *out);

/* Preflight/reserve capacity to reduce early WC_NOMEM. */
WC_API WC_WUR int
wc_reserve(wc *w, size_t expected_unique, size_t expected_bytes);

/* Optional invariant checker (WC_OK or WC_ERROR).
   Full validation is compiled in only when WC_ENABLE_VALIDATE is set. */
WC_API WC_WUR int wc_validate(const wc *w);

/*
** -------------------------------------------------------------------------
** Streaming scan API (library-owned; prevents CLI/library drift)
** -------------------------------------------------------------------------
**
** Allows wc_scan() semantics over chunked input (stdin, sockets, etc.).
** Handles words split across chunk boundaries.
**
** Notes:
**   - Tokenization, normalization, and truncation match wc_scan() exactly.
**   - With WC_ASCII_ONLY=1 (default), a word is [A-Za-z]+ and all other bytes are separators.
**   - With WC_ASCII_ONLY=0, word bytes are (isalnum || apostrophe) under the active C locale.
**   - Error handling differs from wc_scan(): on error, scan_ex reports partial consumption and
**     discards the failing word to allow forward progress.
*/
typedef struct wc_stream wc_stream;

/*
** wc_stream_open:
**   Allocates a wc_stream object via WC_MALLOC (not counted against wc_limits).
**   Returns NULL on failure and stores a code in *err_out when provided.
**   When WC_NO_HEAP=1, always returns NULL with *err_out = WC_NOMEM; use
**   wc_stream_open_inplace().
**
** wc_stream_open_inplace:
**   Uses caller-provided storage. On mem_size < wc_stream_size(w), fails with
**   WC_NOMEM (and errno may be set when WC_HAVE_ERRNO!=0).
*/
WC_API wc_stream *wc_stream_open(wc *w, int *err_out);
WC_API size_t wc_stream_size(const wc *w);
WC_API wc_stream *
wc_stream_open_inplace(wc *w, void *mem, size_t mem_size, int *err_out);

/*
** Scan a chunk. consumed_out (optional) receives the number of bytes
** consumed before returning. On WC_OK, consumed_out == len.
** On WC_NOMEM/WC_ERROR, the stream remains valid; the failing word is
** discarded and the separator is consumed so callers can continue with
** remaining bytes if desired.
*/
WC_API WC_WUR int wc_stream_scan_ex(wc_stream *s,
                                    const char *WC_RESTRICT buf,
                                    size_t len,
                                    size_t *WC_RESTRICT consumed_out);

/* Convenience wrapper: processes the entire chunk or returns error. */
WC_API WC_WUR int
wc_stream_scan(wc_stream *s, const char *WC_RESTRICT buf, size_t len);

/* Finish stream: inserts any trailing in-progress word (idempotent). */
WC_API WC_WUR int wc_stream_finish(wc_stream *s);

WC_API void wc_stream_close(wc_stream *s);

#ifdef __cplusplus
}
#endif

#endif /* WORDCOUNT_H */

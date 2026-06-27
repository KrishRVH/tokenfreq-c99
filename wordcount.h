/* wordcount.h - Word frequency counter
**
** CONTRACT
**
**   Language: C99. Defaults to hosted libc; freestanding/exotic builds
**   are supported via WC_STDC_HOSTED, WC_USE_LIBC_STRING, WC_USE_LIBC_QSORT,
**   WC_HAVE_ERRNO, WC_NO_HEAP, WC_LINEAR_UINTPTR_ALIGNMENT, and
**   WC_TRUST_STATIC_BUFFER_ALIGNMENT.
**
**   Threading/reentrancy: No global mutable state. Separate wc instances
**   may be used concurrently. A single wc or wc_stream must not be used
**   from multiple threads without external synchronization.
**
**   Encoding/word definition: Assumes 8-bit chars and ASCII-compatible
**   execution character set. A "word" is a maximal run of ASCII letters
**   [A-Za-z]; all other bytes are separators and case folding is ASCII-only.
**   wc_scan accepts arbitrary byte sequences up to WC_PTRDIFF_MAX bytes.
**   wc_add/wc_add_n do not fold case and store the supplied bytes up to the
**   first NUL; wc_add_norm_n does not tokenize and folds that same prefix using
**   ASCII rules. Zero-length wc_add_n, wc_add_norm_n, wc_scan, and
**   wc_stream_scan_ex calls are no-ops and may use NULL input pointers.
**
**   Maximum length: max_word is clamped into [4, WC_MAX_WORD]. Bytes
**   beyond max_word are truncated (still counted); truncation never
**   triggers an error.
**
**   Error model: status-returning APIs use WC_OK (0) for success and
**   WC_ERROR / WC_NOMEM as documented per function. WC_EALIGN and
**   WC_EBADLIMITS are detailed wc_open_ex err_out statuses. wc_cursor_next() is
**   a boolean iterator and returns 1 when it produces an item. err_out (when
**   provided) is written on success and failure. errno is only touched on
**   failures when WC_HAVE_ERRNO != 0. wc_errstr() yields stable,
**   human-readable strings.
**
**   Ownership/lifetime: wc_word.word pointers returned by wc_results() and
**   wc_topn() are owned by the wc instance and become invalid after wc_close().
**   Arrays returned by wc_results()/wc_topn() are caller-owned and must be freed
**   with wc_results_free(). Streams are closed with wc_stream_close(). If wc_close()
**   is called before an open stream is closed, the stream is detached: later
**   scan/finish calls return WC_ERROR and wc_stream_close() remains safe.
**
**   Determinism/sorting: Results are sorted by count descending then
**   bytewise word ascending; behavior is locale-independent.
**
**   Public API functions: wc_open_ex, wc_open, wc_close, wc_add,
**     wc_add_n, wc_add_norm_n, wc_scan, wc_total, wc_unique, wc_results,
**     wc_results_free, wc_topn, wc_reserve, wc_cursor_init, wc_cursor_next,
**     wc_errstr, wc_version, wc_build_info, wc_get_stats, wc_validate,
**     wc_stream_open, wc_stream_scan_ex, wc_stream_finish, wc_stream_close.
**
**   Public helpers: wc_limits_init, WC_LIMITS_INIT, WC_STATIC_BUFFER.
**   Supported build-configuration macros are documented below; they are not
**   callable API surface.
*/
#ifndef WORDCOUNT_H
#define WORDCOUNT_H

#ifdef WC_STDC_HOSTED
#if (WC_STDC_HOSTED + 0)
#undef WC_STDC_HOSTED
#define WC_STDC_HOSTED 1
#else
#undef WC_STDC_HOSTED
#define WC_STDC_HOSTED 0
#endif
#else
#ifdef __STDC_HOSTED__
#define WC_STDC_HOSTED __STDC_HOSTED__
#else
#define WC_STDC_HOSTED 1
#endif
#endif

#ifndef WC_HASH_STRONG
#define WC_HASH_STRONG 1
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
** WC_OMIT_ASSERT:
**   When 1, internal WC_ASSERT checks are compiled out and <assert.h> is not
**   included. Defaults to 0 for hosted builds and 1 for freestanding builds.
*/
#ifndef WC_OMIT_ASSERT
#if WC_STDC_HOSTED
#define WC_OMIT_ASSERT 0
#else
#define WC_OMIT_ASSERT 1
#endif
#endif

/*
** WC_NO_HEAP:
**   When 1, the library is built in "no-heap" mode:
**     - The compiled object does not reference WC_MALLOC/WC_FREE.
**     - wc_open() is unavailable; wc_open_ex() requires static_buf/static_size.
**     - wc_results()/wc_topn()/wc_stream_open() are unavailable (valid
**       result-output calls with a valid handle return WC_NOMEM;
**       wc_stream_open returns NULL); use wc_cursor and wc_scan().
*/
#ifndef WC_NO_HEAP
#define WC_NO_HEAP 0
#endif

#ifndef WC_TRUST_STATIC_BUFFER_ALIGNMENT
#define WC_TRUST_STATIC_BUFFER_ALIGNMENT 0
#endif

/* Normalize feature toggles to numeric 0/1 after defaults are chosen. */
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

#ifdef WC_OMIT_ASSERT
#if (WC_OMIT_ASSERT + 0)
#undef WC_OMIT_ASSERT
#define WC_OMIT_ASSERT 1
#else
#undef WC_OMIT_ASSERT
#define WC_OMIT_ASSERT 0
#endif
#else
#define WC_OMIT_ASSERT 0
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

#if (WC_USE_LIBC_QSORT + 0) || \
        (!(WC_NO_HEAP + 0) && (!defined(WC_MALLOC) || !defined(WC_FREE)))
#include <stdlib.h>
#endif

#ifndef WC_SIZE_MAX
#if defined(SIZE_MAX)
#define WC_SIZE_MAX SIZE_MAX
#else
#define WC_SIZE_MAX ((size_t)-1)
#endif
#endif

/* WC_PTRDIFF_MAX: override only on non-standard platforms missing PTRDIFF_MAX. */
#if defined(WC_PTRDIFF_MAX) && WC_STDC_HOSTED && !defined(PTRDIFF_MAX)
#include <stdint.h>
#endif
#if defined(WC_PTRDIFF_MAX) && defined(PTRDIFF_MAX) && \
        ((WC_PTRDIFF_MAX) > (PTRDIFF_MAX))
#error "WC_PTRDIFF_MAX must not exceed the platform PTRDIFF_MAX."
#endif
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

/* Declaration annotations for shared-library exports and ignored-result checks. */
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

#if defined(__GNUC__) || defined(__clang__)
#define WC_WUR __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#ifdef _Check_return_
#define WC_WUR _Check_return_
#else
#define WC_WUR
#endif
#else
#define WC_WUR
#endif

/* --- Versioning ------------------------------------------------------- */
/*
** Version number encoding:
**   (MAJOR * 1000000) + (MINOR * 1000) + PATCH
*/

#define WC_VERSION "5.0.0"
#define WC_VERSION_NUMBER 5000000UL

/* --- Result codes ----------------------------------------------------- */
/*
** Result codes for int-returning functions.
*/
#define WC_OK 0
#define WC_ERROR 1
#define WC_NOMEM 2 /* resource exhausted: allocation, capacity, or counter */
/*
** More specific status codes written through wc_open_ex(err_out).
** Other status-returning functions return only WC_OK/WC_ERROR/WC_NOMEM.
*/
#define WC_EALIGN 3     /* misaligned caller-provided buffer */
#define WC_EBADLIMITS 4 /* invalid/unsatisfiable limits (deterministic) */

/*
** Memory allocator configuration. Define these before including wordcount.h to
** use a custom allocator. WC_MALLOC must behave like malloc for the requested
** byte count: return NULL on failure, return storage suitably aligned for any
** object type used by this library, and keep that storage valid until passed to
** the matching WC_FREE.
*/

#ifndef WC_MALLOC
#define WC_MALLOC(n) malloc(n)
#endif
#ifndef WC_FREE
#define WC_FREE(p) free(p)
#endif

/*
** Stack buffer configuration. Set to 0 for heap allocation.
**
** On tiny MCUs or deeply recursive call stacks, defining
** WC_STACK_BUFFER as 0 is recommended to avoid large fixed-size
** arrays on the stack. In that mode, scan and normalization scratch buffers
** are allocated from the same internal pools that store words and hash slots.
**
** WC_STACK_MAX_WORD:
**   Upper bound allowed for WC_MAX_WORD when WC_STACK_BUFFER=1, because that
**   mode declares automatic scratch arrays sized at WC_MAX_WORD. Raise this
**   only when the target stack budget can support the larger arrays. Ignored
**   when WC_STACK_BUFFER=0.
*/

#ifndef WC_STACK_BUFFER
#define WC_STACK_BUFFER 1
#endif
#ifndef WC_STACK_MAX_WORD
#define WC_STACK_MAX_WORD 4096u
#endif

/*
** Optional compile-time tuning for tiny/embedded targets.
**
** WC_MAX_WORD:
**   Upper bound on max_word accepted by wc_open/wc_open_ex.
**   Defaults to 1024. Lowering this reduces worst-case scratch
**   usage for scan and normalization buffers. The implementation will clamp
**   the runtime max_word argument into [4, WC_MAX_WORD]. Must be an integer
**   constant >= 4 and must leave room within WC_PTRDIFF_MAX for the stored
**   trailing NUL, arena alignment padding, internal block header, and
**   wc_stream object storage. Invalid configurations fail to compile. Must
**   also be <= WC_STACK_MAX_WORD when WC_STACK_BUFFER=1.
**
** WC_MIN_INIT_CAP:
**   Lower bound on the initial hash table capacity (number of
**   slots) chosen by the internal tuner. Defaults to 16. May be
**   lowered for very small memory configurations; values must be
**   positive powers of two and must fit within WC_PTRDIFF_MAX as one
**   internal Slot table object.
**
** WC_MIN_BLOCK_SZ:
**   Lower bound on the first arena block size in bytes. Defaults
**   to 256. May be lowered for tiny static buffers. Reducing this
**   too far will limit how many distinct words can be stored
**   before WC_NOMEM is returned. Must leave room within
**   WC_PTRDIFF_MAX for the internal block header.
**
** WC_DEFAULT_INIT_CAP / WC_DEFAULT_BLOCK_SZ:
**   Initial default table capacity and arena block size. The init
**   capacity must be a positive power of two. Both defaults must fit
**   the same object-span constraints as WC_MIN_INIT_CAP/WC_MIN_BLOCK_SZ.
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
**     - Custom types must not require alignment stricter than void*, size_t,
**       unsigned long, and long double.
**   Defaults use <stdint.h> uint32_t/uint64_t.
**   Targets without usable <stdint.h> should define WC_U32_T, WC_PTRDIFF_MAX,
**   and WC_HAVE_UINTPTR=0 explicitly. Targets with uintptr_t values unsuitable
**   for modulo alignment checks should define WC_LINEAR_UINTPTR_ALIGNMENT=0.
*/
#ifndef WC_U32_T
#include <stdint.h>
#define WC_U32_T uint32_t
#endif

#if (WC_HASH_STRONG + 0)
#ifndef WC_U64_T
#include <stdint.h>
#define WC_U64_T uint64_t
#endif
#endif

#ifdef WC_HAVE_UINTPTR
#if (WC_HAVE_UINTPTR + 0)
#undef WC_HAVE_UINTPTR
#define WC_HAVE_UINTPTR 1
#else
#undef WC_HAVE_UINTPTR
#define WC_HAVE_UINTPTR 0
#endif
#else
#include <stdint.h>
#if defined(UINTPTR_MAX)
#define WC_HAVE_UINTPTR 1
#else
#define WC_HAVE_UINTPTR 0
#endif
#endif

/*
** WC_LINEAR_UINTPTR_ALIGNMENT:
**   When 1, uintptr_t integer values are assumed to preserve enough linear
**   address information for `(uintptr_t)p % align` to be a valid runtime
**   alignment check. This is true for conventional hosted and embedded
**   flat-address targets. Exotic or segmented targets that provide uintptr_t
**   only for pointer round-tripping should define this to 0; static-buffer mode
**   will then fail closed unless WC_TRUST_STATIC_BUFFER_ALIGNMENT=1 makes
**   correct alignment a caller precondition.
*/
#ifndef WC_LINEAR_UINTPTR_ALIGNMENT
#define WC_LINEAR_UINTPTR_ALIGNMENT WC_HAVE_UINTPTR
#endif

#ifdef WC_LINEAR_UINTPTR_ALIGNMENT
#if (WC_LINEAR_UINTPTR_ALIGNMENT + 0)
#undef WC_LINEAR_UINTPTR_ALIGNMENT
#define WC_LINEAR_UINTPTR_ALIGNMENT 1
#else
#undef WC_LINEAR_UINTPTR_ALIGNMENT
#define WC_LINEAR_UINTPTR_ALIGNMENT 0
#endif
#else
#define WC_LINEAR_UINTPTR_ALIGNMENT 0
#endif

#if (WC_LINEAR_UINTPTR_ALIGNMENT + 0) && !(WC_HAVE_UINTPTR + 0)
#error "WC_LINEAR_UINTPTR_ALIGNMENT=1 requires WC_HAVE_UINTPTR=1."
#endif

/*
** Opaque word counter handle.
*/
typedef struct wc wc;
/*
** Optional per-instance memory and sizing limits.
**
**   max_bytes:
**     Steady-state budget for internal allocations for this wc object when
**     using the default dynamic allocator. If strict_max_bytes is non-zero,
**     the same budget is enforced as a hard peak cap. The following pools are
**     counted against this budget:
**       - the hash table (Slot array and its growth)
**       - the arena blocks used for word storage
**       - the optional heap scan/normalization scratch buffer when
**         WC_STACK_BUFFER==0
**
**     Not counted against this budget:
**       - the wc handle itself in heap-enabled builds,
**       - arrays returned by wc_results() and wc_topn(),
**       - the wc_stream object returned by wc_stream_open().
**     These allocations are outside the wc allocator budget. In non-strict
**     dynamic mode, table growth may transiently exceed max_bytes if the
**     post-growth footprint fits. 0 = unlimited.
**
**   init_cap:
**     Initial hash table capacity (number of slots). If nonzero, rounded up
**     to a power of two internally. 0 = library default chosen from
**     WC_DEFAULT_INIT_CAP based on platform.
**
**   block_size:
**     Arena block size in bytes. Acts as the typical allocation
**     quantum for word storage. 0 = library default chosen from
**     WC_DEFAULT_BLOCK_SZ based on platform. In static-buffer mode, a
**     defaulted block_size is expanded to use the remaining effective static
**     budget after fixed allocations; an explicit block_size is honored when
**     the static dry run can satisfy it.
**
**   static_buf / static_size:
**     Optional caller-supplied memory region for all internal
**     allocations. When static_buf is non-NULL and static_size > 0:
**
**       - The library does NOT call WC_MALLOC/WC_FREE for internal
**         structures (hash table, arena blocks, heap scan/normalization
**         scratch buffer).
**
**       - All such objects are carved out of [static_buf,
**         static_buf + static_size) using a simple bump allocator,
**         with alignment chosen to be safe for the internal types
**         used by the library.
**
**       - The buffer must be suitably aligned; on hosted implementations,
**         alignment at least as strict as that of void*, size_t, unsigned long,
**         and long double is sufficient for supported configurations. Custom
**         WC_U32_T / WC_U64_T types with stricter alignment are rejected at
**         compile time. When uintptr_t is available and
**         WC_LINEAR_UINTPTR_ALIGNMENT=1, misaligned buffers are rejected at
**         runtime. Without a usable linear uintptr_t alignment model,
**         static-buffer mode is rejected unless WC_TRUST_STATIC_BUFFER_ALIGNMENT=1;
**         in that trust mode, alignment of static_buf is a caller precondition
**         and cannot be verified at runtime.
**
**       - Static-buffer mode uses one arena block for word storage. With an
**         explicit block_size, that value is the static word-storage capacity,
**         not a request to allocate more blocks from unused static storage.
**
**       - The buffer must remain valid and must not be shared with
**         another wc instance for its entire lifetime.
**
**       - In WC_NO_HEAP=1 builds, the handle is placed at static_buf and its
**         aligned footprint is charged to this buffer.
**
**       - max_bytes, if non-zero, is treated as an additional guard
**         and is clamped to static_size when computing initial
**         sizing.
**
**     The wc handle itself is allocated via WC_MALLOC/WC_FREE in heap-enabled
**     builds. In WC_NO_HEAP=1 builds, the handle is placed at static_buf. When
**     WC_NO_HEAP=0, arrays returned by wc_results()/wc_topn() and stream
**     objects returned by wc_stream_open() are allocated via WC_MALLOC/WC_FREE.
**
** On small static-buffer systems, set static_buf/static_size and leave
** init_cap/block_size at 0 to let the library derive a fitting layout.
** In dynamic mode, max_bytes is a budget guard; auto-selected layouts retry
** smaller table capacities before reporting an impossible budget. On larger
** systems, you can tune init_cap/block_size directly.
**
** Portable helper to declare a suitably-aligned static buffer for
** wc_limits.static_buf in C99.
**
** WC_STATIC_BUFFER provides alignment only. Static-buffer mode also assumes
** the target implementation permits suitably aligned caller-provided byte
** storage to back typed internal objects. This is the common embedded/hosted C
** implementation model, but strictly conforming ISO C does not define every
** effective-type detail for arbitrary unsigned char backing arrays.
**
** Example:
**   WC_STATIC_BUFFER(pool, 2048);
**   wc_limits lim = WC_LIMITS_INIT();
**   lim.static_buf = pool.buf;
**   lim.static_size = sizeof pool.buf;
*/
#ifndef WC_STATIC_BUFFER
#if (WC_HASH_STRONG + 0)
#define WC_STATIC_BUFFER(name, size_) \
    union {                           \
        void *p;                      \
        size_t sz;                    \
        unsigned long ul;             \
        long double ld;               \
        WC_U32_T u32;                 \
        WC_U64_T u64;                 \
        unsigned char buf[(size_)];   \
    } name
#else
#define WC_STATIC_BUFFER(name, size_) \
    union {                           \
        void *p;                      \
        size_t sz;                    \
        unsigned long ul;             \
        long double ld;               \
        WC_U32_T u32;                 \
        unsigned char buf[(size_)];   \
    } name
#endif
#endif

typedef struct wc_limits {
    /*
     * Size of this struct instance in bytes.
     *
     * Compatibility contract (append-only struct):
     *   - If struct_size < sizeof(wc_limits), missing tail fields are treated as 0.
     *   - If struct_size > sizeof(wc_limits), extra tail bytes are ignored.
     *   - struct_size must be at least sizeof(size_t), because struct_size is
     *     the required first field read from caller storage.
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
} wc_limits;

/* If you append fields to wc_limits, update the C++ initializer list here. */
#ifdef __cplusplus
/* C++ aggregate init: fully initialize to avoid -Wmissing-field-initializers. */
#define WC_LIMITS_INIT()                          \
    {                                             \
        sizeof(wc_limits), /* struct_size */      \
        0,                 /* max_bytes */        \
        0,                 /* strict_max_bytes */ \
        0,                 /* init_cap */         \
        0,                 /* block_size */       \
        0,                 /* static_buf */       \
        0,                 /* static_size */      \
        0ul                /* hash_seed */        \
    }
#else
/* C99 constant initializer (valid for static storage). */
#define WC_LIMITS_INIT() { .struct_size = sizeof(wc_limits) }
#endif

/* Recommended initializer (sets struct_size and zeroes all fields). */
static inline void wc_limits_init(wc_limits *limits)
{
    if (!limits)
        return;
    limits->struct_size = sizeof(*limits);
    limits->max_bytes = 0;
    limits->strict_max_bytes = 0;
    limits->init_cap = 0;
    limits->block_size = 0;
    limits->static_buf = NULL;
    limits->static_size = 0;
    limits->hash_seed = 0ul;
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
**   hosted / use_libc_string / use_libc_qsort / have_errno / no_heap /
**   hash_strong / have_uintptr / trust_static_buffer_alignment /
**   linear_uintptr_alignment:
**     Normalized build-time feature toggles for the compiled library.
**
**   stack_max_word:
**     Compile-time WC_STACK_MAX_WORD used when building this library.
*/
typedef struct wc_build_config {
    size_t struct_size;
    unsigned long version_number;
    size_t max_word;
    size_t min_init_cap;
    size_t min_block_sz;
    int stack_buffer;

    int hosted;
    int use_libc_string;
    int use_libc_qsort;
    int have_errno;
    int no_heap;
    int hash_strong;
    int have_uintptr;
    int trust_static_buffer_alignment;
    int linear_uintptr_alignment;
    size_t stack_max_word;
} wc_build_config;

/*
** Minimal runtime stats for observability and budgeting.
**
**   bytes_used:
**     Counted internal allocator bytes currently charged to this wc. In static
**     mode this includes alignment padding and, in WC_NO_HEAP=1 builds, the
**     aligned handle footprint.
**   bytes_limit:
**     The active max_bytes guard. 0 means no explicit max_bytes guard; a static
**     buffer can still bound allocation through static_size.
**   static_mode:
**     1 when internal allocations are carved from wc_limits.static_buf.
**   cap:
**     Current hash table slot capacity.
**   arena_blocks:
**     Current arena block count. Static-buffer mode uses one arena block.
*/
typedef struct wc_stats {
    size_t bytes_used;
    size_t bytes_limit;
    int static_mode; /* 1 if using caller-provided static buffer */
    size_t cap;
    size_t arena_blocks;
} wc_stats;

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
WC_API WC_WUR wc *
wc_open_ex(size_t max_word, const wc_limits *limits, int *err_out);
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

/* --- Word insertion and scanning ------------------------------------- */

WC_API WC_WUR int wc_add(wc *w, const char *word);
WC_API WC_WUR int wc_add_n(wc *w, const char *word, size_t len);
WC_API WC_WUR int wc_add_norm_n(wc *w, const char *word, size_t len);
/* Length-based adds accept word==NULL only when len==0. Embedded '\0'
   terminates the word (prefix is used). */
/* wc_scan accepts text==NULL only when len==0 and rejects len > WC_PTRDIFF_MAX
   with WC_ERROR. */
WC_API WC_WUR int wc_scan(wc *w, const char *text, size_t len);

/*
** Query total word count (including duplicates). Returns 0 if w is NULL.
*/
WC_API size_t wc_total(const wc *w);

/*
** Query unique word count. Returns 0 if w is NULL.
*/
WC_API size_t wc_unique(const wc *w);

/*
** Get sorted results (by count desc, then bytewise word ascending).
**
**   out: Receives pointer to array (caller must free via
**        wc_results_free)
**   n:   Receives array length
**
** Returns WC_OK, WC_ERROR (bad args), or WC_NOMEM.
** On empty heap-enabled results, *out=NULL and *n=0 with WC_OK return.
**
** Note: The temporary results array is allocated via WC_MALLOC and
** is not counted against max_bytes in wc_limits, nor against any
** static buffer provided via wc_limits.static_buf, since its
** lifetime is entirely under the caller's control.
**
** When WC_NO_HEAP=1, this function returns WC_NOMEM when output arguments are
** valid; bad arguments still return WC_ERROR. Use wc_cursor instead.
*/
WC_API WC_WUR int wc_results(const wc *w, wc_word **out, size_t *n);

/*
** Free results array. NULL-safe.
*/
WC_API void wc_results_free(wc_word *r);

/* Top-N helper (count desc, then bytewise word ascending).
**
** Note: The output array returned via *out is allocated via WC_MALLOC and is
** not counted against max_bytes in wc_limits, nor against any static buffer
** provided via wc_limits.static_buf, since its lifetime is under the caller's
** control.
**
** When WC_NO_HEAP=1, returns WC_NOMEM when output arguments are valid; bad
** arguments still return WC_ERROR. Use wc_cursor and sort externally if a
** sorted result is required.
*/
WC_API WC_WUR int wc_topn(const wc *w, size_t n, wc_word **out, size_t *out_n);

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
**   word:  Optional output pointer for the stored word string.
**   count: Optional output pointer for the occurrence count.
**
** Returns 1 if a word was found, 0 if iteration is complete.
*/
WC_API WC_WUR int
wc_cursor_next(wc_cursor *c, const char **word, size_t *count);

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

/* Best-effort preflight/growth helper to reduce early WC_NOMEM. It is not a
   success guarantee for future insertions; callers must still handle WC_NOMEM
   from insertion and scanning APIs. */
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
**   - A word is [A-Za-z]+ and all other bytes are separators.
**   - Error handling differs from wc_scan(): when an active stream reaches a
**     separator and insertion of the buffered word fails, scan_ex reports
**     partial consumption and discards that word to allow forward progress.
*/
typedef struct wc_stream wc_stream;

/*
** wc_stream_open:
**   Allocates a wc_stream object via WC_MALLOC (not counted against wc_limits).
**   Returns NULL on failure and stores a code in *err_out when provided.
**   When WC_NO_HEAP=1, always returns NULL with *err_out = WC_NOMEM.
**   If the parent wc is closed before the stream, the stream is detached:
**   later scan/finish calls return WC_ERROR and wc_stream_close() remains safe.
*/
WC_API wc_stream *wc_stream_open(wc *w, int *err_out);

/*
** Scan a chunk. consumed_out (optional) receives the number of bytes
** consumed before returning. On WC_OK before finish, consumed_out == len.
** buf may be NULL only when len==0. len must be <= WC_PTRDIFF_MAX; larger
** chunks return WC_ERROR with no progress.
** After a terminal wc_stream_finish() result, wc_stream_scan_ex() returns the
** saved finish status and leaves consumed_out at 0.
** If insertion of a buffered word fails while scanning a separator, the stream
** remains valid; that word is discarded and the separator is consumed so callers
** can continue with remaining bytes if desired. WC_ERROR from invalid arguments,
** detached streams, or oversized chunks does not provide forward progress.
*/
WC_API WC_WUR int wc_stream_scan_ex(wc_stream *s,
                                    const char *buf,
                                    size_t len,
                                    size_t *consumed_out);

/*
** Finish stream: inserts any trailing in-progress word. On WC_NOMEM, the
** trailing word remains buffered and the stream is not finished, so callers may
** retry. Other return codes finish the stream and are idempotently returned by
** later finish/scan calls. Closing a stream does not finish it; call
** wc_stream_finish first when the trailing word should be counted.
*/
WC_API WC_WUR int wc_stream_finish(wc_stream *s);

WC_API void wc_stream_close(wc_stream *s);

#ifdef __cplusplus
}
#endif

#endif /* WORDCOUNT_H */

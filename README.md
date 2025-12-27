# word-frequency-counter – C99 Word Frequency Library

A small, embeddable, **C99** word-frequency library with:

- Predictable memory use (optional hard caps and static-buffer mode)
- Robust error handling (including torture-tested OOM paths)
- Cross-platform support (Windows + POSIX)
- A clean, stable API (single header + single implementation)
- C++-friendly linkage via `extern "C"` and a `WC_RESTRICT` macro
- Build-configuration introspection (`wc_build_info`) for ABI sanity checks
- A zero-allocation iterator API for memory-constrained enumeration

`wordcount.h` defines the public API contract. This README explains the
expected behavior, invariants, and integration model for the library.
For a concise reference, see:

- `docs/spec.md` (normative API/behavior spec)
- `docs/guide.md` (practical integration guide)
- `docs/cli.md` (CLI contract and formats)

---

## Table of Contents

1. [Overview](#overview)
2. [Motivation and Non-Goals](#motivation-and-non-goals)
3. [High-Level Design](#high-level-design)
   - [Data Model](#data-model)
   - [Hash Table](#hash-table)
   - [Arena Allocator](#arena-allocator)
   - [Memory Accounting and Limits](#memory-accounting-and-limits)
   - [Static Buffer (MCU / No-malloc Mode)](#static-buffer-mcu--no-malloc-mode)
   - [Invariants and Consistency Guarantees](#invariants-and-consistency-guarantees)
   - [Error Model](#error-model)
   - [Thread Safety](#thread-safety)
4. [API Reference](#api-reference)
   - [Types](#types)
   - [Result Codes](#result-codes)
   - [Lifecycle Functions](#lifecycle-functions)
   - [Word Insertion and Scanning](#word-insertion-and-scanning)
   - [Query and Results Functions](#query-and-results-functions)
   - [Zero-Allocation Iterator API](#zero-allocation-iterator-api)
   - [Utility Functions](#utility-functions)
5. [Word Detection and Normalization](#word-detection-and-normalization)
6. [Memory Configuration](#memory-configuration)
   - [Compile-Time Configuration Macros](#compile-time-configuration-macros)
   - [Runtime Limits via `wc_limits`](#runtime-limits-via-wc_limits)
7. [Building and Integration](#building-and-integration)
   - [Using CMake (Presets, Recommended)](#using-cmake-presets-recommended)
   - [One-shot Build & Quality Script](#one-shot-build--quality-script)
   - [Direct Compilation](#direct-compilation)
   - [Directory Layout](#directory-layout)
8. [CLI Tool (`wc`)](#cli-tool-wc)
9. [Testing, Fuzzing, and OOM Injection](#testing-fuzzing-and-oom-injection)
10. [Portability and Platform Assumptions](#portability-and-platform-assumptions)
11. [Adversarial Inputs and Complexity](#adversarial-inputs-and-complexity)
12. [ABI and Build Configuration Compatibility](#abi-and-build-configuration-compatibility)
13. [Performance and Complexity](#performance-and-complexity)
14. [Versioning and Stability Guarantees](#versioning-and-stability-guarantees)
15. [Guidelines for Contributors](#guidelines-for-contributors)
16. [License](#license)

---

## Overview

The `wordcount` library provides a **small, robust, and embeddable** API for
counting word frequencies in text. It is designed to be:

- **Predictable**: all internal allocations are overflow-checked; behavior on
  `WC_NOMEM` is defined and tested.
- **Portable**: pure C99 core, with no dependencies beyond the standard C
  library.
- **Configurable**: suitable both for desktop/server workloads and
  resource-constrained embedded systems.
- **Defensive**: fails fast on unsupported platforms (non-ASCII, strange
  character encodings) via compile-time checks.
- **Inspectable**: build-time configuration is available at runtime via
  `wc_build_info`.

The core implementation resides in:

- `wordcount.h` – public API
- `wordcount.c` – implementation

On top of the library, the repository ships:

- `wc_main.c` – a CLI tool using memory-mapped I/O for files and streaming for
  stdin
- `wc_test.c` – a comprehensive test suite with optional OOM injection + fuzz harness
- `CMakeLists.txt` / `CMakePresets.json` – cross-compiler build configuration
- `c-build.sh` / `c-quality.sh` – developer tooling for multi-toolchain builds
  and static analysis
- `mingw-w64.cmake` – optional MinGW cross-compilation toolchain file

Quick start (library + CLI):

```bash
cmake --preset clang && cmake --build --preset clang && ./build/clang/wc input.txt
```

```c
wc_limits lim;
wc_limits_init(&lim);
lim.max_bytes = 8 * 1024 * 1024; /* optional budget */
wc *w = wc_open_ex(0, &lim, NULL);
wc_add_norm_n(w, "Hello", 5); /* length-based, lowercased */
wc_reserve(w, 1024, 0);       /* optional preflight */
/* ... feed data ... */
wc_word *out = NULL; size_t n = 0;
wc_topn(w, 10, &out, &n);
/* counts sorted by count desc, then lex asc */
wc_results_free(out);
wc_close(w);
```

Length-based adds (`wc_add_n` / `wc_add_norm_n`) truncate at the first embedded
`'\0'` so stored words remain valid C strings for sorting and comparison. `wc_scan`
continues to accept arbitrary bytes.

---

## Motivation and Non-Goals

### Motivation

The library is aimed at:

- Applications that need **word frequency statistics** as a component:
  - Text analytics, indexing, search, tagging
  - Developer tools and static analyzers
- Environments that require **tight control over memory usage**:
  - Embedded Linux, RTOSes, larger MCUs
  - Sandboxed / untrusted-input scenarios
- Systems that value **robustness over cleverness**:
  - All size calculations are overflow-checked
  - All allocation failures yield defined error codes
  - No undefined behavior for malformed input on supported platforms
- Scenarios where **configuration drift matters**:
  - You can introspect the build-time configuration at runtime and verify it
    matches your expectations.

### Non-Goals

The library deliberately does **not** try to:

- Provide Unicode word segmentation or locale-aware rules  
  Default build keeps word detection ASCII-only (`A–Z`, `a–z`). Building with
  `-DWC_ASCII_ONLY=0` opts into ctype/locale-dependent tokenization
  (`isalnum` + apostrophe).
- Be the fastest possible implementation on x86-64  
  The focus is correctness and robustness, not micro-benchmarks.
- Expose complex concurrency primitives  
  The library is re-entrant and per-instance thread-safe; it does not manage
  threads for you.
- Support arbitrary text encodings or EBCDIC  
  It assumes an ASCII-compatible execution character set.
- Provide cryptographic or DoS-hard hashing  
  FNV-1a is used for speed and reproducibility; a hash seed is available for
  simple randomization, but this is not a cryptographic defense.

---

## High-Level Design

### Data Model

A `wc` instance (opaque handle) tracks:

- A hash table of **slots**, each holding:
  - `word` – pointer to NUL-terminated stored word
  - `hash` – precomputed 32-bit hash (FNV-1a over stored characters, with a per-instance seed)
  - `cnt` – occurrence count
  - `n` – stored key length (collision-safe comparisons)
- An **arena** of one or more blocks in which:
  - Word strings are stored in contiguous buffers
- Global counters:
  - `tot` – total number of words observed (including duplicates)
  - `len` – number of unique words
- Configuration:
  - `maxw` – maximum stored word length (runtime; clamped)
  - Internal memory accounting / limits
  - A per-instance hash seed used for hash randomization

### Hash Table

- **Open addressing** with **linear probing**
- **Power-of-two capacity** (`cap` always a power of two):
  - Index: `hash & (cap - 1)`
  - With a 32-bit masked hash, `cap` never grows beyond 2^32 on 64-bit
    platforms; `wc_reserve`/growth will fail cleanly before that.
- **Load factor threshold** ~0.7:
  - When inserting a *new unique word* and `len * 10 >= cap * 7`, the table is doubled
    (unless static-buffer mode is active).
- **Collision-safe comparisons**:
  - Slots store both `hash` and `n` (length), so comparisons check:
    - hash equality
    - length equality
    - `memcmp` of exactly `n` bytes  
  This prevents out-of-bounds reads even for adversarial hash collisions.

Hash function and storage:

- Each slot stores a precomputed hash in `wc_hash_t` (internally `wc_u32` by default).
- Default hashing is FNV-1a over the stored bytes, using a per-instance basis derived from the FNV offset and optional `wc_limits.hash_seed`.
- With `WC_HASH_STRONG=1`, hashing switches to SipHash-2-4 keyed from `hash_seed` and the stored slot hash is the low 32 bits (used for indexing and fast rejection).


```c
/* Conceptual (default build) */
wc_hash_t h = basis;
for each byte c:
    h ^= (unsigned char)c;
    h *= 16777619u;
/* `h` is stored in Slot.hash (wc_hash_t). */
```
The public function signatures use only standard types (`size_t`, `int`, pointers).
The public header defines internal hashing typedefs (`wc_u32` and, when enabled, `wc_u64`)
to make hashing behavior explicit and portable without leaking those types into API
signatures.

### Arena Allocator

Word strings are stored in an arena:

* First block allocated during `wc_open_ex`
* Subsequent blocks added as needed (unless in static-buffer mode)
* Each block:

  * Header: `next`, `cur`, `end`
  * Flexible buffer: `buf[]`
* Allocations:

  * Bump-pointer within the current block
  * Alignment to an internal alignment requirement (`WC_ALIGN`) derived from the
    union of internal types used by the allocator
  * Zero-initialized memory for safety and deterministic behavior

In static-buffer mode:

* Only a **single initial block** is used; no further blocks are allocated.
* When the arena fills up, insertions of *new unique words* fail with `WC_NOMEM`
  while leaving the structure in a valid, queryable state.

### Memory Accounting and Limits

Internal allocations (hash table, arena blocks, optional heap/static scan
buffer) are accounted in `bytes_used`, up to `bytes_limit`:

* In **dynamic mode** (no static buffer):

  * `bytes_limit` (from `wc_limits.max_bytes`) caps internal allocations.
* In **static-buffer mode**:

  * All internal allocations are carved out of `[static_buf,
    static_buf + static_size)`.
  * `bytes_limit`, if provided, is clamped to `static_size` and acts as an
    additional guard.
  * `bytes_used` includes internal alignment padding (strict cap).

All relevant arithmetic is overflow-checked; any attempted allocation that would
overflow `size_t` or exceed configured limits fails cleanly.

Allocations performed via the public interface (e.g., `wc_results` buffer) are
*not* counted against these internal limits, as their lifetime is under the
caller’s control. Internal table growth may temporarily allocate via
`WC_MALLOC` to avoid peak budget spikes; accounting is updated after the rehash
completes.

### Static Buffer (MCU / No-malloc Mode)

For MCU-style environments without reliable `malloc`/`free`,
`wc_open_ex` can be configured with a caller-supplied static buffer:

* `static_buf` – pointer to caller-owned memory
* `static_size` – size of that region in bytes

In this mode:

* **All internal allocations** (hash table, first arena block, heap/static scan
  buffer when `WC_STACK_BUFFER == 0`) use a bump allocator inside the static buffer.
* **No re-use** or freeing occurs inside the buffer.
* **Alignment matters**: use the `WC_STATIC_BUFFER(name, size)` helper to get
  portable alignment for the internal types. Misaligned buffers are rejected
  with `WC_EALIGN`.
* **Hash table growth is disabled**:

  * Once the load factor exceeds ~0.7, inserting a *new unique* word fails with `WC_NOMEM`.
* **Arena growth is disabled**:

  * All stored unique words must fit in the first block; further growth yields `WC_NOMEM`.

When `WC_NO_HEAP=0` (default):

* Unless `wc_limits.handle_buf` is provided, the handle (`struct wc`) itself is
  allocated via `WC_MALLOC` / `WC_FREE`.
* Arrays returned by `wc_results()` / `wc_topn()` are allocated via
  `WC_MALLOC` / `WC_FREE`.
* `wc_stream_open()` allocates a small `wc_stream` object via `WC_MALLOC` /
  `WC_FREE` (closed by `wc_stream_close()`).

When `WC_NO_HEAP=1`:

* The library never calls `malloc` / `realloc` / `free`.
* `wc_open_ex` requires `static_buf` (or fails).
* `wc_results` and `wc_topn` return `WC_NOMEM` unconditionally; use
  `wc_results_into` / `wc_topn_into` with caller-supplied storage, or
  iterate via `wc_cursor`.
* `wc_stream_open` returns `NULL` with `WC_NOMEM`; use
  `wc_stream_open_inplace` with caller-supplied storage.

On very small systems with `WC_NO_HEAP=0`, redirect `WC_MALLOC` / `WC_FREE`
to a custom allocator or secondary static pool for the handle and results
arrays.

To make behavior deterministic and fail-fast:

* In static-buffer mode, `wc_open_ex` performs an **initialization-time dry
  run** using a scratch allocator state that is **side-effect free**: it
  validates the budget and alignment without writing into the caller’s static
  buffer (except for optional `handle_buf` storage if explicitly provided).
  It simulates allocating:

  * The initial hash table
  * The first arena block
  * The optional heap/static scan buffer (if `WC_STACK_BUFFER == 0`)
  * If any simulated allocation would fail under the effective budget
    (`min(static_size, max_bytes)` when both are set), `wc_open_ex` returns
    `NULL` without creating the instance.

Static buffer alignment:

- When `uintptr_t` is available, `wc_open_ex` verifies `static_buf` alignment at runtime.
- If `uintptr_t` is not available, static-buffer mode is rejected unless `WC_TRUST_STATIC_BUFFER_ALIGNMENT=1` is set (which disables the runtime check and relies on the caller/platform guarantees).
- There is intentionally no “size_t-based” runtime fallback, because pointer-to-integer conversions are only reliably supported via `uintptr_t`.

### Invariants and Consistency Guarantees

These invariants are maintained at all times on successful API calls, and are
designed to remain true even after `WC_NOMEM` returns (partial progress allowed):

* `cap` is always a power of two and non-zero after successful open.
* `len == number_of_occupied_slots` (unique entries).
* `tot == sum_of_all_slot_counts` (modulo `size_t` overflow semantics).
* Every stored word string is NUL-terminated and lives in the arena for the
  lifetime of the `wc`.
* After `WC_NOMEM`, the instance remains valid and queryable via:

  * `wc_total`, `wc_unique`, `wc_results`, and cursor iteration.

**Critical guarantee (memory-limit semantics):**

* If a word already exists in the table, incrementing its count does not
  require allocation and therefore continues to succeed even after the instance
  has exhausted memory/capacity for inserting *new unique* words.

Practically:

* `wc_add(w, "existing")` can still return `WC_OK` after previous inserts returned `WC_NOMEM`,
  as long as `"existing"` was already present.
* `wc_scan()` may return `WC_NOMEM` when it encounters a *new unique* word that
  cannot be inserted. Any words processed before that failure are retained.

### Error Model

All public functions follow a clear error protocol:

* Functions returning `int`:

  * `WC_OK` (0) – success
  * `WC_ERROR` (1) – invalid arguments or internal consistency failure
  * `WC_NOMEM` (2) – allocation failed or memory/capacity limit reached
* Query functions returning `size_t`:

  * Return 0 when passed `NULL`
* `wc_results`:

  * Returns `WC_OK`, `WC_ERROR`, or `WC_NOMEM`
  * On `WC_OK` with no entries: `*out == NULL`, `*n == 0`
* `wc_errstr` converts any result code into a human-readable static string.
* The header also defines `enum wc_rc` as a convenience alias for these result codes.
* When `WC_HAVE_ERRNO != 0`, APIs may set `errno` on failure as a diagnostic.
  Only the return code (and `err_out` where present) is the stable contract.

### Thread Safety

* Separate `wc` instances **may be used concurrently** from multiple threads.
* A single `wc` instance **MUST NOT** be accessed concurrently without external
  synchronization.
* The library does not use global mutable state and does not perform its own
  locking.

---

## API Reference

The API is defined in `wordcount.h`. This section summarizes the public surface.

### Types

```c
typedef struct wc wc;
```

Opaque handle for a word counter. Must be created via `wc_open` /
`wc_open_ex` and destroyed via `wc_close`.

```c
typedef struct wc_limits {
    size_t        struct_size;
    size_t        max_bytes;
    int           strict_max_bytes;
    size_t        init_cap;
    size_t        block_size;
    void         *static_buf;
    size_t        static_size;
    /* Set to 0 for deterministic behavior (default)
       Set to random value for DoS protection. */
    unsigned long hash_seed;
    size_t        max_probe;
    void         *handle_buf;
    size_t        handle_size;
} wc_limits;
```

Per-instance memory and sizing limits:

* `max_bytes`:

  * Hard cap on total **internal** allocations for this `wc` in dynamic mode.
  * If `strict_max_bytes != 0`, the cap is treated as a **peak** limit: growth
    fails if the transient footprint (old table + new table + arena writes)
    would exceed `max_bytes`.
  * Counts:

    * Hash table (and growth)
    * Arena blocks (dynamic mode only; static mode uses first block only)
    * Optional heap/static scan buffer (if `WC_STACK_BUFFER == 0`)
  * Does **not** count:

    * The `wc` struct itself when allocated outside the internal allocator
      (e.g., via `WC_MALLOC` or placed in `handle_buf`)
    * Arrays returned by `wc_results` and `wc_topn`
    * The `wc_stream` object returned by `wc_stream_open`
  * `0` = unlimited.
* `strict_max_bytes`:

  * `0` (default) allows transient peaks during table growth when the
    **post-grow** footprint fits the budget.
  * `1` enforces `max_bytes` as a hard peak cap (no transient overages during
    growth).
* `init_cap`:

  * Initial hash table capacity (number of slots).
  * `0` = let the library choose a default.
  * Rounded up to a power of two internally and floored by `WC_MIN_INIT_CAP`.
* `block_size`:

  * Arena block size in bytes for the first block.
  * `0` = default, floored by `WC_MIN_BLOCK_SZ`.
* `static_buf`, `static_size`:

  * Optional caller-supplied region used for all **internal** allocations.
  * Enables static-buffer mode (see above).
  * Must be suitably aligned for internal allocation types.
  * Must not be shared with another `wc` instance.
  * When both `static_size` and `max_bytes` are set, the effective budget is
    the minimum of the two.
* `hash_seed`:

  * Optional per-instance hash seed.
  * `0` = deterministic; non-zero = perturbed basis (not cryptographic).

* `max_probe`:

    * Optional upper bound on probe length during open addressing.
    * `0` = no bound (default).
    * The value is the maximum slots examined per insertion/lookup, including
      the initial slot. In dynamic mode, hitting the bound triggers at most one
      grow attempt before failing with `WC_NOMEM`.
    * Note: duplicates still increment even when the bound is hit; the bound
      primarily limits insertion of new unique keys under probe storms.

* `handle_buf`, `handle_size`:

    * Optional caller-provided storage for the `wc` handle itself.
    * When both are non-zero, the library places the `struct wc` there instead
      of calling `WC_MALLOC`.
    * If `handle_buf` aliases `static_buf`, it must start at `static_buf` and
      its footprint (rounded up to internal alignment) counts against the
      static budget.
    * Must be aligned for `struct wc`; use `wc_handle_size()` to query the
      required size.

Always initialize with `wc_limits_init(&lim)` or
`wc_limits lim = WC_LIMITS_INIT();`. `struct_size` follows an
append-only contract: smaller values zero-fill missing tail fields, larger
values have their extra bytes ignored; `struct_size == 0` is invalid.

```c
typedef struct wc_word {
    const char *word;
    size_t      count;
} wc_word;
```

```c
typedef struct wc_build_config {
    size_t        struct_size;
    unsigned long version_number;
    size_t        max_word;
    size_t        min_init_cap;
    size_t        min_block_sz;
    int           stack_buffer; /* 1 = stack, 0 = heap/static */
    size_t        sizeof_wc;
    size_t        sizeof_slot;
    size_t        sizeof_wc_limits;
} wc_build_config;
```

```c
typedef struct wc_cursor {
    const wc *w;
    size_t    index;
} wc_cursor;
```

### Result Codes

```c
#define WC_OK    0
#define WC_ERROR 1
#define WC_NOMEM 2
#define WC_EALIGN 3     /* misaligned static_buf */
#define WC_EBADLIMITS 4 /* invalid/unsatisfiable limits */
#define WC_EBUSY 5      /* conflicting stream/static buffer use */
```

`WC_EALIGN` / `WC_EBADLIMITS` / `WC_EBUSY` are returned by `wc_open_ex` and
streaming APIs when the caller-provided buffers/limits are unusable or already
in use; most other functions return only `WC_OK` / `WC_ERROR` / `WC_NOMEM`.
`wc_scan` and `wc_add*` may return WC_EBUSY when WC_STREAM_REUSE_SCANBUF=1 
and a stream is active.
### Lifecycle Functions

```c
wc *wc_open(size_t max_word);
wc *wc_open_ex(size_t max_word, const wc_limits *limits, int *err_out);
void wc_close(wc *w);
```

### Word Insertion and Scanning
```c
int wc_add(wc *w, const char *WC_RESTRICT word);
int wc_add_n(wc *w, const char *WC_RESTRICT word, size_t len);
int wc_add_norm_n(wc *w, const char *WC_RESTRICT word, size_t len);
int wc_scan(wc *w, const char *WC_RESTRICT text, size_t len);
```

Key semantics:

* `wc_add` is **case-sensitive** and NUL-terminated.
* `wc_add_n` is **case-sensitive** with explicit length; embedded `'\0'`
  terminates the word (prefix is used).
* `wc_add_norm_n` is **case-insensitive** (ASCII lowercase normalization) with
  explicit length; embedded `'\0'` terminates the word.
* `wc_scan` is **case-insensitive** (ASCII lowercase normalization).
* Words are truncated at `max_word` and the hash/equality operate on the stored prefix.

Partial-progress semantics under `WC_NOMEM`:

* `wc_add*`: on `WC_NOMEM`, the operation failed; counts are unchanged for that attempted insert.
* `wc_scan`: on `WC_NOMEM`, the function stops at the first failing insertion.
  Any words fully processed before the failing word remain counted.

### Query and Results Functions
```c
size_t wc_total(const wc *w);
size_t wc_unique(const wc *w);

int wc_results(const wc *w, wc_word **WC_RESTRICT out, size_t *WC_RESTRICT n);
int wc_results_into(const wc *w, wc_word *out, size_t out_cap, size_t *out_n);
void wc_results_free(wc_word *r);

int wc_topn(const wc *w, size_t n,
            wc_word **WC_RESTRICT out, size_t *WC_RESTRICT out_n);
int wc_topn_into(const wc *w, size_t n,
                 wc_word *out, size_t out_cap, size_t *out_n);

int wc_get_stats(const wc *w, wc_stats *out);
int wc_reserve(wc *w, size_t expected_unique, size_t expected_bytes);
```

* `wc_results` / `wc_topn`: allocate via `WC_MALLOC`; caller frees with
  `wc_results_free`. Unavailable when `WC_NO_HEAP=1` (returns `WC_NOMEM`).
* `wc_results_into` / `wc_topn_into`: zero-allocation variants; caller supplies
  storage. Query mode: pass `out==NULL` or `out_cap==0` to retrieve required
  length in `*out_n`.
* `wc_reserve`: preflight capacity for `expected_unique` words and
  `expected_bytes` of arena storage. Returns `WC_OK` if the budget can
  accommodate the request, `WC_NOMEM` otherwise.

### Streaming API
```c
wc_stream *wc_stream_open(wc *w, int *err_out);
size_t wc_stream_size(const wc *w);
wc_stream *wc_stream_open_inplace(wc *w, void *mem, size_t mem_size, int *err_out);

int wc_stream_scan(wc_stream *s, const char *WC_RESTRICT buf, size_t len);
int wc_stream_scan_ex(wc_stream *s, const char *WC_RESTRICT buf, size_t len,
                      size_t *WC_RESTRICT consumed_out);

int wc_stream_finish(wc_stream *s);
void wc_stream_close(wc_stream *s);
```

Streaming matches `wc_scan` for tokenization, normalization, and truncation.
On error, `wc_stream_scan_ex` reports partial consumption via `consumed_out`
and discards the failing word to allow forward progress.

### Zero-Allocation Iterator API

```c
void wc_cursor_init(wc_cursor *c, const wc *w);

int wc_cursor_next(wc_cursor *c,
                   const char **WC_RESTRICT word,
                   size_t *WC_RESTRICT count);
```

Iteration order is implementation-defined (hash table order) and not sorted.

### Utility Functions

```c
const char *wc_errstr(int rc);
const char *wc_version(void);
const wc_build_config *wc_build_info(void);
```

---

## Word Detection and Normalization

By default (`WC_ASCII_ONLY=1`), a **word** is a maximal run of ASCII letters:
`A–Z` and `a–z`.

* All other bytes are separators (digits, punctuation, whitespace, non-ASCII).
* `wc_scan` lowercases ASCII letters using `c + ('a' - 'A')`.

When built with `-DWC_ASCII_ONLY=0`, tokenization switches to
`isalnum` + apostrophe via `<ctype.h>` and lowercasing uses `tolower`; both
follow the active C locale. The library does not change locale; the CLI forces
`LC_CTYPE=C` in this mode to keep behavior deterministic.

Examples (default build, WC_ASCII_ONLY=1):

- "it's" -> "it", "s"
- "foo-bar" -> "foo", "bar"
- "abc123def" -> "abc", "def"
- UTF-8 "café" -> "caf" (non-ASCII bytes are separators)

Examples (WC_ASCII_ONLY=0, locale/ctype-dependent):

- Word bytes are (isalnum || apostrophe) under the active C locale.
- "it's" -> "it's"   (apostrophe is treated as a word byte)
- "abc123def" -> "abc123def"  (digits are word bytes)
- Bytes >= 128 are classified by `isalnum`/`tolower` under the active locale (often separators in the "C" locale).


---

## Memory Configuration

### Compile-Time Configuration Macros

Define these **before** including `wordcount.h` (and/or when compiling `wordcount.c`).
All `WC_*` feature toggles must be defined as numeric 0/1 in C/C++ source or compiler flags; CMake `ON/OFF` applies only to the CMake cache, not to the C preprocessor.

#### Allocator Overrides

```c
#define WC_MALLOC(n)     my_malloc(n)
#define WC_FREE(p)       my_free(p)
#define WC_REALLOC(p, n) my_realloc(p, n)
```

Used for:

* The `wc` handle itself
* Dynamic-mode internal allocations
* Arrays returned by `wc_results`

#### Stack vs. Heap Scan Buffer

```c
#define WC_STACK_BUFFER 0  /* default is 1 */
```

* When `1` (default): `wc_scan` uses a stack buffer sized at `WC_MAX_WORD`.
* When `0`: `wc_scan` uses a per-instance buffer sized at runtime `max_word`:

  * allocated once per `wc`
  * freed in `wc_close`
  * in static-buffer mode, carved from the static buffer

#### Stream Buffer Reuse
```c
#define WC_STREAM_REUSE_SCANBUF 1  /* default is 0 */
```

* When `1`: `wc_stream` reuses the per-instance scan buffer allocated when
  `WC_STACK_BUFFER=0`, reducing memory footprint.
* **Requires** `WC_STACK_BUFFER=0` (enforced at compile time).
* **Constraint**: only one stream may be active per `wc` instance; concurrent
  `wc_stream_open` or `wc_add*`/`wc_scan` calls return `WC_EBUSY`.

#### Global Limits

```c
#define WC_MAX_WORD     1024u
#define WC_MIN_INIT_CAP 16u
#define WC_MIN_BLOCK_SZ 256u
```

#### Default Initial Sizing

Derived from `WC_SIZE_MAX` (falls back to `~(size_t)0` if `SIZE_MAX` is
unavailable) unless overridden:

* 16-bit `size_t`: `WC_DEFAULT_INIT_CAP=128`, `WC_DEFAULT_BLOCK_SZ=1024`
* 32-bit `size_t`: `WC_DEFAULT_INIT_CAP=1024`, `WC_DEFAULT_BLOCK_SZ=16384`
* 64-bit `size_t`: `WC_DEFAULT_INIT_CAP=4096`, `WC_DEFAULT_BLOCK_SZ=65536`

### Runtime Limits via `wc_limits`

Initialize with `wc_limits_init(&lim)` or `wc_limits lim = WC_LIMITS_INIT();`
to set `struct_size` for forward compatibility. Smaller `struct_size` values
zero-fill missing fields; larger values have their extra tail ignored.
Passing `struct_size==0` is invalid.

Dynamic mode with a 1 MiB internal budget:

```c
wc_limits lim = WC_LIMITS_INIT();
lim.max_bytes = 1 * 1024 * 1024;

wc *w = wc_open_ex(0, &lim, NULL);
```

Static-buffer mode (use `WC_STATIC_BUFFER` for portable alignment):

```c
WC_STATIC_BUFFER(pool, 2048);

wc_limits lim = WC_LIMITS_INIT();
lim.static_buf  = pool.buf;
lim.static_size = sizeof pool.buf;

wc *w = wc_open_ex(32, &lim, NULL);
```

`wc_reserve` preflights against the effective remaining allocator budget:
in static mode it honors both `static_size` and `max_bytes` and includes the
next-allocation alignment padding, so a successful reserve will not
immediately hit `WC_NOMEM` on the next request for the promised size.

Randomized hashing for untrusted inputs (not cryptographic):

```c
#include <time.h>

wc_limits lim = WC_LIMITS_INIT();
lim.hash_seed = (unsigned long)time(NULL);

wc *w = wc_open_ex(0, &lim, NULL);
```

---

## Building and Integration

### Using CMake (Presets, Recommended)

Common workflow:

```bash
cmake --preset clang
cmake --build --preset clang
ctest --preset clang
```

Available configure presets (see `CMakePresets.json`):

* `clang` – Clang (Debug + ASan/UBSan)
* `clang-stronghash` – Clang (ASan/UBSan + `WC_HASH_STRONG=ON`)
* `gcc` – GCC (Debug + ASan/UBSan)
* `compcert` – CompCert (ccomp; no sanitizers)
* `mingw` – MinGW-w64 cross (static exe)

Sanitizers are applied to executables and shared/module libraries on native
GCC/Clang builds (not MinGW/MSVC/CompCert).

Notable CMake options:

* `WC_ENABLE_VALIDATE` – enable `wc_validate` invariant walk (debug/fuzz builds).
* `WC_HASH_STRONG` – build with SipHash-2-4 instead of FNV-1a.
* `WC_BUILD_VARIANTS` – build heap/tiny library variants (same symbols; off by
  default for installs, enabled automatically when `BUILD_TESTING` is on).
  Install targets include only built variants.

### Build Profiles

Hosted (default): no extra defines needed.

Freestanding / exotic toolchains:

* `-DWC_STDC_HOSTED=0`
* `-DWC_USE_LIBC_STRING=0`
* `-DWC_USE_LIBC_QSORT=0`
* `-DWC_HAVE_ERRNO=0`
* `-DWC_NO_HEAP=1`
* `-DWC_U32_T=your_uint32_type`
* `-DWC_U64_T=your_uint64_type` (only needed with `WC_HASH_STRONG=1`)

Tiny RAM profile:

* `-DWC_STACK_BUFFER=0` (required for stream buffer reuse)
* `-DWC_STREAM_REUSE_SCANBUF=1`
* `-DWC_MAX_WORD=16`

### One-shot Build & Quality Script

```bash
./c-build.sh
```

This script:

1. Builds + tests:

   * `clang`
   * `gcc`
2. Optionally, if available:

   * `compcert` (if `ccomp` exists)
   * `mingw` (if `x86_64-w64-mingw32-gcc` exists; tests skipped by default)
3. Runs `./c-quality.sh`:

   * `clang-format` (in-place)
   * `clang-tidy` (uses `compile_commands.json` when present)
   * `cppcheck`

### Direct Compilation

```bash
cc -std=c99 -O2 -DWC_HASH_STRONG=1 wordcount.c your_program.c -o your_program
```

CLI:

```bash
cc -std=c99 -O2 wordcount.c wc_main.c -o wc
```

Heap/static scan buffer:

```bash
cc -std=c99 -O2 -DWC_STACK_BUFFER=0 wordcount.c your_program.c -o your_program
```

### Continuous Integration

GitHub Actions builds run on Linux (clang/gcc with sanitizers + fuzz smoke via clang),
macOS (clang preset), and Windows (MSVC). Presets keep options consistent, and
`BUILD_TESTING` enables the heap/tiny variants for coverage without installing them by default.

### Directory Layout

```text
.
├── CMakeLists.txt
├── CMakePresets.json
├── c-build.sh
├── c-quality.sh
├── mingw-w64.cmake
├── wordcount.h
├── wordcount.c
├── wc_main.c
├── wc_test.c
├── LICENSE
└── README.md
```

### Streaming inputs

For chunked sources (stdin, sockets, etc.), use the streaming API:

- `wc_stream_open` / `wc_stream_scan` / `wc_stream_finish` / `wc_stream_close`
  (tokenization/normalization match `wc_scan`; streaming may report partial
  consumption on error and discards the failing word to allow forward progress)

Streaming matches `wc_scan` for tokenization, normalization, and truncation but
intentionally differs in error handling: on errors it may report partial
consumption and skip the failing word to keep forward progress. The CLI uses
this API for stdin to avoid drift from the library’s normalization rules.

---

## CLI Tool (`wc`)

Usage:

```bash
wc [OPTIONS] [FILE|-]
```

Key options:

* Output: `-n/--top N` (default 25), `--all`, `--format table|tsv|json`,
  `--summary`, `-q/--quiet`
* Filters: `--min-len N`, `--max-len N`, `--max-word N`
* Memory: `--max-bytes N`, `--strict-max-bytes`, `--no-mmap`
* Presentation: `--color auto|always|never`, `--no-color`
* Misc: `--version`, `-h/--help`

Output formats:

* `table` (default): width-aware columns (rank, count, word) with optional color
  headers and a summary footer.
* `tsv`: header row + rows (`rank<TAB>count<TAB>word`); summary prints to stderr.
* `json`: object containing `words` (array of objects with `rank`, `count`,
  `word`) and `summary` (`total`, `unique`, `filtered`, `displayed`, `bytes`).

Behavior:

* No args: read stdin (streaming).
* With files: memory-map each file and aggregate counts (use `--no-mmap` to
  stream).
* `--top N` / `--all` operate on fully sorted results (count desc, then
  lex asc).

### Environment-Based Limits

`WC_MAX_BYTES` or `--max-bytes` sets an internal allocation cap:

```bash
WC_MAX_BYTES=8388608 ./wc largefile.txt
```

### Output

Default: width-aware table of the top 25 words plus a summary footer (stdout).
TSV emits a header + rows; JSON emits both `words` and `summary`. If no words
pass filters, a short notice is printed to stderr.

Exit codes:

* `0` if all inputs processed successfully (even if no words found)
* `1` for runtime failures (I/O, allocation, parse failures)
* `2` for usage/argument errors
* On Windows, file paths are treated as UTF-8 consistently for mmap and
  streaming (`--no-mmap`).

---

## Testing, Fuzzing, and OOM Injection

Run tests via presets:

```bash
cmake --preset clang
cmake --build --preset clang
ctest --preset clang
```

OOM injection (glibc only):

```bash
cc -std=c99 -O0 -g -DWC_TEST_OOM wordcount.c wc_test.c -o wc_test_oom
./wc_test_oom
```

Fuzzing (libFuzzer):

```bash
clang -std=c99 -O1 -g -fsanitize=address,undefined,fuzzer \
  -DWC_TEST_FUZZ wordcount.c wc_test.c -o wc_fuzz
```

---

## Portability and Platform Assumptions

* C99 core (hosted environment).
* Requires `CHAR_BIT == 8` and ASCII-compatible execution character set.
* Default build is locale-independent. With `WC_ASCII_ONLY=0`, word detection
  follows the active C locale (the CLI sets `LC_CTYPE=C` to remain stable).

---

## Adversarial Inputs and Complexity

* `wc_scan` accepts arbitrary byte sequences (including embedded NULs); `wc_add_n`
  and `wc_add_norm_n` truncate at the first embedded `'\0'` to keep stored words
  valid C strings for sorting/comparison.
* Not cryptographic / not DoS-hard hashing.
* For untrusted inputs:

  * consider non-zero `hash_seed`
  * enforce request-level CPU/memory limits outside the library

### Hardening Guide

- Enable keyed hashing: set `wc_limits.hash_seed` or compile with `-DWC_HASH_STRONG=1`
  (or, when using CMake, configure with `-DWC_HASH_STRONG=ON`).
- Bound memory: use `wc_limits.max_bytes` (or `WC_MAX_BYTES`/`--max-bytes`) and `wc_reserve` for known workloads.
- Clamp probe storms: set `wc_limits.max_probe` when linear probing must be bounded; the value is the maximum slots examined per operation (including the initial slot). Exhaustion allows one grow attempt in dynamic mode, then returns `WC_NOMEM` (duplicates still increment even when the cap is hit).
- `wc_stream_open()` allocates a small `wc_stream` object via `WC_MALLOC` and returns a library-owned handle; close it with `wc_stream_close()`.
- `wc_stream_open_inplace()` uses caller-provided storage (size via `wc_stream_size()`), performs no allocations, and is suitable for strict no-heap deployments.
- Stream objects are not counted against `wc_limits.max_bytes` because `max_bytes` applies only to internal allocations tracked by the wc allocator state, not to caller/lifetime-owned API buffers.
- Use `wc_validate` (build with `-DWC_ENABLE_VALIDATE`) in debug or fuzz builds to catch corruption early.

---

## ABI and Build Configuration Compatibility

`wc_build_info()` exposes build-time parameters of the compiled library binary
so callers can detect header/library mismatches in complex deployments.

---

## Performance and Complexity

* `wc_scan`: O(n) over input bytes
* `wc_add`: amortized O(1)
* `wc_results`: O(U log U) for U unique words
* memory: O(U) table + O(total stored string bytes)

---

## Versioning and Stability Guarantees

Version macros are defined in `wordcount.h`:

```c
#define WC_VERSION        "5.0.0"
#define WC_VERSION_NUMBER 5000000UL
```

* Major: may introduce breaking changes
* Minor/Patch: stable signatures; internal improvements and new additions only

### ABI and extensible configuration structs

- `wc_limits` is the canonical, struct_size-extensible limits struct.
  `wc_limits_init` sets `struct_size`; smaller values zero-fill missing
  fields, larger values have extra bytes ignored.
- `wc_build_config` is similarly struct_size-extensible. `wc_build_info()`
  returns the canonical struct with sizes of key internal types to detect
  mismatches across translation units.

---

## Guidelines for Contributors

1. Preserve C99, small dependencies
2. Overflow-check all size math
3. Keep OOM behavior defined and testable
4. Update tests for new behavior (including constrained configs)
5. Run `./c-quality.sh`

---

## License

Public domain / Unlicense. See `LICENSE`.

---

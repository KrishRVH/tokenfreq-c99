# wordcount Specification

This document is the normative reference for the `wordcount` library API and
behavior.

## Environment and Portability

- Language: C99. No extensions are required for the public API.
- Default build assumes a hosted libc; freestanding/exotic targets are supported
  via the configuration macros listed below.
- Execution character set: ASCII-compatible with `CHAR_BIT == 8`.
- Threading: no global mutable state. Separate `wc` instances are independent;
  a single `wc`/`wc_stream` must not be used from multiple threads without
  external synchronization.
- Determinism: given the same inputs, limits, and build-time defines, all
  outputs (including ordering) are stable across architectures and compilers.

### Portability Macros (single source of truth)

| Macro | Default | Effect / Notes |
| --- | --- | --- |
| `WC_STDC_HOSTED` | `__STDC_HOSTED__` or `1` | Advertises hosted vs freestanding. |
| `WC_USE_LIBC_STRING` | `WC_STDC_HOSTED` | Controls use of libc `strcmp`/`mem*`; internal fallbacks otherwise. |
| `WC_USE_LIBC_QSORT` | `WC_STDC_HOSTED` | Controls use of libc `qsort`; heapsort fallback otherwise. |
| `WC_HAVE_ERRNO` | `WC_STDC_HOSTED` | If `0`, errno is never read or written. |
| `WC_NO_HEAP` | `0` | If `1`, library never calls `malloc`/`realloc`/`free`; static buffer is required. |
| `WC_TRUST_STATIC_BUFFER_ALIGNMENT` | `0` | If `1`, alignment of caller-provided buffers is trusted when `uintptr_t` is unavailable. |
| `WC_STREAM_REUSE_SCANBUF` | `0` | If `1`, a `wc` can host only one active stream and reuses `scanbuf` for it. |
| `WC_ASCII_ONLY` | `1` | If `0`, ctype is used (with unsigned char casts) for word detection. |
| `WC_HASH_STRONG` | `0` | If `1`, use SipHash-2-4 (`wc_u64` hookable); else FNV-1a (`wc_u32`). |
| `WC_STACK_BUFFER` | `1` | If `0`, scan buffers are heap/static allocated and counted against budgets. |

`WC_U32_T` / `WC_U64_T` may be defined to override `<stdint.h>` types on
toolchains without `<stdint.h>`.

## Tokenization and Normalization

- Word definition: by default (`WC_ASCII_ONLY=1`), a maximal run of ASCII
  letters `[A-Za-z]` when scanning via `wc_scan`/`wc_stream_*`. All other bytes
  are treated as separators. With `WC_ASCII_ONLY=0`, classification uses
  `isalnum` + apostrophe via `<ctype.h>` and follows the active C locale.
- Case folding: default build lowercases ASCII letters; with `WC_ASCII_ONLY=0`
  normalization uses `tolower` from `<ctype.h>` and is locale-dependent.
  `wc_add` and `wc_add_n` are byte-wise and case-sensitive; `wc_add_norm_n`
  lowercases ASCII letters in the provided range.
- Locale: the library does not change locale; the CLI forces `LC_CTYPE=C` when
  built with `WC_ASCII_ONLY=0` to keep tokenization deterministic.
- Maximum word length: runtime `max_word` is clamped into `[4, WC_MAX_WORD]`.
  Bytes beyond `max_word` are truncated (still counted); truncation never
  raises an error.

## Memory Model and Budgeting

- Counted toward `max_bytes` (when non-zero):
  - Hash table allocations and rehash growth.
  - Arena blocks for stored strings.
  - Scan buffer when `WC_STACK_BUFFER == 0`.
  - Alignment padding for any counted region.
- Not counted:
  - Caller-owned result arrays from `wc_results*`.
  - `wc_stream` objects (self-owned or inplace).
  - The `wc` handle itself unless it lives inside the static buffer.
- Modes:
  - **Steady-state (default)**: growth may transiently exceed `max_bytes` when
    rehashing if the post-grow table fits the limit.
  - **Strict (`strict_max_bytes=1`)**: allocations must fit without any
    transient excess; growth paths fail closed with `WC_NOMEM`.
- Static buffer mode:
  - `wc_limits.static_buf/static_size` provide storage for all counted
    allocations. The buffer must be aligned to `WC_ALIGN`.
  - If `uintptr_t` is unavailable, alignment is rejected unless
    `WC_TRUST_STATIC_BUFFER_ALIGNMENT` is `1` (caller guarantees correctness).
  - Layout (conceptual): optional handle prefix, then hash table, arena blocks,
    and (when `WC_STACK_BUFFER==0`) the scan buffer, each aligned to `WC_ALIGN`.
  - If the `wc` handle is placed inside the static buffer
    (`wc_limits.handle_buf/handle_size`), it must start at `static_buf` and the
    footprint is charged against the static budget.
  - Initialization fails with `WC_EBADLIMITS` or `WC_EALIGN` if the buffer is
    too small, misaligned, or inconsistent; no partial initialization occurs.
- `WC_NO_HEAP=1` forbids all heap allocations: static buffer + handle storage
  are required and streaming with heap-backed buffers is unavailable.

## Sorting and Ordering

- `wc_results` / `wc_topn` return results sorted by count descending, then
  byte-wise ascending word order (via `WC_STRCMP`).
- `wc_topn` is a prefix of the full sorted results for the requested size.

## Streaming Semantics

- At most one active stream when `WC_STREAM_REUSE_SCANBUF=1`; additional opens
  or simultaneous `wc_scan` return `WC_EBUSY`.
- State machine:
  - Start → `wc_stream_scan[_ex]` (any number of calls) → `wc_stream_finish`
    (flushes trailing word, idempotent, records final status) → `wc_stream_close`.
  - Calling `wc_stream_scan[_ex]` after `wc_stream_finish` returns the same
    terminal code as `wc_stream_finish` and leaves `consumed_out` at `0`.
  - Closing is always allowed; in reuse mode it releases the scan buffer for
    other operations.

## API Surface and Contracts

Public functions (all declared `WC_API`):

```
wc_open_ex, wc_open, wc_close, wc_handle_size,
wc_add, wc_add_n, wc_add_norm_n, wc_scan,
wc_total, wc_unique, wc_results, wc_results_into,
wc_topn, wc_topn_into, wc_results_free,
wc_cursor_init, wc_cursor_next,
wc_errstr, wc_version, wc_build_info,
wc_get_stats, wc_validate,
wc_stream_open, wc_stream_size, wc_stream_open_inplace,
wc_stream_scan_ex, wc_stream_scan, wc_stream_finish, wc_stream_close.
```

Conventions:

- int-returning APIs use `WC_OK` for success and documented non-zero codes on
  error. Pointer-returning APIs return `NULL` on error.
- `err_out` (when provided) is written on success and failure. `wc_errstr`
  produces stable human-readable strings for any code.
- `errno` is only written on failure when `WC_HAVE_ERRNO != 0`; success paths
  never clobber it.
- `wc_limits.struct_size` must be set; fields beyond `struct_size` are treated
  as zero/NULL. Inconsistent limit combinations fail with `WC_EBADLIMITS`.

## Ownership, Lifetimes, and Cursors

- `wc_word.word` pointers returned by results APIs are owned by the `wc`
  instance and become invalid after `wc_close`.
- Arrays produced by `wc_results` / `wc_topn*` are caller-owned and must be
  freed with `wc_results_free`.
- Cursor iteration does not invalidate results; inserting new words while a
  cursor is active is not supported.

## Error Codes

- `WC_OK`: success
- `WC_ERROR`: invalid argument or corrupted handle
- `WC_NOMEM`: allocation failed or budget exceeded
- `WC_EALIGN`: alignment failure for caller-provided buffers
- `WC_EBADLIMITS`: inconsistent or undersized limits
- `WC_EBUSY`: conflicting use of shared scan buffer/stream

`wc_errstr` maps each code to a stable lowercase string.

## Freestanding Build Profiles (examples)

- Hosted default: no defines needed.
- Libc-free: `-DWC_STDC_HOSTED=0 -DWC_USE_LIBC_STRING=0 -DWC_USE_LIBC_QSORT=0 -DWC_HAVE_ERRNO=0`.
- Heapless: `-DWC_NO_HEAP=1 -DWC_STACK_BUFFER=0` with static buffer supplied.
- Tiny RAM streaming: `-DWC_STACK_BUFFER=0 -DWC_STREAM_REUSE_SCANBUF=1` with a
  small `WC_MAX_WORD`.

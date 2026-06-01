# wordcount Specification

This document is the normative reference for the `wordcount` library API and
behavior.

## Environment and Portability

- Language: C99. The public API requires no compiler extensions.
- Default build assumes a hosted C environment. Freestanding and exotic targets
  are supported by the documented configuration macros.
- Required platform properties:
  - `CHAR_BIT == 8`
  - ASCII-compatible execution character set
  - an unsigned 32-bit type available through `WC_U32_T`
- Threading: the library has no global mutable state. Separate `wc` instances
  may be used concurrently; a single `wc` or `wc_stream` requires external
  synchronization.
- Determinism: sorted result APIs are stable for the same inputs, limits, and
  build-time defines. CLI row ordering and machine formats are stable for the
  same options; presentation details such as automatic color depend on CLI
  options and TTY detection.
  Cursor iteration is hash-table order and intentionally unspecified.

## Portability Macros

| Macro | Default | Effect |
| --- | --- | --- |
| `WC_STDC_HOSTED` | `__STDC_HOSTED__` or `1` | Hosted vs. freestanding profile. |
| `WC_USE_LIBC_STRING` | `WC_STDC_HOSTED` | Use libc `strcmp`/`mem*`; otherwise use internal fallbacks. |
| `WC_USE_LIBC_QSORT` | `WC_STDC_HOSTED` | Use libc `qsort`; otherwise use internal heapsort. |
| `WC_HAVE_ERRNO` | `WC_STDC_HOSTED` | If `0`, `errno` is never read or written. |
| `WC_NO_HEAP` | `0` | If `1`, the library never calls heap allocation macros. |
| `WC_HAVE_UINTPTR` | auto | If `1`, static-buffer alignment checks use `uintptr_t`; if `0`, static-buffer mode fails closed unless explicit static-buffer alignment trust mode applies. |
| `WC_TRUST_STATIC_BUFFER_ALIGNMENT` | `0` | If `1`, static-buffer alignment is a caller precondition when `uintptr_t` is unavailable. |
| `WC_HASH_STRONG` | `0` | If `1`, use SipHash-2-4; otherwise use FNV-1a. |
| `WC_STACK_BUFFER` | `1` | If `0`, scan buffers are allocated from heap/static storage and counted. |

`WC_U32_T` and `WC_U64_T` may override the internal hash integer types.
Toolchains without usable `<stdint.h>` or `PTRDIFF_MAX` must define
`WC_U32_T`, `WC_PTRDIFF_MAX`, and `WC_HAVE_UINTPTR=0`; strong-hash builds
without `<stdint.h>` must also define `WC_U64_T`.

## Tokenization and Normalization

- Word definition: a maximal run of ASCII letters `[A-Za-z]`. All other bytes
  are separators.
- `wc_scan` and the streaming APIs lowercase accepted word bytes.
- `wc_add` and `wc_add_n` are byte-wise and case-sensitive.
- `wc_add_norm_n` folds the supplied prefix but does not tokenize it.
- Length-based add APIs stop at the first embedded `'\0'` so stored words remain
  valid C strings.
- Runtime `max_word` is clamped into `[4, WC_MAX_WORD]`; longer words are
  truncated without error.

## Memory Model and Budgeting

Counted toward `wc_limits.max_bytes` when non-zero:

- hash table storage and rehash growth
- arena blocks for stored strings
- scan buffer when `WC_STACK_BUFFER=0`
- alignment padding for counted regions

Not counted:

- arrays returned by `wc_results` and `wc_topn`
- `wc_stream` objects from `wc_stream_open`
- the `wc` handle itself in heap-enabled builds. In heapless mode, the aligned
  handle footprint is charged to the static budget.

Modes:

- Default steady-state mode may transiently exceed `max_bytes` during table
  growth when the post-grow footprint fits.
- `strict_max_bytes=1` enforces `max_bytes` as a peak cap; growth fails closed
  with `WC_NOMEM` instead of exceeding the cap.
- `WC_NO_HEAP=1` forbids heap allocation. `static_buf` must provide room for
  the handle and all internal storage; the handle is placed at `static_buf`.

Static-buffer contracts:

- `static_buf/static_size` provide all counted internal storage.
- Buffers must be aligned to the library internal alignment.
- Without `uintptr_t`, static-buffer mode is rejected unless
  `WC_TRUST_STATIC_BUFFER_ALIGNMENT=1`.
- Static layout preflight occurs before writing caller-provided static storage.

## Results and Ordering

- `wc_results` and `wc_topn` return arrays sorted by count descending, then
  byte-wise ascending word order.
- `wc_topn(w, n)` is a prefix of the full sorted result for that `n`.
- `wc_cursor` is zero-allocation enumeration. Its order is unspecified.
- In `WC_NO_HEAP=1` builds, `wc_results` and `wc_topn` return `WC_NOMEM` for
  valid result-output calls. `wc_stream_open` returns `NULL` and writes
  `WC_NOMEM` to `err_out` when provided. Use `wc_cursor` and `wc_scan`.

## Streaming Semantics

- `wc_stream_scan_ex` matches `wc_scan` tokenization, normalization, and
  truncation across chunk boundaries.
- `wc_stream_finish` flushes the trailing in-progress word. On `WC_NOMEM`, that
  word remains buffered and the stream is not finished, so callers may retry.
  Other return codes are recorded as terminal status and returned
  idempotently.
- `wc_stream_close` does not flush a trailing word; call `wc_stream_finish`
  first when the trailing word should count.
- After a terminal `wc_stream_finish` result, `wc_stream_scan_ex` returns the
  saved finish status and leaves `consumed_out` at `0`.
- If insertion of a buffered word fails while `wc_stream_scan_ex` is scanning a
  separator, that word is discarded, the separator is consumed, and the stream
  remains valid for forward progress. `WC_ERROR` from invalid arguments,
  detached streams, or oversized chunks does not provide forward progress.
- If a parent `wc` is closed before an open stream, the stream is detached:
  later scan/finish calls return `WC_ERROR`, and `wc_stream_close` remains safe.
## Public API

Public functions:

```text
wc_open_ex, wc_open, wc_close,
wc_add, wc_add_n, wc_add_norm_n, wc_scan,
wc_total, wc_unique, wc_results, wc_topn, wc_results_free,
wc_cursor_init, wc_cursor_next,
wc_errstr, wc_version, wc_build_info,
wc_get_stats, wc_reserve, wc_validate,
wc_stream_open,
wc_stream_scan_ex, wc_stream_finish, wc_stream_close
```

Conventions:

- Status-returning APIs return `WC_OK` on success and a documented non-zero
  code on failure. `wc_cursor_next` is a boolean iterator: it returns `1` when
  it produces an item and `0` at end or for an invalid cursor.
- Pointer-returning APIs return `NULL` on failure.
- `err_out`, when provided, is written on success and failure.
- `errno` is written only on failure when `WC_HAVE_ERRNO!=0`; return codes are
  the stable contract.
- `wc_limits.struct_size` must be at least `sizeof(size_t)`. Fields beyond
  `struct_size` are treated as zero/NULL; extra tail bytes are ignored.
- `wc_reserve` is best-effort preflight/growth only. Future inserts/scans may
  still return `WC_NOMEM`.
- Count increments fail with `WC_NOMEM` instead of wrapping at `SIZE_MAX`.

## Ownership and Lifetimes

- `wc` handles are created by `wc_open`/`wc_open_ex` and destroyed by
  `wc_close`.
- `wc_word.word` pointers returned by result APIs are owned by the `wc` instance
  and become invalid after `wc_close`.
- Arrays returned by `wc_results`/`wc_topn` are caller-owned and must be freed
  with `wc_results_free`.
- Cursor iteration requires the parent `wc` to remain valid. Mutating the
  counter while a cursor is active is unsupported.
- Caller-provided static buffers must not be modified, reused, or released while
  the library object using them is active.

## Error Codes

- `WC_OK`: success
- `WC_ERROR`: invalid argument or corrupted handle
- `WC_NOMEM`: allocation failed, a budget/capacity limit was reached, or a
  counter would overflow
- `WC_EALIGN`: caller-provided buffer alignment failure
- `WC_EBADLIMITS`: inconsistent or undersized caller limits

`wc_errstr` maps every code to a stable lowercase string.

## Build Profiles

- Hosted default: no extra defines.
- Libc-light: `-DWC_STDC_HOSTED=0 -DWC_USE_LIBC_STRING=0
  -DWC_USE_LIBC_QSORT=0 -DWC_HAVE_ERRNO=0 -DWC_OMIT_ASSERT=1`. This profile is
  still heap-enabled and requires `<stdlib.h>` allocator support or custom
  `WC_MALLOC`/`WC_FREE` definitions.
- Libc-free heapless: `-DWC_NO_HEAP=1 -DWC_STACK_BUFFER=0
  -DWC_STDC_HOSTED=0 -DWC_USE_LIBC_STRING=0 -DWC_USE_LIBC_QSORT=0
  -DWC_HAVE_ERRNO=0 -DWC_OMIT_ASSERT=1` with caller-supplied static storage.
- Tiny RAM: `-DWC_STACK_BUFFER=0` with a reduced `WC_MAX_WORD` if desired.
  This reduces stack and arena pressure but does not imply suitability for
  sub-100-byte RAM parts; validate the target compiler/linker map.

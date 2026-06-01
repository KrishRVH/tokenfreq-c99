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
- Determinism: given the same inputs, limits, build-time defines, and active C
  locale when `WC_ASCII_ONLY=0`, sorted result APIs and CLI output are stable.
  Cursor iteration is hash-table order and intentionally unspecified.

## Portability Macros

| Macro | Default | Effect |
| --- | --- | --- |
| `WC_STDC_HOSTED` | `__STDC_HOSTED__` or `1` | Hosted vs. freestanding profile. |
| `WC_USE_LIBC_STRING` | `WC_STDC_HOSTED` | Use libc `strcmp`/`mem*`; otherwise use internal fallbacks. |
| `WC_USE_LIBC_QSORT` | `WC_STDC_HOSTED` | Use libc `qsort`; otherwise use internal heapsort. |
| `WC_HAVE_ERRNO` | `WC_STDC_HOSTED` | If `0`, `errno` is never read or written. |
| `WC_NO_HEAP` | `0` | If `1`, the library never calls heap allocation macros. |
| `WC_HAVE_UINTPTR` | auto | If `1`, pointer alignment/overlap checks use `uintptr_t`; if `0`, ambiguous checks fail closed unless explicit static-buffer alignment trust mode applies. |
| `WC_TRUST_STATIC_BUFFER_ALIGNMENT` | `0` | If `1`, static/handle buffer alignment is a caller precondition when `uintptr_t` is unavailable. It does not enable in-place streams without `uintptr_t`. |
| `WC_STREAM_REUSE_SCANBUF` | `0` | If `1`, one active stream may reuse the instance scan buffer. |
| `WC_ASCII_ONLY` | `1` | If `0`, `<ctype.h>` classification is used. |
| `WC_HASH_STRONG` | `0` | If `1`, use SipHash-2-4; otherwise use FNV-1a. |
| `WC_STACK_BUFFER` | `1` | If `0`, scan buffers are allocated from heap/static storage and counted. |

`WC_U32_T` and `WC_U64_T` may override the internal hash integer types.
Toolchains without usable `<stdint.h>` or `PTRDIFF_MAX` must define
`WC_PTRDIFF_MAX` and set `WC_HAVE_UINTPTR=0`.

## Tokenization and Normalization

- Default word definition (`WC_ASCII_ONLY=1`): a maximal run of ASCII letters
  `[A-Za-z]`. All other bytes are separators.
- `WC_ASCII_ONLY=0`: word classification uses `isalnum((unsigned char)c)` plus
  apostrophe under the active C locale.
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
- caller-owned in-place stream storage
- the `wc` handle itself when it is allocated via `WC_MALLOC` or in separate
  caller-provided `handle_buf` storage. When `handle_buf` is exactly
  `static_buf`, or when heapless mode omits `handle_buf`, the aligned handle
  footprint is charged to the static budget.

Modes:

- Default steady-state mode may transiently exceed `max_bytes` during table
  growth when the post-grow footprint fits.
- `strict_max_bytes=1` enforces `max_bytes` as a peak cap; growth fails closed
  with `WC_NOMEM` instead of exceeding the cap.
- `WC_NO_HEAP=1` forbids heap allocation. `static_buf` must provide room for
  the handle and all internal storage. If `handle_buf` is omitted, the handle is
  placed at `static_buf`.

Static-buffer contracts:

- `static_buf/static_size` provide all counted internal storage.
- Buffers must be aligned to the library internal alignment.
- Without `uintptr_t`, static-buffer mode is rejected unless
  `WC_TRUST_STATIC_BUFFER_ALIGNMENT=1`.
- If `handle_buf` shares `static_buf`, it must be exactly equal to
  `static_buf`; the aligned handle footprint is charged to the static budget.
- Partial handle/static overlap is invalid. Separate non-overlapping buffers are
  accepted only when pointer-range checks can prove non-overlap.
- Static layout preflight occurs before writing caller-provided handle/storage.

## Results and Ordering

- `wc_results` and `wc_topn` return arrays sorted by count descending, then
  byte-wise ascending word order.
- `wc_topn(w, n)` is a prefix of the full sorted result for that `n`.
- `wc_cursor` is zero-allocation enumeration. Its order is unspecified.
- In `WC_NO_HEAP=1` builds, `wc_results` and `wc_topn` return `WC_NOMEM` for
  valid result-output calls. `wc_stream_open` returns `NULL` and writes
  `WC_NOMEM` to `err_out` when provided. Use `wc_cursor`; use
  `wc_stream_open_inplace` only when `WC_HAVE_UINTPTR=1`.

## Streaming Semantics

- `wc_stream_scan[_ex]` matches `wc_scan` tokenization, normalization, and
  truncation across chunk boundaries.
- `wc_stream_finish` flushes the trailing in-progress word, records the terminal
  status, and is idempotent.
- `wc_stream_close` does not flush a trailing word; call `wc_stream_finish`
  first when the trailing word should count.
- After `wc_stream_finish`, scan calls return the saved finish status and leave
  `consumed_out` at `0`.
- If insertion of a buffered word fails while `wc_stream_scan_ex` is scanning a
  separator, that word is discarded, the separator is consumed, and the stream
  remains valid for forward progress. `WC_ERROR` from invalid arguments,
  detached streams, or oversized chunks does not provide forward progress.
- If a parent `wc` is closed before an open stream, the stream is detached:
  later scan/finish calls return `WC_ERROR`, and `wc_stream_close` remains safe.
- In-place stream storage is caller-owned. It must remain valid for every call
  that uses the stream pointer, including `wc_stream_close`. If the parent `wc`
  is closed first, the library holds no further references to that storage;
  callers may reuse it only if they will not call stream APIs with that pointer
  again.
- In-place stream storage must not overlap the parent `wc`, its static buffer,
  internal allocations, or active streams. Builds without `uintptr_t` always
  reject in-place stream open with `WC_EBADLIMITS`, because non-overlap cannot be
  verified.
- With `WC_STREAM_REUSE_SCANBUF=1`, at most one stream may be active per `wc`;
  conflicting opens or simultaneous `wc_scan`/`wc_add*` calls return `WC_EBUSY`.

## Public API

Public functions:

```text
wc_open_ex, wc_open, wc_close, wc_handle_size,
wc_add, wc_add_n, wc_add_norm_n, wc_scan,
wc_total, wc_unique, wc_results, wc_topn, wc_results_free,
wc_cursor_init, wc_cursor_next,
wc_errstr, wc_version, wc_build_info,
wc_get_stats, wc_reserve, wc_validate,
wc_stream_open, wc_stream_size, wc_stream_open_inplace,
wc_stream_scan_ex, wc_stream_scan, wc_stream_finish, wc_stream_close
```

Conventions:

- Integer-returning APIs return `WC_OK` on success and a documented non-zero
  code on failure.
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
- Caller-provided static buffers and in-place stream buffers must not be
  modified, reused, or released while the library object using them is active.

## Error Codes

- `WC_OK`: success
- `WC_ERROR`: invalid argument or corrupted handle
- `WC_NOMEM`: allocation failed, a budget/capacity limit was reached, or a
  counter would overflow
- `WC_EALIGN`: caller-provided buffer alignment failure
- `WC_EBADLIMITS`: inconsistent, overlapping, or undersized caller limits
- `WC_EBUSY`: conflicting use of shared scan buffer or active stream

`wc_errstr` maps every code to a stable lowercase string.

## Build Profiles

- Hosted default: no extra defines.
- Libc-light: `-DWC_STDC_HOSTED=0 -DWC_USE_LIBC_STRING=0
  -DWC_USE_LIBC_QSORT=0 -DWC_HAVE_ERRNO=0 -DWC_OMIT_ASSERT=1`. This profile is
  still heap-enabled and requires `<stdlib.h>` allocator support or custom
  `WC_MALLOC`/`WC_REALLOC`/`WC_FREE` definitions.
- Libc-free heapless: `-DWC_NO_HEAP=1 -DWC_STACK_BUFFER=0
  -DWC_STDC_HOSTED=0 -DWC_USE_LIBC_STRING=0 -DWC_USE_LIBC_QSORT=0
  -DWC_HAVE_ERRNO=0 -DWC_OMIT_ASSERT=1` with caller-supplied static storage.
- Tiny RAM streaming: `-DWC_STACK_BUFFER=0 -DWC_STREAM_REUSE_SCANBUF=1` with a
  reduced `WC_MAX_WORD` if desired.

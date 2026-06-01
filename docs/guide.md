# wordcount Guide

Practical notes for integrating the library and CLI.

## Quick Start (Library)

```c
#include "wordcount.h"

wc_limits lim;
wc_limits_init(&lim);      /* sets struct_size and clears fields */
lim.max_bytes = 8 * 1024;  /* optional budget (0 = unlimited) */

int rc = WC_OK;
wc *w = wc_open_ex(0, &lim, &rc);
if (!w) { /* handle rc */ }

wc_add_norm_n(w, "Hello", 5); /* normalized add; does not tokenize */
wc_scan(w, "more text here", 15);

wc_word *out = NULL;
size_t n = 0;
if (wc_topn(w, 10, &out, &n) == WC_OK) {
    /* results sorted by count desc, word asc */
    wc_results_free(out);
}

wc_close(w);
```

## Streaming Workflow

```c
wc_stream *s = wc_stream_open(w, NULL);
size_t consumed = 0;
wc_stream_scan_ex(s, chunk0, chunk0_len, &consumed);
/* ...more chunks... */
wc_stream_finish(s); /* flush trailing token */
wc_stream_close(s);
```

- `wc_stream_finish` is idempotent; after it runs, `wc_stream_scan_ex` returns
  the same code as `wc_stream_finish` with `consumed_out` left at 0.
- `wc_stream_close` does not flush a trailing word. Call `wc_stream_finish`
  before closing when the final token should be counted.
- If the parent `wc` is closed first, open streams are detached: later
  scan/finish calls return `WC_ERROR`, and `wc_stream_close` remains safe.

## Static Buffer Setup

```c
WC_STATIC_BUFFER(pool, 4096);
wc_limits lim = WC_LIMITS_INIT();
lim.static_buf = pool.buf;
lim.static_size = sizeof pool.buf;

wc *w = wc_open_ex(64, &lim, NULL);
```

Preconditions:

- `static_buf` must meet the library's internal alignment requirement; use
  `WC_STATIC_BUFFER` for portable static-buffer alignment. If `uintptr_t` is
  unavailable, set `WC_TRUST_STATIC_BUFFER_ALIGNMENT=1` only when the caller
  guarantees alignment for static buffers. In that trust mode, misalignment is
  not reliably rejected at runtime.
- In `WC_NO_HEAP=1` builds, the handle is placed at the start of `static_buf`;
  its aligned footprint counts against `static_size`.
- With `block_size=0`, static-buffer mode sizes the single arena block to use
  the remaining effective static budget after fixed internal allocations.

## Budgets and Strict Mode

- `max_bytes` counts table storage, arena blocks, and heap/static scan buffers
  (`WC_STACK_BUFFER=0`); results arrays and stream objects are excluded.
- `strict_max_bytes=1` forbids any transient peak above `max_bytes` during
  rehash; failures return `WC_NOMEM`.
- `wc_reserve` is a best-effort preflight/growth helper. It can reduce early
  `WC_NOMEM`, but callers must still handle `WC_NOMEM` from inserts and scans.

## Freestanding / Custom Builds

- Libc-light: `-DWC_STDC_HOSTED=0 -DWC_USE_LIBC_STRING=0 -DWC_USE_LIBC_QSORT=0
  -DWC_HAVE_ERRNO=0 -DWC_OMIT_ASSERT=1`. This profile is still heap-enabled and
  requires `<stdlib.h>` allocator support or custom `WC_MALLOC`/`WC_FREE`; use
  the heapless profile for libc-free builds.
- Heapless: `-DWC_NO_HEAP=1` plus a static buffer for both handle and storage.
- Custom integer types: define `WC_U32_T` / `WC_U64_T`; also define
  `WC_PTRDIFF_MAX` and `WC_HAVE_UINTPTR=0` if `<stdint.h>`/`PTRDIFF_MAX` or
  `uintptr_t` is unavailable.

## Error Handling Patterns

- All `err_out` parameters are written on success and failure; check them to
  avoid stale values.
- `errno` is set only on failure when `WC_HAVE_ERRNO!=0`; success leaves it
  untouched.
- Use `wc_errstr(rc)` for human-readable diagnostics; strings are static.

## CLI Quick Examples

- Default top 25 words: `wc README.md`
- TSV top 50: `wc -n 50 --format tsv README.md`
- Strict 256 KiB budget: `wc --max-bytes 262144 --strict-max-bytes input.txt`

See `docs/cli.md` for the full CLI contract.

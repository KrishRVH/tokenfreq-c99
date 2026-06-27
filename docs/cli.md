# `wc` CLI Reference

## Usage

```
wc [OPTIONS] [FILE ...]
```

With no file operands, stdin is read. File operands are streamed and aggregated
into a single result set. A literal `-` is not a stdin sentinel. Input failures
are reported, later file operands are still attempted, and the final exit status
is `1` if any input failed. When any input fails, normal result output is
suppressed because the aggregate would be partial.

The CLI requires the linked `wordcount` library to be hosted and heap-enabled.
`--help` and `--version` still work with other linked configurations so build
mismatches can be diagnosed.

Memory note: ordinary `--top N` output without length filters uses `wc_topn()`
and allocates only the displayed rows. Non-quiet `--all` and length-filtered
output need the full sorted result array before filtering; quiet length-filtered
output counts matches with the zero-allocation cursor.

## Exit Codes

- `0` success
- `1` runtime failure (I/O, OOM/allocation, invalid `WC_MAX_BYTES`, output
  errors)
- `2` usage/argument errors; usage diagnostics keep this exit code even if the
  diagnostic stream itself fails.

## Options

- Output:
  - `-n, --top N` – show top N words (default: 25; N must be greater than 0
    unless `--all` is set, in which case the top limit is ignored).
  - `--all` – show all unique words.
  - `--format {table,tsv,json}` – output format (default: table).
  - `-q, --quiet` – suppress per-word listing; summary only.
  - `--color {auto,always,never}` / `--no-color`.
- Parsing / limits:
  - `--min-len N`, `--max-len N` – filter results by displayed word length.
  - `--max-word N` – set library `max_word` (clamped to build-time `WC_MAX_WORD`).
  - `--max-bytes BYTES` – set the internal memory budget (table/arena/scanbuf).
  - `--strict-max-bytes` – enforce that budget as a hard peak cap for tracked
    internal allocations. Requires a nonzero `--max-bytes` or `WC_MAX_BYTES`.
    Result arrays and stream objects are outside this cap; use `--quiet` to
    avoid result-array allocation.
- Misc:
  - `--version`
  - `-h, --help`

## Output Formats

- `table` (default): when rows are displayed, width-aware columns with
  header/separator; counts are right-aligned, words left-aligned. Ordering is
  always count desc, word asc. Summary prints to stdout.
- `tsv`: when rows are displayed, header row `rank<TAB>count<TAB>word\n`
  followed by stable rows. Summary prints to stderr.
- `json`: object with keys `words` (array of `{rank,count,word}`) and
  `summary` (`total`, `unique`, `filtered`, `displayed`, `bytes`). Empty input
  yields an empty array.

Word rows and row headers go to stdout. Table summaries go to stdout; TSV
summaries go to stderr; JSON writes the complete object to stdout. Diagnostics
and the non-JSON, non-quiet "No words found." notice go to stderr.

## Examples

- Top 10 words as TSV: `wc -n 10 --format tsv book.txt`
- Summary only: `wc -q README.md`
- Full list with summary: `wc --all README.md`
- Strict memory-capped run: `wc --max-bytes 262144 --strict-max-bytes data.txt`

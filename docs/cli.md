# `wc` CLI Reference

## Usage

```
wc [OPTIONS] [FILE|-]
```

Single file or `-` for stdin. Multiple files are processed independently and
summarized; failures increment the exit status but do not abort other inputs.

## Exit Codes

- `0` success
- `1` runtime failure (I/O, OOM, parse)
- `2` usage/argument errors

## Options

- Output:
  - `-n, --top N` – show top N words (default: 25; `0` prints only summaries).
  - `--all` – show all unique words.
  - `--format {table,tsv,json}` – output format (default: table).
  - `--summary` – always print summary totals.
  - `-q, --quiet` – suppress per-word listing; summary only.
  - `--color {auto,always,never}` / `--no-color`.
- Parsing / limits:
  - `--min-len N`, `--max-len N` – filter results by displayed word length.
  - `--max-word N` – set library `max_word` (clamped to build-time `WC_MAX_WORD`).
  - `--max-bytes BYTES` – cap internal memory (applies to table/arena/scanbuf).
  - `--strict-max-bytes` – enforce `max_bytes` as a hard peak cap.
  - `--no-mmap` – Force streaming for file inputs (disable memory mapping).
- Misc:
  - `--version`
  - `-h, --help`

## Output Formats

- `table` (default): width-aware columns with header/separator; counts are
  right-aligned, words left-aligned. Ordering is always count desc, word asc.
- `tsv`: header row `rank<TAB>count<TAB>word\n` followed by stable rows.
- `json`: object with keys `words` (array of `{rank,count,word}`) and
  `summary` (`total`, `unique`, `filtered`, `displayed`, `bytes`). Empty input
  yields an empty array.

All data rows go to stdout; diagnostics go to stderr.

## Examples

- Top 10 words as TSV: `wc -n 10 --format tsv book.txt`
- Full list with summary only: `wc --all --summary README.md`
- Memory-capped run: `wc --max-bytes 262144 --strict-max-bytes data.txt`

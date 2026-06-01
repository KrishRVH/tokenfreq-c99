#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'

# ------------------------------------------------------------------------------
# wordcount benchmark harness
#
# Goals:
#   - Benchmark the library itself (wc_scan / streaming / results) with minimal noise
#   - Deterministic corpus construction (cached sources, exact byte sizing)
#   - Fair scheduling (interleaved runs, warmups, optional cache priming)
#   - Useful artifacts (raw samples CSV, summary JSON, env/build metadata)
#
# Notes:
#   - This script is Linux-first. macOS works best if GNU coreutils are installed:
#       brew install coreutils gnu-time
#     and then ensure 'gtime' exists.
# ------------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ----------------------------- Defaults ---------------------------------------
DO_TOY=1
RUNS=8
WARMUP=2
SIZE_MIB=512
CHUNK_BYTES=65536
MAX_WORD=0 # 0 => library default (DEF_WORD), clamped internally
VALIDATE=0
REGEN_CORPUS=0
NO_DOWNLOAD=0
PRIME_CACHE=1
PIN_CPU=""    # e.g. "0" to pin to cpu 0 via taskset
NICE_LEVEL="" # e.g. "-10" (may require privileges)
OUT_ROOT="$SCRIPT_DIR/results"
CORPUS_PATH="" # if empty, auto-build in bench_data/
MODES_CSV="scan,stream,scan_results"
VARIANTS_CSV="default,heap,tiny"
DO_CLI=0 # optional: also build+benchmark wc_main.c CLI
QUIET=1  # for benchmarking (no output from the bench binaries)
PERF=0   # best-effort perf stat capture (optional, Linux)

# ------------------------------ Styling ---------------------------------------
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  BOLD=$'\033[1m'
  RED=$'\033[31m'
  YELLOW=$'\033[33m'
  BLUE=$'\033[34m'
  NC=$'\033[0m'
else
  BOLD=""
  RED=""
  YELLOW=""
  BLUE=""
  NC=""
fi

note() { echo "${BLUE}[INFO]${NC} $*"; }
warn() { echo "${YELLOW}[WARN]${NC} $*" >&2; }
die() {
  echo "${RED}[ERR ]${NC} $*" >&2
  exit 1
}

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

Corpus:
  --corpus PATH          Use an existing corpus file (no download/build)
  --size-mib N           Target size for generated corpus (default: $SIZE_MIB MiB)
  --regen-corpus         Rebuild corpus even if it already exists
  --no-download          Fail if sources are missing; do not fetch from network
  --no-prime-cache       Do not pre-read corpus to warm page cache

Benchmark configuration:
  --runs N               Measured runs per implementation (default: $RUNS)
  --warmup N             Warmup cycles (default: $WARMUP)
  --modes LIST           Comma list: scan,stream,scan_results,stream_results (default: $MODES_CSV)
  --variants LIST        Comma list: default,heap,tiny (default: $VARIANTS_CSV)
  --max-word N           Runtime max_word passed to wc_open_ex (0 = default)
  --chunk BYTES          Streaming chunk size (default: $CHUNK_BYTES)
  --no-toy              Do not build/run the Linux toy competitor

Execution controls:
  --pin-cpu N            Pin each run to a specific CPU (Linux taskset)
  --nice N               Run benchmarks with 'nice -n N' (best-effort)
  --cli                  Also benchmark the wc_main.c CLI (scan+results+I/O)

Validation:
  -v, --validate         Validate scan vs stream consistency on tricky bytes

Output:
  --out DIR              Output root for results (default: $OUT_ROOT)
  --verbose              Do not silence benchmark binaries (prints Total/Unique)

Perf:
  --perf                 Best-effort 'perf stat' capture per implementation (Linux)

Examples:
  # Quick smoke
  ./bench.sh --size-mib 128 --runs 5 --warmup 1

  # More stable single-core run
  ./bench.sh --pin-cpu 0 --runs 12 --warmup 3

  # Validate correctness invariants + benchmark
  ./bench.sh --validate

EOF
}

# --------------------------- Arg parsing --------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h | --help)
      usage
      exit 0
      ;;
    --runs)
      RUNS="${2:?}"
      shift 2
      ;;
    --warmup)
      WARMUP="${2:?}"
      shift 2
      ;;
    --size-mib)
      SIZE_MIB="${2:?}"
      shift 2
      ;;
    --chunk)
      CHUNK_BYTES="${2:?}"
      shift 2
      ;;
    --modes)
      MODES_CSV="${2:?}"
      shift 2
      ;;
    --variants)
      VARIANTS_CSV="${2:?}"
      shift 2
      ;;
    --max-word)
      MAX_WORD="${2:?}"
      shift 2
      ;;
    --corpus)
      CORPUS_PATH="${2:?}"
      shift 2
      ;;
    --regen-corpus)
      REGEN_CORPUS=1
      shift
      ;;
    --no-download)
      NO_DOWNLOAD=1
      shift
      ;;
    --no-prime-cache)
      PRIME_CACHE=0
      shift
      ;;
    --pin-cpu)
      PIN_CPU="${2:?}"
      shift 2
      ;;
    --nice)
      NICE_LEVEL="${2:?}"
      shift 2
      ;;
    --out)
      OUT_ROOT="${2:?}"
      shift 2
      ;;
    --cli)
      DO_CLI=1
      shift
      ;;
    --no-toy)
      DO_TOY=0
      shift
      ;;
    --perf)
      PERF=1
      shift
      ;;
    --verbose)
      QUIET=0
      shift
      ;;
    -v | --validate)
      VALIDATE=1
      shift
      ;;
    *) die "Unknown argument: $1 (use --help)" ;;
  esac
done

# ------------------------------ Tools -----------------------------------------
have() { command -v "$1" > /dev/null 2>&1; }

# Prefer clang if available, else gcc, else cc.
CC="${CC:-}"
if [[ -z "$CC" ]]; then
  if have clang; then
    CC=clang
  elif have gcc; then
    CC=gcc
  elif have cc; then
    CC=cc
  else
    die "No C compiler found (clang/gcc/cc)."
  fi
fi

# GNU time is strongly preferred so we can capture RSS etc.
TIME_BIN=""
if have gtime; then
  TIME_BIN="gtime"
elif [[ -x /usr/bin/time ]]; then
  # Probe whether it supports -f (GNU time)
  if /usr/bin/time -f "%e" true > /dev/null 2>&1; then
    TIME_BIN="/usr/bin/time"
  fi
fi
[[ -n "$TIME_BIN" ]] || die "GNU time not found. On macOS: brew install gnu-time (gtime)."

# Python is optional but strongly improves reporting.
PYTHON_BIN=""
if have python3; then
  PYTHON_BIN=python3
elif have python; then
  PYTHON_BIN=python
fi

# ---------------------------- Paths -------------------------------------------
BENCH_DATA_DIR="$SCRIPT_DIR/bench_data"
SRC_DIR="$BENCH_DATA_DIR/sources"
BUILD_DIR="$SCRIPT_DIR/build"
BIN_DIR="$SCRIPT_DIR/bin"

mkdir -p "$BENCH_DATA_DIR" "$SRC_DIR" "$BUILD_DIR" "$BIN_DIR" "$OUT_ROOT"

# ------------------------ Helper: file size ----------------------------------
file_size_bytes() {
  local f="$1"
  if stat -c%s "$f" > /dev/null 2>&1; then
    stat -c%s "$f"
  else
    stat -f%z "$f"
  fi
}
cc_supports_flag() {
  local flag="$1"
  echo 'int main(void){return 0;}' | "$CC" -x c - "$flag" -o "$BUILD_DIR/.flagtest" > /dev/null 2>&1
}

split_flags() {
  local src="$1"
  local -n dst="$2"
  dst=()
  [[ -n "$src" ]] || return 0
  local old_ifs="$IFS"
  IFS=' '
  # shellcheck disable=SC2034 # dst is a nameref output parameter.
  read -r -a dst <<< "$src"
  IFS="$old_ifs"
}

format_argv() {
  printf '%q ' "$@"
}

# ---------------------- Environment / metadata --------------------------------
system_summary() {
  echo "date_utc=$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  echo "uname=$(uname -a)"
  echo "cc=$CC"
  echo "cc_version=$($CC --version 2> /dev/null | head -n 1 || true)"
  if have git && git -C "$ROOT_DIR" rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    echo "git_head=$(git -C "$ROOT_DIR" rev-parse HEAD)"
    echo "git_dirty=$(git -C "$ROOT_DIR" status --porcelain | wc -l | tr -d ' ')"
  fi

  if [[ "$(uname -s)" == "Linux" ]]; then
    if [[ -r /proc/cpuinfo ]]; then
      echo "cpu_model=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs || true)"
    fi
    if have nproc; then
      echo "logical_cpus=$(nproc)"
    fi
    if have lscpu; then
      echo "lscpu=$(
        lscpu 2> /dev/null | tr '\n' ';' | sed 's/;*$//'
      )"
    fi
  elif [[ "$(uname -s)" == "Darwin" ]]; then
    if have sysctl; then
      echo "cpu_model=$(sysctl -n machdep.cpu.brand_string 2> /dev/null || true)"
      echo "logical_cpus=$(sysctl -n hw.logicalcpu 2> /dev/null || true)"
    fi
  fi
}

# ----------------------------- Corpus build -----------------------------------
# Default sources: Gutenberg (cached locally). You can always pass --corpus PATH.
BOOK_URLS=(
  "https://www.gutenberg.org/files/1342/1342-0.txt"
  "https://www.gutenberg.org/files/84/84-0.txt"
  "https://www.gutenberg.org/files/2701/2701-0.txt"
)

download_sources() {
  mkdir -p "$SRC_DIR"
  if ((NO_DOWNLOAD)); then
    for url in "${BOOK_URLS[@]}"; do
      local f
      f="$SRC_DIR/$(basename "$url")"
      [[ -f "$f" ]] || die "--no-download set, missing source: $f"
    done
    return 0
  fi

  have curl || die "curl is required to download corpora (or pass --corpus / use --no-download)."

  note "Ensuring corpus sources exist in: $SRC_DIR"
  for url in "${BOOK_URLS[@]}"; do
    local f
    f="$SRC_DIR/$(basename "$url")"
    if [[ -f "$f" ]]; then
      continue
    fi
    note "Downloading: $(basename "$url")"
    curl -fsSL --retry 3 --retry-delay 1 -o "$f" "$url" \
      || die "Download failed: $url"
  done
}

build_corpus() {
  local req_bytes=$((SIZE_MIB * 1024 * 1024))
  local corpus="$BENCH_DATA_DIR/corpus_${SIZE_MIB}MiB.bin"
  local seed="$BENCH_DATA_DIR/seed.txt"
  local tmp="$BENCH_DATA_DIR/corpus.tmp"

  if [[ -n "$CORPUS_PATH" ]]; then
    [[ -f "$CORPUS_PATH" ]] || die "Corpus not found: $CORPUS_PATH"
    echo "$CORPUS_PATH"
    return 0
  fi

  if [[ -f "$corpus" && $REGEN_CORPUS -eq 0 ]]; then
    local sz
    sz="$(file_size_bytes "$corpus")"
    if [[ "$sz" -eq "$req_bytes" ]]; then
      note "Corpus already exists: $corpus (${SIZE_MIB} MiB exact)"
      echo "$corpus"
      return 0
    else
      warn "Corpus size mismatch (have $sz bytes, want $req_bytes). Rebuilding."
    fi
  fi

  download_sources

  note "Building deterministic seed..."
  rm -f "$seed"
  # Fixed order concatenation => deterministic.
  for url in "${BOOK_URLS[@]}"; do
    cat "$SRC_DIR/$(basename "$url")" >> "$seed"
    printf "\n" >> "$seed"
  done

  local seed_bytes
  seed_bytes="$(file_size_bytes "$seed")"
  [[ "$seed_bytes" -gt 0 ]] || die "Seed corpus is empty."

  local reps=$(((req_bytes + seed_bytes - 1) / seed_bytes))
  note "Target: $req_bytes bytes (${SIZE_MIB} MiB), seed: $seed_bytes bytes, reps: $reps"

  rm -f "$tmp" "$corpus"
  for ((i = 1; i <= reps; i++)); do
    cat "$seed" >> "$tmp"
  done

  # Trim to exact byte size for stable throughput calculations.
  # head -c works on both GNU and BSD.
  head -c "$req_bytes" "$tmp" > "$corpus"
  rm -f "$tmp"

  note "Corpus ready: $corpus ($(file_size_bytes "$corpus") bytes)"
  echo "$corpus"
}

prime_page_cache() {
  local corpus="$1"
  ((PRIME_CACHE)) || return 0

  note "Priming page cache (sequential read)..."
  if have dd; then
    # status=none is GNU; suppress errors on BSD dd.
    dd if="$corpus" of=/dev/null bs=8m status=none 2> /dev/null \
      || dd if="$corpus" of=/dev/null bs=8m 2> /dev/null \
      || cat "$corpus" > /dev/null
  else
    cat "$corpus" > /dev/null
  fi
}

# ----------------------- Benchmark harness C code ------------------------------
write_bench_c() {
  local out="$BUILD_DIR/bench_wc.c"
  cat > "$out" << 'EOF'
/*
 * bench_wc.c - benchmarking harness for wordcount library
 *
 * Modes:
 *   scan          : mmap + wc_scan
 *   scan_results  : mmap + wc_scan + wc_results (sort) + free
 *   stream        : read() + wc_stream_scan_ex + finish
 *   stream_results: read() + wc_stream_scan_ex + finish + wc_results + free
 *
 * Output:
 *   By default prints: "Total: <n> Unique: <n>\n" to stdout
 *   With --quiet prints nothing (useful for timing)
 */
#define _GNU_SOURCE

#include "wordcount.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef MADV_SEQUENTIAL
#define MADV_SEQUENTIAL 0
#endif

typedef enum {
    MODE_SCAN = 0,
    MODE_SCAN_RESULTS,
    MODE_STREAM,
    MODE_STREAM_RESULTS
} Mode;

static void die(const char *msg) {
    int e = errno;
    if (e)
        fprintf(stderr, "bench_wc: %s: %s\n", msg, strerror(e));
    else
        fprintf(stderr, "bench_wc: %s\n", msg);
    exit(2);
}

static int streq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static uint64_t parse_u64(const char *s, const char *what) {
    char *end = NULL;
    unsigned long long v;
    if (!s || !*s) die("missing numeric value");
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || end == s || (end && *end != '\0')) {
        fprintf(stderr, "bench_wc: invalid %s: '%s'\n", what, s);
        exit(2);
    }
    return (uint64_t)v;
}

static void print_build_info(void) {
    const wc_build_config *cfg = wc_build_info();
    if (!cfg) return;
    fprintf(stderr, "bench_wc: build_info: version=%lu max_word=%zu min_init_cap=%zu min_block_sz=%zu stack_buffer=%d\n",
            cfg->version_number, cfg->max_word, cfg->min_init_cap, cfg->min_block_sz, cfg->stack_buffer);
}

static int run_scan(const char *path, wc *w, int do_results, int quiet) {
#if !(defined(__unix__) || defined(__APPLE__))
    (void)path; (void)w; (void)do_results; (void)quiet;
    die("scan mode requires POSIX mmap");
    return 2;
#else
    int fd = -1;
    struct stat st;
    void *mem = NULL;
    size_t len = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) die("open");

    if (fstat(fd, &st) < 0) die("fstat");
    if (st.st_size < 0) {
        close(fd);
        errno = EFBIG;
        die("file too large");
    }

    len = (size_t)st.st_size;
    if (len == 0) {
        close(fd);
        if (!quiet) printf("Total: 0 Unique: 0\n");
        return 0;
    }

    mem = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mem == MAP_FAILED) die("mmap");

#if defined(__linux__)
    (void)madvise(mem, len, MADV_SEQUENTIAL);
#endif

    {
        int rc = wc_scan(w, (const char *)mem, len);
        if (rc != WC_OK) {
            fprintf(stderr, "bench_wc: wc_scan failed: %s\n", wc_errstr(rc));
            munmap(mem, len);
            close(fd);
            return 2;
        }
    }

    if (do_results) {
        wc_word *r = NULL;
        size_t n = 0;
        int rc = wc_results(w, &r, &n);
        if (rc != WC_OK) {
            fprintf(stderr, "bench_wc: wc_results failed: %s\n", wc_errstr(rc));
            munmap(mem, len);
            close(fd);
            return 2;
        }
        wc_results_free(r);
    }

    munmap(mem, len);
    close(fd);

    if (!quiet) {
        printf("Total: %zu Unique: %zu\n", wc_total(w), wc_unique(w));
    }
    return 0;
#endif
}

static int run_stream(const char *path, wc *w, int do_results, size_t chunk, int quiet) {
#if !(defined(__unix__) || defined(__APPLE__))
    (void)path; (void)w; (void)do_results; (void)chunk; (void)quiet;
    die("stream mode requires POSIX read()");
    return 2;
#else
    int fd = -1;
    wc_stream *s = NULL;
    int rc = WC_OK;
    unsigned char *buf = NULL;

    if (chunk < 1024) chunk = 1024;

    buf = (unsigned char *)malloc(chunk);
    if (!buf) die("malloc(chunk)");

    fd = open(path, O_RDONLY);
    if (fd < 0) die("open");

    s = wc_stream_open(w, &rc);
    if (!s) {
        fprintf(stderr, "bench_wc: wc_stream_open failed: %s\n", wc_errstr(rc));
        close(fd);
        free(buf);
        return 2;
    }

    for (;;) {
        ssize_t n = read(fd, buf, chunk);
        if (n < 0) die("read");
        if (n == 0) break;
        {
            size_t consumed = 0;
            rc = wc_stream_scan_ex(s, (const char *)buf, (size_t)n, &consumed);
            if (rc == WC_OK && consumed != (size_t)n)
                rc = WC_ERROR;
        }
        if (rc != WC_OK) {
            fprintf(stderr, "bench_wc: wc_stream_scan_ex failed: %s\n", wc_errstr(rc));
            wc_stream_close(s);
            close(fd);
            free(buf);
            return 2;
        }
    }

    rc = wc_stream_finish(s);
    if (rc != WC_OK) {
        fprintf(stderr, "bench_wc: wc_stream_finish failed: %s\n", wc_errstr(rc));
        wc_stream_close(s);
        close(fd);
        free(buf);
        return 2;
    }

    wc_stream_close(s);
    close(fd);
    free(buf);

    if (do_results) {
        wc_word *r = NULL;
        size_t n = 0;
        rc = wc_results(w, &r, &n);
        if (rc != WC_OK) {
            fprintf(stderr, "bench_wc: wc_results failed: %s\n", wc_errstr(rc));
            return 2;
        }
        wc_results_free(r);
    }

    if (!quiet) {
        printf("Total: %zu Unique: %zu\n", wc_total(w), wc_unique(w));
    }
    return 0;
#endif
}

int main(int argc, char **argv) {
    const char *path = NULL;
    Mode mode = MODE_SCAN;
    size_t chunk = 65536;
    size_t max_word = 0;
    uint64_t max_bytes = 0;
    unsigned long hash_seed = 0;
    int quiet = 0;
    int print_build = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (streq(a, "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (streq(m, "scan")) mode = MODE_SCAN;
            else if (streq(m, "scan_results")) mode = MODE_SCAN_RESULTS;
            else if (streq(m, "stream")) mode = MODE_STREAM;
            else if (streq(m, "stream_results")) mode = MODE_STREAM_RESULTS;
            else {
                fprintf(stderr, "bench_wc: unknown mode: %s\n", m);
                return 2;
            }
        } else if (streq(a, "--chunk") && i + 1 < argc) {
            chunk = (size_t)parse_u64(argv[++i], "chunk");
        } else if (streq(a, "--max-word") && i + 1 < argc) {
            max_word = (size_t)parse_u64(argv[++i], "max-word");
        } else if (streq(a, "--max-bytes") && i + 1 < argc) {
            max_bytes = parse_u64(argv[++i], "max-bytes");
        } else if (streq(a, "--hash-seed") && i + 1 < argc) {
            hash_seed = (unsigned long)parse_u64(argv[++i], "hash-seed");
        } else if (streq(a, "--quiet")) {
            quiet = 1;
        } else if (streq(a, "--print-build")) {
            print_build = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "bench_wc: unknown option: %s\n", a);
            return 2;
        } else {
            path = a;
        }
    }

    if (!path) {
        fprintf(stderr, "bench_wc usage:\n");
        fprintf(stderr, "  bench_wc <file> [--mode scan|scan_results|stream|stream_results] [--chunk N] [--max-word N]\n");
        return 2;
    }

    if (print_build) {
        print_build_info();
    }

    wc_limits lim;
    int open_rc = WC_OK;
    wc_limits_init(&lim);
    lim.max_bytes = (size_t)max_bytes;
    lim.hash_seed = hash_seed;

    wc *w = wc_open_ex(max_word, &lim, &open_rc);
    if (!w) {
        fprintf(stderr, "bench_wc: wc_open_ex failed: %s\n", wc_errstr(open_rc));
        return 2;
    }

    int rc = 0;
    switch (mode) {
        case MODE_SCAN: rc = run_scan(path, w, 0, quiet); break;
        case MODE_SCAN_RESULTS: rc = run_scan(path, w, 1, quiet); break;
        case MODE_STREAM: rc = run_stream(path, w, 0, chunk, quiet); break;
        case MODE_STREAM_RESULTS: rc = run_stream(path, w, 1, chunk, quiet); break;
        default: rc = 2; break;
    }

    wc_close(w);
    return rc;
}
EOF
  echo "$out"
}

write_toy_competitor_c() {
  local out="$BUILD_DIR/toyfast.c"
  cat > "$out" << 'EOF'
/*
 * toyfast.c - hyper-optimized Linux-only competitor
 *
 * Semantics (approx aligned with wc_scan defaults):
 *   - ASCII letters only: [A-Za-z]
 *   - lowercases by (c|32)
 *   - truncates words to max_word (default 64 unless overridden)
 *
 * Design:
 *   - mmap input
 *   - split into thread chunks w/ boundary fixups (no double counting)
 *   - per-thread open-addressed table storing pointers into the mmap input
 *   - merge per-thread tables into a global table (pointer reuse, no key copies)
 *
 * Not robust. Built for speed and elegance.
 */
#define _GNU_SOURCE
#ifndef __linux__
#error "toyfast competitor is Linux-only."
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#ifndef MADV_SEQUENTIAL
#define MADV_SEQUENTIAL 2
#endif

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define HOT __attribute__((hot))
#define AI  __attribute__((always_inline)) inline
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define HOT
#define AI inline
#define UNLIKELY(x) (x)
#endif

static void die(const char *msg) {
    int e = errno;
    if (e) fprintf(stderr, "toyfast: %s: %s\n", msg, strerror(e));
    else   fprintf(stderr, "toyfast: %s\n", msg);
    _exit(2);
}

static AI int is_alpha(unsigned char c) {
    return (((unsigned)c | 32u) - (unsigned)'a') < 26u;
}

/* A fast 64-bit multiplicative mixer (fxhash-ish). */
static AI uint64_t hstep(uint64_t h, uint64_t c) {
    return (h ^ c) * 0x517cc1b727220a95ULL;
}

static AI size_t next_pow2(size_t x) {
    if (x <= 2) return 2;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
#if SIZE_MAX > 0xffffffffu
    x |= x >> 32;
#endif
    return x + 1;
}

typedef struct {
    const unsigned char *key; /* points into the mmap input */
    uint32_t len;
    uint32_t cnt;
    uint64_t h;
} Entry;

typedef struct {
    Entry  *slots;
    size_t  cap;
    size_t  mask;
    size_t  len;
    size_t  threshold;
} Map;

static void *xmap_anon(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}

static HOT void map_init(Map *m, size_t cap_hint) {
    memset(m, 0, sizeof *m);

    /* Clamp for sanity: too small => resize thrash, too big => waste. */
    size_t cap = next_pow2(cap_hint);
    if (cap < (1u << 16)) cap = (1u << 16);
    if (cap > (1u << 24)) cap = (1u << 24); /* toy clamp */

    size_t bytes = cap * sizeof(Entry);
    m->slots = (Entry *)xmap_anon(bytes);
    if (!m->slots) die("mmap(slots)");
    madvise(m->slots, bytes, MADV_HUGEPAGE);

    m->cap = cap;
    m->mask = cap - 1;
    m->len = 0;
    m->threshold = (cap * 4) / 5; /* ~0.80 load */
}

static AI uint64_t load64u(const unsigned char *p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static AI int key_eq_fold(const unsigned char *a, const unsigned char *b, uint32_t n) {
    const uint64_t lower = 0x2020202020202020ULL;

    while (n >= 8) {
        if ((load64u(a) | lower) != (load64u(b) | lower)) return 0;
        a += 8;
        b += 8;
        n -= 8;
    }
    while (n) {
        if (((unsigned)*a++ | 32u) != ((unsigned)*b++ | 32u)) return 0;
        n--;
    }
    return 1;
}

static HOT void map_grow(Map *m) {
    size_t new_cap = m->cap * 2;
    if (new_cap > (1u << 25)) die("map too large (toyfast)");

    size_t new_bytes = new_cap * sizeof(Entry);
    Entry *ns = (Entry *)xmap_anon(new_bytes);
    if (!ns) die("mmap(new slots)");
    madvise(ns, new_bytes, MADV_HUGEPAGE);

    size_t new_mask = new_cap - 1;
    for (size_t i = 0; i < m->cap; i++) {
        Entry e = m->slots[i];
        if (!e.key) continue;
        size_t idx = (size_t)e.h & new_mask;
        while (ns[idx].key) idx = (idx + 1) & new_mask;
        ns[idx] = e;
    }

    munmap(m->slots, m->cap * sizeof(Entry));
    m->slots = ns;
    m->cap = new_cap;
    m->mask = new_mask;
    m->threshold = (new_cap * 4) / 5;
}

static HOT void map_put_span(Map *m, const unsigned char *key, uint32_t n, uint64_t h, uint32_t add_cnt) {
    if (UNLIKELY(m->len >= m->threshold)) map_grow(m);

    size_t idx = (size_t)h & m->mask;
    for (;;) {
        Entry *e = &m->slots[idx];
        if (!e->key) {
            e->key = key;
            e->len = n;
            e->cnt = add_cnt;
            e->h = h;
            m->len++;
            return;
        }
        if (e->h == h && e->len == n && key_eq_fold(e->key, key, n)) {
            e->cnt += add_cnt;
            return;
        }
        idx = (idx + 1) & m->mask;
    }
}

/* Count CPUs available under current affinity mask (respects taskset). */
static int cpu_allowed_count(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
#ifdef CPU_COUNT
        {
            int n = CPU_COUNT(&set);
            if (n > 0) return n;
        }
#endif
        int n = 0;
        for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &set)) n++;
        if (n > 0) return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return (int)n;
}

/* Get the i-th CPU id from the current affinity set (round-robin). */
static int cpu_id_at(int idx) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return idx;

    int seen = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &set)) {
            if (seen == idx) return cpu;
            seen++;
        }
    }
    return idx;
}

typedef struct {
    const unsigned char *start;
    const unsigned char *end;
    uint32_t maxw;
    int pin_cpu; /* -1 = no pin */
    Map map;
    uint64_t total_words;
} Ctx;

static void pin_thread(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void *worker(void *arg) {
    Ctx *c = (Ctx *)arg;
    pin_thread(c->pin_cpu);

    /* Heuristic: big-ish table to reduce grow. */
    size_t chunk_len = (size_t)(c->end - c->start);
    size_t cap_hint = (chunk_len / 64) + (1u << 16);        /* toy heuristic */

    map_init(&c->map, cap_hint);

    const unsigned char *p = c->start;
    const unsigned char *end = c->end;

    uint64_t total = 0;

    while (p < end) {
        while (p < end && !is_alpha(*p)) p++;
        if (p >= end) break;

        const unsigned char *word = p;
        uint64_t h = 0x9e3779b97f4a7c15ULL;
        uint32_t n = 0;

        while (p < end && is_alpha(*p)) {
            unsigned char ch = (unsigned char)(*p++ | 32u);
            if (n < c->maxw) {
                h = hstep(h, ch);
                n++;
            }
        }

        if (n) {
            map_put_span(&c->map, word, n, h, 1);
            total++;
        }
    }

    c->total_words = total;
    return NULL;
}

static void usage(void) {
    fprintf(stderr, "toyfast <file> [--threads N] [--max-word N] [--quiet]\n");
    _exit(2);
}

static uint64_t parse_u64(const char *s) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || (end && *end)) usage();
    return (uint64_t)v;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int quiet = 0;
    int threads = 0;        /* 0 => auto */
    uint32_t maxw = 64;     /* competitor default: 64 */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--quiet")) quiet = 1;
        else if (!strcmp(a, "--threads") && i + 1 < argc) threads = (int)parse_u64(argv[++i]);
        else if (!strcmp(a, "--max-word") && i + 1 < argc) maxw = (uint32_t)parse_u64(argv[++i]);
        else if (a[0] == '-') usage();
        else path = a;
    }
    if (!path) usage();
    if (maxw == 0) maxw = 64;
    if (maxw < 4) maxw = 4;
    if (maxw > 1024) maxw = 1024; /* toy clamp */

    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open");

    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat");
    if (st.st_size < 0) die("file too large");
    size_t fsize = (size_t)st.st_size;

    if (fsize == 0) {
        if (!quiet) printf("Total: 0 Unique: 0\n");
        return 0;
    }

    unsigned char *data = (unsigned char *)mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) die("mmap(file)");
    close(fd);

    madvise(data, fsize, MADV_SEQUENTIAL);
    madvise(data, fsize, MADV_HUGEPAGE);

    int avail = cpu_allowed_count();
    if (threads <= 0) threads = avail;
    if (threads < 1) threads = 1;
    if (threads > 256) threads = 256;

    /* Avoid threading overhead for small inputs */
    if (fsize < (16u << 20)) threads = 1;

    pthread_t *th = (pthread_t *)malloc((size_t)threads * sizeof(*th));
    Ctx *ctx = (Ctx *)malloc((size_t)threads * sizeof(*ctx));
    if (!th || !ctx) die("malloc(threads)");

    size_t chunk = fsize / (size_t)threads;

    for (int i = 0; i < threads; i++) {
        size_t s = (size_t)i * chunk;
        size_t e = (i == threads - 1) ? fsize : (size_t)(i + 1) * chunk;

        /* Boundary fix: if start is in middle of a word, skip to end of that word. */
        if (s > 0 && is_alpha(data[s - 1])) {
            while (s < fsize && is_alpha(data[s])) s++;
        }
        if (e < s) {
            e = s;
        }
        /* Extend end forward to include a whole word (next chunk will skip). */
        if (i != threads - 1) {
            if (e > 0 && is_alpha(data[e - 1])) {
                while (e < fsize && is_alpha(data[e])) e++;
            }
        }

        ctx[i].start = data + s;
        ctx[i].end   = data + e;
        ctx[i].maxw  = maxw;
        ctx[i].total_words = 0;
        ctx[i].pin_cpu = cpu_id_at(i % avail); /* respects taskset affinity */

        if (pthread_create(&th[i], NULL, worker, &ctx[i]) != 0) die("pthread_create");
    }

    for (int i = 0; i < threads; i++) {
        pthread_join(th[i], NULL);
    }

    /* Merge into a global map (single-threaded merge, reusing key pointers). */
    uint64_t total_words = 0;
    size_t total_uniques_est = 0;
    for (int i = 0; i < threads; i++) {
        total_words += ctx[i].total_words;
        total_uniques_est += ctx[i].map.len;
    }

    Map g;
    memset(&g, 0, sizeof g);
    /* global cap based on estimated uniques */
    size_t gcap_hint = (total_uniques_est * 2) + (1u << 16);
    map_init(&g, gcap_hint);

    for (int i = 0; i < threads; i++) {
        Map *m = &ctx[i].map;
        for (size_t j = 0; j < m->cap; j++) {
            Entry *e = &m->slots[j];
            if (!e->key) continue;
            map_put_span(&g, e->key, e->len, e->h, e->cnt);
        }
    }

    if (!quiet) {
        printf("Total: %llu Unique: %zu\n",
               (unsigned long long)total_words,
               g.len);
    }

    munmap(data, fsize);
    return 0;
}
EOF
  echo "$out"
}

# -------------------------- Build variants ------------------------------------
# Variant set: compile-time macro differences (important for embedded profiles)
declare -A VAR_DEFINES=(
  [default]="-DWC_OMIT_ASSERT"
  [heap]="-DWC_OMIT_ASSERT -DWC_STACK_BUFFER=0"
  [tiny]="-DWC_OMIT_ASSERT -DWC_STACK_BUFFER=0 -DWC_MIN_INIT_CAP=8 -DWC_MIN_BLOCK_SZ=64 -DWC_MAX_WORD=32"
)

split_csv() {
  local csv="$1"
  local IFS=','
  read -r -a _out <<< "$csv"
  printf "%s\n" "${_out[@]}"
}

build_binaries() {
  local bench_c="$1"

  # Conservative, performance-oriented defaults. Users can override via CFLAGS env.
  local cflags_src="${CFLAGS:--O3 -DNDEBUG -pipe}"
  local -a cflags=()
  split_flags "$cflags_src" cflags
  # -march=native is great for local perf work; keep it default unless user opted out via CFLAGS.
  if [[ "${CFLAGS:-}" != *"-march="* && "${CFLAGS:-}" != *"-march "* ]] && cc_supports_flag -march=native; then
    cflags+=("-march=native")
  fi

  note "Building benchmark binaries with: $(format_argv "$CC" "${cflags[@]}")"
  for v in "${VARIANTS[@]}"; do
    [[ -n "${VAR_DEFINES[$v]:-}" ]] || die "Unknown variant: $v"
    local out="$BIN_DIR/bench_${v}"
    local defs="${VAR_DEFINES[$v]}"
    local -a defs_arr=()
    split_flags "$defs" defs_arr

    "$CC" "${cflags[@]}" "${defs_arr[@]}" -I"$ROOT_DIR" \
      "$ROOT_DIR/wordcount.c" "$bench_c" \
      -o "$out" \
      || die "Build failed for variant '$v'"

    note "Built: $out (${v})"
  done

  # --- toy competitor (Linux-only) ---
  if ((DO_TOY)); then
    if [[ "$(uname -s)" == "Linux" ]]; then
      local toy_c
      toy_c="$(write_toy_competitor_c)"

      # Aggressive defaults. Let users override via CFLAGS_TOY.
      local toy_flags_src="${CFLAGS_TOY:--O3 -DNDEBUG -pipe}"
      local -a toy_flags=()
      split_flags "$toy_flags_src" toy_flags
      if [[ "${CFLAGS_TOY:-}" != *"-march="* && "${CFLAGS_TOY:-}" != *"-march "* ]] && cc_supports_flag -march=native; then
        toy_flags+=("-march=native")
      fi

      # Opportunistic: LTO if supported
      if cc_supports_flag -flto; then
        toy_flags+=("-flto")
      fi
      # Opportunistic: reduce some linkage overhead if supported
      if cc_supports_flag -fno-plt; then
        toy_flags+=("-fno-plt")
      fi
      if cc_supports_flag -fno-semantic-interposition; then
        toy_flags+=("-fno-semantic-interposition")
      fi

      note "Building toy competitor with: $(format_argv "$CC" "${toy_flags[@]}" -pthread "$toy_c" -o "$BIN_DIR/toyfast")"
      "$CC" "${toy_flags[@]}" -pthread "$toy_c" -o "$BIN_DIR/toyfast" \
        || die "Build failed for toyfast competitor"
      note "Built: $BIN_DIR/toyfast"
    else
      warn "Toy competitor is Linux-only; skipping."
    fi
  fi

  if ((DO_CLI)); then
    note "Building CLI binaries (wc_main.c)..."
    for v in "${VARIANTS[@]}"; do
      local out="$BIN_DIR/wc_cli_${v}"
      local defs="${VAR_DEFINES[$v]}"
      local -a defs_arr=()
      split_flags "$defs" defs_arr
      "$CC" "${cflags[@]}" "${defs_arr[@]}" -I"$ROOT_DIR" \
        "$ROOT_DIR/wordcount.c" "$ROOT_DIR/wc_main.c" \
        -o "$out" \
        || die "Build failed for CLI variant '$v'"
      note "Built: $out (CLI ${v})"
    done
  fi
}

# ----------------------------- Validation -------------------------------------
make_validation_blob() {
  local f="$BENCH_DATA_DIR/validation.bin"
  if [[ -f "$f" && $REGEN_CORPUS -eq 0 ]]; then
    echo "$f"
    return 0
  fi

  if [[ -n "$PYTHON_BIN" ]]; then
    "$PYTHON_BIN" - << PY > "$f"
import sys
# Includes: mixed case, punctuation, digits, UTF-8 bytes, embedded NULs, edge separators
b = bytearray()
b += b"Hello, WORLD! it's fine. abc123DEF\\n"
b += b"cafe\\xc3\\xa9 CAF\\xc3\\x89\\n"            # UTF-8 "café" / "CAFÉ"
b += b"nul\\x00byte\\x00split\\n"
b += b"----A----b----C----\\n"
b += b"x"*200 + b" " + b"Y"*200 + b"\\n"          # long runs (truncation stress)
sys.stdout.buffer.write(b)
PY
  else
    # Fallback without python: no embedded NUL (still exercises basics)
    printf "Hello, WORLD! it's fine. abc123DEF\ncafe CAF\n----A----b----C----\n" > "$f"
  fi

  echo "$f"
}

extract_totuniq() {
  # Expects a line like "Total: X Unique: Y"
  # Prints: "X Y" or empty on failure.
  awk '
    /Total:/ && /Unique:/ {
      for (i=1;i<=NF;i++) {
        if ($i=="Total:") t=$(i+1);
        if ($i=="Unique:") u=$(i+1);
      }
      if (t ~ /^[0-9]+$/ && u ~ /^[0-9]+$/) { print t, u; exit 0 }
    }
  '
}

run_validation() {
  note "Validation: ensuring scan/stream consistency on tricky bytes..."
  local val
  val="$(make_validation_blob)"

  for v in "${VARIANTS[@]}"; do
    local bin="$BIN_DIR/bench_${v}"
    [[ -x "$bin" ]] || die "Missing binary: $bin"

    local a b c d
    a="$("$bin" "$val" --mode scan | extract_totuniq || true)"
    b="$("$bin" "$val" --mode stream --chunk "$CHUNK_BYTES" | extract_totuniq || true)"
    c="$("$bin" "$val" --mode scan_results | extract_totuniq || true)"
    d="$("$bin" "$val" --mode stream_results --chunk "$CHUNK_BYTES" | extract_totuniq || true)"

    if [[ -z "$a" || -z "$b" || -z "$c" || -z "$d" ]]; then
      die "Validation parse failed for variant '$v' (output malformed)"
    fi

    if ! [[ "$a" == "$b" && "$a" == "$c" && "$a" == "$d" ]]; then
      echo "variant=$v"
      echo "  scan         : $a"
      echo "  stream       : $b"
      echo "  scan_results : $c"
      echo "  stream_results: $d"
      die "Validation failed (scan/stream mismatch) for variant '$v'"
    fi

    note "OK: $v (Total/Unique = $a)"
  done

  note "Validation passed."
}

# ------------------------------ Benchmarking ----------------------------------
# We'll measure with GNU time capturing:
#   wall_seconds,user_seconds,sys_seconds,max_rss_kb
TIME_FMT="%e,%U,%S,%M"

cmd_prefix() {
  # Compose optional taskset + nice prefixes.
  local prefix=()
  if [[ -n "$PIN_CPU" ]]; then
    if have taskset; then
      prefix+=(taskset -c "$PIN_CPU")
    else
      warn "--pin-cpu requested but taskset not available; ignoring."
    fi
  fi
  if [[ -n "$NICE_LEVEL" ]]; then
    if have nice; then
      prefix+=(nice -n "$NICE_LEVEL")
    else
      warn "--nice requested but 'nice' not available; ignoring."
    fi
  fi
  ((${#prefix[@]} == 0)) && return 0
  printf "%q " "${prefix[@]}"
}

bench_one() {
  local label="$1"
  local run_id="$2"
  shift 2
  local tmpfile="$RESULTS_DIR/tmp_time.txt"

  if "$TIME_BIN" -f "$TIME_FMT" -o "$tmpfile" -- "$@" > /dev/null 2>&1; then
    local line
    line="$(cat "$tmpfile")"
    echo "$label,$run_id,$line" >> "$RAW_CSV"
  else
    die "Command failed during benchmark: $label"
  fi
}

shuffle_indices() {
  local n="$1"
  if have shuf; then
    seq 0 $((n - 1)) | shuf
  else
    # Fallback: no shuffle (still interleaves by run)
    seq 0 $((n - 1))
  fi
}

maybe_perf_stat() {
  ((PERF)) || return 0
  [[ "$(uname -s)" == "Linux" ]] || return 0
  have perf || {
    warn "--perf requested but perf not found; skipping."
    return 0
  }

  local label="$1"
  shift
  local out="$RESULTS_DIR/perf_${label//[^a-zA-Z0-9_.-]/_}.txt"

  # Best-effort: perf permissions vary by distro.
  set +e
  perf stat -d -- "$@" > /dev/null 2> "$out"
  local rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    warn "perf stat failed for '$label' (see $out)."
  else
    note "perf stat captured: $out"
  fi
}

summarize_results() {
  if [[ -z "$PYTHON_BIN" ]]; then
    warn "python not found; leaving raw CSV only: $RAW_CSV"
    return 0
  fi

  "$PYTHON_BIN" - << PY
import csv, json, math, os, statistics, sys
from collections import defaultdict

raw = os.environ["RAW_CSV"]
out_json = os.environ["SUMMARY_JSON"]
corpus_bytes = int(os.environ["CORPUS_BYTES"])
baseline = os.environ["BASELINE"]

groups = defaultdict(list)
rss_groups = defaultdict(list)

with open(raw, newline="") as f:
    r = csv.reader(f)
    header = next(r)
    # label,run_id,wall,user,sys,rss_kb
    for row in r:
        label = row[0]
        wall = float(row[2])
        user = float(row[3])
        sys_t = float(row[4])
        rss = float(row[5])
        groups[label].append((wall, user, sys_t))
        rss_groups[label].append(rss)

def pct(p, xs):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    k = math.ceil(p * len(xs)) - 1
    k = max(0, min(k, len(xs) - 1))
    return xs[k]

def stats(label, rows):
    walls = [w for (w,_,_) in rows]
    users = [u for (_,u,_) in rows]
    syss  = [s for (_,_,s) in rows]
    mean = statistics.mean(walls)
    stdev = statistics.stdev(walls) if len(walls) > 1 else 0.0
    med = statistics.median(walls)
    p95 = pct(0.95, walls)
    mn = min(walls)
    mx = max(walls)
    cv = (stdev / mean * 100.0) if mean > 0 else 0.0
    gib_s = (corpus_bytes / (1024**3)) / mean if mean > 0 else 0.0
    mib_s = (corpus_bytes / (1024**2)) / mean if mean > 0 else 0.0
    rss_mean = statistics.mean(rss_groups[label])
    rss_max  = max(rss_groups[label])
    return {
        "label": label,
        "n": len(walls),
        "mean_s": mean,
        "stdev_s": stdev,
        "median_s": med,
        "p95_s": p95,
        "min_s": mn,
        "max_s": mx,
        "cv_pct": cv,
        "throughput_gib_s": gib_s,
        "throughput_mib_s": mib_s,
        "rss_mean_kb": rss_mean,
        "rss_max_kb": rss_max,
        "user_mean_s": statistics.mean(users),
        "sys_mean_s": statistics.mean(syss),
    }

summ = [stats(lbl, rows) for (lbl, rows) in groups.items()]
summ.sort(key=lambda x: x["mean_s"])

base_mean = None
for s in summ:
    if s["label"] == baseline:
        base_mean = s["mean_s"]
        break

for s in summ:
    if base_mean and s["mean_s"] > 0:
        s["speedup_vs_baseline"] = base_mean / s["mean_s"]
    else:
        s["speedup_vs_baseline"] = None

# Pretty table
def fmt(x, w=8):
    if x is None or (isinstance(x, float) and (math.isnan(x) or math.isinf(x))):
        return " " * (w-3) + "n/a"
    return f"{x:{w}.4f}"

print()
print("Results (GNU time; lower is better):")
print("-" * 120)
print(f"{'Label':35} {'mean(s)':>10} {'stdev':>10} {'p95':>10} {'GiB/s':>10} {'RSS(MiB)':>10} {'speedup':>10}")
print("-" * 120)
for s in summ:
    rss_mib = s["rss_max_kb"] / 1024.0
    sp = s["speedup_vs_baseline"]
    sp_s = f"{sp:10.2f}x" if sp else f"{'':>10}"
    print(f"{s['label'][:35]:35} {s['mean_s']:10.4f} {s['stdev_s']:10.4f} {s['p95_s']:10.4f} {s['throughput_gib_s']:10.3f} {rss_mib:10.1f} {sp_s}")
print("-" * 120)
print()

payload = {
    "corpus_bytes": corpus_bytes,
    "baseline": baseline,
    "summary": summ,
}

with open(out_json, "w") as f:
    json.dump(payload, f, indent=2)

print(f"Wrote: {out_json}")
PY
}

# ------------------------------ Main ------------------------------------------
note "${BOLD}wordcount benchmark harness${NC}"
note "Root: $ROOT_DIR"
note "Compiler: $CC"
note "Runs: $RUNS (warmup cycles: $WARMUP)"
note "Modes: $MODES_CSV"
note "Variants: $VARIANTS_CSV"
note "Max word (runtime): $MAX_WORD"
note "Stream chunk: $CHUNK_BYTES bytes"
if [[ -n "$PIN_CPU" ]]; then note "CPU pin: taskset -c $PIN_CPU"; fi
if [[ -n "$NICE_LEVEL" ]]; then note "nice: $NICE_LEVEL"; fi

# Resolve corpus
CORPUS="$(build_corpus)"
CORPUS_BYTES="$(file_size_bytes "$CORPUS")"
note "Corpus: $CORPUS (${CORPUS_BYTES} bytes)"

# Generate harness C code
BENCH_C="$(write_bench_c)"
note "Generated harness: $BENCH_C"

# Parse variants/modes
mapfile -t VARIANTS < <(split_csv "$VARIANTS_CSV")
mapfile -t MODES < <(split_csv "$MODES_CSV")

# Build binaries
build_binaries "$BENCH_C"

# Optional validation
if ((VALIDATE)); then
  run_validation
fi

# Create results directory
STAMP="$(date -u +"%Y%m%d-%H%M%S")"
RESULTS_DIR="$OUT_ROOT/$STAMP"
mkdir -p "$RESULTS_DIR"
RAW_CSV="$RESULTS_DIR/raw.csv"
SUMMARY_JSON="$RESULTS_DIR/summary.json"
ENV_TXT="$RESULTS_DIR/env.txt"
BUILD_TXT="$RESULTS_DIR/build.txt"

# Write metadata
system_summary > "$ENV_TXT"
{
  echo "CFLAGS=${CFLAGS:-"(default)"}"
  echo "runs=$RUNS"
  echo "warmup=$WARMUP"
  echo "modes=$MODES_CSV"
  echo "variants=$VARIANTS_CSV"
  echo "max_word=$MAX_WORD"
  echo "chunk_bytes=$CHUNK_BYTES"
  echo "pin_cpu=$PIN_CPU"
  echo "nice=$NICE_LEVEL"
  echo "corpus=$CORPUS"
  echo "corpus_bytes=$CORPUS_BYTES"
  echo "quiet=$QUIET"
} > "$BUILD_TXT"

note "Results dir: $RESULTS_DIR"
note "Writing raw samples to: $RAW_CSV"

# Header for raw CSV:
# label,run_id,wall_s,user_s,sys_s,maxrss_kb
echo "label,run_id,wall_s,user_s,sys_s,maxrss_kb" > "$RAW_CSV"

# Prime cache once (best-effort)
prime_page_cache "$CORPUS"

# Prepare job list
declare -a JOB_LABELS=()
declare -a JOB_CMDS=()

cmd_prefix_text="$(cmd_prefix)"

for v in "${VARIANTS[@]}"; do
  for m in "${MODES[@]}"; do
    case "$m" in
      scan | stream | scan_results | stream_results) ;;
      *) die "Unknown mode requested: $m" ;;
    esac

    label="${v}:${m}"
    bin="$BIN_DIR/bench_${v}"
    [[ -x "$bin" ]] || die "Missing binary: $bin"

    # bench_wc arguments
    args=("$bin" "$CORPUS" "--mode" "$m" "--chunk" "$CHUNK_BYTES" "--max-word" "$MAX_WORD")
    if ((QUIET)); then args+=("--quiet"); fi

    # Store as a command array serialized via printf %q for safe bash -c evaluation.
    cmd="$cmd_prefix_text"
    for a in "${args[@]}"; do cmd+=$(printf "%q " "$a"); done

    JOB_LABELS+=("$label")
    JOB_CMDS+=("$cmd")
  done
done

if ((DO_TOY)) && [[ "$(uname -s)" == "Linux" ]]; then
  label="toyfast"
  bin="$BIN_DIR/toyfast"
  [[ -x "$bin" ]] || die "Missing toy competitor binary: $bin"

  args=("$bin" "$CORPUS" "--max-word" "$MAX_WORD")
  if ((QUIET)); then args+=("--quiet"); fi

  cmd="$cmd_prefix_text"
  for a in "${args[@]}"; do cmd+=$(printf "%q " "$a"); done

  JOB_LABELS+=("$label")
  JOB_CMDS+=("$cmd")
fi

if ((DO_CLI)); then
  for v in "${VARIANTS[@]}"; do
    label="cli:${v}"
    bin="$BIN_DIR/wc_cli_${v}"
    [[ -x "$bin" ]] || die "Missing CLI binary: $bin"
    cmd="$cmd_prefix_text$(printf "%q " "$bin" "$CORPUS")"
    JOB_LABELS+=("$label")
    JOB_CMDS+=("$cmd")
  done
fi

# Baseline selection: default:scan if present, else first job.
BASELINE="default:scan"
baseline_found=0
for lbl in "${JOB_LABELS[@]}"; do
  if [[ "$lbl" == "$BASELINE" ]]; then
    baseline_found=1
    break
  fi
done
if ((baseline_found == 0)); then
  BASELINE="${JOB_LABELS[0]}"
fi
note "Baseline: $BASELINE"

# Warmup cycles (interleaved)
note "Warmup cycles: $WARMUP"
for ((w = 1; w <= WARMUP; w++)); do
  for ((i = 0; i < ${#JOB_LABELS[@]}; i++)); do
    # shellcheck disable=SC2090
    bash -c "${JOB_CMDS[$i]}" > /dev/null 2>&1 || die "Warmup failed: ${JOB_LABELS[$i]}"
  done
done

# Measured runs (interleaved + shuffled per run)
note "Measured runs: $RUNS (interleaved order; per-run shuffle when available)"
for ((r = 1; r <= RUNS; r++)); do
  note "Run $r/$RUNS"
  mapfile -t order < <(shuffle_indices "${#JOB_LABELS[@]}")
  for idx in "${order[@]}"; do
    label="${JOB_LABELS[$idx]}"
    cmd="${JOB_CMDS[$idx]}"

    # Convert command string back into argv by using bash -c; time wraps bash.
    # We time the bash shell too, but overhead is tiny for large corpora.
    bench_one "$label" "$r" bash -c "$cmd"
  done
done

# Optional perf stat: run once per job (best-effort)
if ((PERF)); then
  note "perf stat (best-effort; may be restricted by kernel settings)..."
  for ((i = 0; i < ${#JOB_LABELS[@]}; i++)); do
    label="${JOB_LABELS[$i]}"
    cmd="${JOB_CMDS[$i]}"
    # Run without --quiet so perf isn't timing an empty process? (still fine either way)
    maybe_perf_stat "$label" bash -c "$cmd"
  done
fi

# Summarize
export RAW_CSV SUMMARY_JSON
export CORPUS_BYTES="$CORPUS_BYTES"
export BASELINE="$BASELINE"
summarize_results

note "Done."
note "Raw:    $RAW_CSV"
note "Env:    $ENV_TXT"
note "Build:  $BUILD_TXT"
note "Summary:$SUMMARY_JSON"

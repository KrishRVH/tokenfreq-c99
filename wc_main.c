/*
** wc_main.c - Command-line interface
**
** Public domain.
**
** DESIGN
**
**   All input is processed via the library's streaming API (wc_stream_*),
**   keeping input buffering bounded and ensuring tokenization stays in sync
**   with wc_scan(). Total memory remains proportional to unique words and
**   result arrays unless allocation limits are configured. The CLI
**   intentionally avoids memory-mapped I/O: portable C cannot reliably recover
**   if a mapped file is truncated during scanning.
**
**   NOTE ABOUT BUILD MISMATCHES
**
**   wc_main.c is compiled against wordcount.h but may be linked against a
**   separately-built wordcount.c with different build macros. All input paths
**   use the library streaming API (wc_stream_*), avoiding duplicated
**   tokenization logic in the CLI.
**
** Usage: wc [OPTIONS] [FILE ...]
**   Reads stdin if no files are given. File operands are aggregated into one
**   result set. Table summaries print to stdout, TSV summaries to stderr,
**   and JSON embeds its summary.
**
** Environment:
**   WC_MAX_BYTES  - Optional steady-state allocation budget for internal
**                   table/arena/scan-buffer storage, in bytes (e.g.
**                   "8388608" for 8MB). With a nonzero budget,
**                   --strict-max-bytes enforces a hard peak cap on those tracked
**                   internal allocations. Must be a non-negative decimal integer.
*/
#ifndef WC_NO_HOSTED_MAIN

#include "wordcount.h"

#if (WC_NO_HEAP + 0) || !(WC_STDC_HOSTED + 0)
#error "wc_main.c requires a hosted heap-enabled wordcount build; define WC_NO_HOSTED_MAIN to omit the CLI main."
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <wchar.h>

static wchar_t *utf8_to_wide(const char *utf8);
#else
#include <signal.h>
#include <unistd.h>
#endif

#define TOPN 25
#if WC_PTRDIFF_MAX < 1
#error "WC_PTRDIFF_MAX must be positive."
#endif
#define WC_CLI_MAX_CHUNK 4096u
#define STDIN_CHUNK                                      \
    (((size_t)WC_PTRDIFF_MAX < (size_t)WC_CLI_MAX_CHUNK) \
             ? (size_t)WC_PTRDIFF_MAX                    \
             : ((WC_SIZE_MAX < (size_t)WC_CLI_MAX_CHUNK) \
                        ? WC_SIZE_MAX                    \
                        : (size_t)WC_CLI_MAX_CHUNK))
#define TABLE_WORD_WIDTH_LIMIT 4096u

typedef char wc_cli_chunk_must_be_positive[(STDIN_CHUNK > 0u) ? 1 : -1];
typedef char wc_cli_chunk_must_fit_ptrdiff
        [(STDIN_CHUNK <= (size_t)WC_PTRDIFF_MAX) ? 1 : -1];
typedef char wc_cli_chunk_must_fit_size[(STDIN_CHUNK <= WC_SIZE_MAX) ? 1 : -1];

#if defined(__GNUC__) || defined(__clang__)
#define WC_CLI_PRINTF(fmt_index, first_arg) \
    __attribute__((format(printf, fmt_index, first_arg)))
#else
#define WC_CLI_PRINTF(fmt_index, first_arg)
#endif

#ifndef EFBIG
#define EFBIG ERANGE
#endif
typedef enum {
    FORMAT_TABLE,
    FORMAT_TSV,
    FORMAT_JSON
} output_format;

typedef enum {
    COLOR_AUTO,
    COLOR_ALWAYS,
    COLOR_NEVER
} color_mode;

typedef struct {
    size_t topn;
    int show_all;
    size_t min_len;
    size_t max_len;
    int has_min_len;
    int has_max_len;
    size_t max_word;
    int has_max_word;
    int show_version;
    int show_help;
    size_t max_bytes;
    int has_max_bytes;
    int strict_max_bytes;
    int quiet;
    output_format format;
    color_mode color;
} cli_opts;

typedef struct {
    uintmax_t bytes_processed;
    size_t files_processed;
    size_t files_failed;
} run_stats;

#ifdef _WIN32
static FILE *win32_fopen_utf8(const char *path, const wchar_t *mode)
{
    wchar_t *wpath = utf8_to_wide(path);
    FILE *fp;

    if (!wpath)
        return NULL;

    fp = _wfopen(wpath, mode);
    free(wpath);
    return fp;
}
#endif

static int parse_size_arg(const char *s, size_t *out)
{
    const char *p = s;
    char *end = NULL;
    unsigned long long v;

    if (!s || !*s || !out)
        return -1;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' ||
           *p == '\v')
        p++;
    if (*p == '-')
        return -1;

    errno = 0;
    v = strtoull(p, &end, 10);
    if (errno != 0 || end == p)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' ||
           *end == '\f' || *end == '\v')
        end++;
    if (*end != '\0')
        return -1;

    if (v > (unsigned long long)SIZE_MAX)
        return -1;
    *out = (size_t)v;
    return 0;
}

/* --- Parse environment-based limits ----------------------------------- */

static int parse_wc_limits_from_env(wc_limits *lim)
{
    if (!lim)
        return 0;

    wc_limits_init(lim);

    const char *env = getenv("WC_MAX_BYTES");
    if (!env || !*env)
        return 0;

    if (parse_size_arg(env, &lim->max_bytes) < 0)
        return -1;
    return 1;
}

static int add_uintmax_checked(uintmax_t *acc, uintmax_t n)
{
    if (!acc)
        return -1;
    if (*acc > UINTMAX_MAX - (uintmax_t)n) {
        errno = EFBIG;
        return -1;
    }
    *acc += (uintmax_t)n;
    return 0;
}

static int cli_fprintf(FILE *out, const char *fmt, ...) WC_CLI_PRINTF(2, 3);

static int cli_fprintf(FILE *out, const char *fmt, ...)
{
    int rc;
    va_list ap;

    if (!out || !fmt)
        return -1;

    va_start(ap, fmt);
    rc = vfprintf(out, fmt, ap);
    va_end(ap);
    return (rc < 0) ? -1 : 0;
}

static int cli_fputs(FILE *out, const char *s)
{
    if (!out || !s)
        return -1;
    return (fputs(s, out) == EOF) ? -1 : 0;
}

static int cli_putc(FILE *out, int ch)
{
    if (!out)
        return -1;
    return (fputc(ch, out) == EOF) ? -1 : 0;
}

static int cli_flush(FILE *out)
{
    if (!out)
        return -1;
    if (fflush(out) == EOF)
        return -1;
    return ferror(out) ? -1 : 0;
}

static int diag_fputs_quoted(FILE *out, const char *s)
{
    static const char hex[] = "0123456789abcdef";

    if (cli_putc(out, '"') < 0)
        return -1;
    while (s && *s) {
        unsigned char c = (unsigned char)*s++;

        if (c == '"' || c == '\\') {
            if (cli_putc(out, '\\') < 0 || cli_putc(out, c) < 0)
                return -1;
        } else if (c >= 0x20u && c < 0x7fu) {
            if (cli_putc(out, c) < 0)
                return -1;
        } else if (cli_fputs(out, "\\x") < 0 ||
                   cli_putc(out, hex[(c >> 4) & 0x0fu]) < 0 ||
                   cli_putc(out, hex[c & 0x0fu]) < 0) {
            return -1;
        }
    }
    return cli_putc(out, '"');
}

static void diag_arg(const char *prefix, const char *value)
{
    (void)cli_fputs(stderr, prefix);
    (void)diag_fputs_quoted(stderr, value);
    (void)cli_putc(stderr, '\n');
}

static void diag_path_msg(const char *path, const char *msg)
{
    (void)cli_fputs(stderr, "wc: ");
    (void)diag_fputs_quoted(stderr, path);
    (void)cli_fprintf(stderr, ": %s\n", msg);
}

static void diag_path_errno(const char *path, int saved_errno)
{
    (void)cli_fputs(stderr, "wc: ");
    (void)diag_fputs_quoted(stderr, path);
    (void)cli_fprintf(stderr, ": %s\n", strerror(saved_errno));
}

static void diag_path_wc(const char *path, int wc_rc)
{
    (void)cli_fputs(stderr, "wc: ");
    (void)diag_fputs_quoted(stderr, path);
    (void)cli_fprintf(stderr, ": %s\n", wc_errstr(wc_rc));
}

static void diag_path_wc_after(const char *path, int wc_rc, uintmax_t bytes)
{
    (void)cli_fputs(stderr, "wc: ");
    (void)diag_fputs_quoted(stderr, path);
    (void)cli_fprintf(stderr,
                      ": %s (after %" PRIuMAX " bytes)\n",
                      wc_errstr(wc_rc),
                      bytes);
}

static int print_usage(FILE *out)
{
    return cli_fprintf(
            out,
            "Usage: wc [OPTIONS] [FILE ...]\n"
            "  Reads FILEs, or stdin when no FILEs are given, and reports "
            "word frequencies.\n"
            "\n"
            "Output:\n"
            "  -n, --top N           Show top N words (default %d)\n"
            "      --all             Show all unique words\n"
            "      --format {table,tsv,json}\n"
            "  -q, --quiet           Summary only\n"
            "      --color {auto,always,never}\n"
            "      --no-color        Disable color entirely\n"
            "\n"
            "Parsing:\n"
            "      --min-len N       Exclude words shorter than N\n"
            "      --max-len N       Exclude words longer than N\n"
            "      --max-word N      Set library max_word limit\n"
            "\n"
            "Memory:\n"
            "      --max-bytes N     Budget internal bytes (overrides "
            "WC_MAX_BYTES)\n"
            "      --strict-max-bytes Enforce nonzero --max-bytes/WC_MAX_BYTES "
            "as peak cap\n"
            "\n"
            "Other:\n"
            "      --version         Print version and exit\n"
            "  -h, --help            Show this help\n"
            "\n"
            "Examples:\n"
            "  wc book.txt\n"
            "  wc -n 50 --format tsv book.txt\n"
            "  wc --max-bytes 262144 --strict-max-bytes huge.txt\n",
            TOPN);
}

static int
parse_cli_opts(int argc, char **argv, cli_opts *opts, int *first_file_index)
{
    int i;

    if (!opts || !first_file_index)
        return -1;

    opts->topn = TOPN;
    opts->show_all = 0;
    opts->min_len = 0;
    opts->max_len = 0;
    opts->has_min_len = 0;
    opts->has_max_len = 0;
    opts->max_word = 0;
    opts->has_max_word = 0;
    opts->show_version = 0;
    opts->show_help = 0;
    opts->max_bytes = 0;
    opts->has_max_bytes = 0;
    opts->strict_max_bytes = 0;
    opts->quiet = 0;
    opts->format = FORMAT_TABLE;
    opts->color = COLOR_AUTO;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-')
            break;
        if (strcmp(arg, "-") == 0)
            break;
        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            opts->show_help = 1;
            i++;
            break;
        } else if (strcmp(arg, "--version") == 0) {
            opts->show_version = 1;
            i++;
            break;
        } else if (strcmp(arg, "--all") == 0) {
            opts->show_all = 1;
        } else if (strcmp(arg, "--strict-max-bytes") == 0) {
            opts->strict_max_bytes = 1;
        } else if (strcmp(arg, "--no-color") == 0) {
            opts->color = COLOR_NEVER;
        } else if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0) {
            opts->quiet = 1;
        } else if (strcmp(arg, "--top") == 0 || strcmp(arg, "-n") == 0) {
            size_t v;
            if (i + 1 >= argc)
                return -1;
            if (parse_size_arg(argv[i + 1], &v) < 0) {
                (void)fprintf(stderr, "wc: invalid value for --top\n");
                return -1;
            }
            opts->topn = v;
            i++;
        } else if (strcmp(arg, "--max-bytes") == 0) {
            size_t v;
            if (i + 1 >= argc)
                return -1;
            if (parse_size_arg(argv[i + 1], &v) < 0) {
                (void)fprintf(stderr, "wc: invalid value for --max-bytes\n");
                return -1;
            }
            opts->has_max_bytes = 1;
            opts->max_bytes = v;
            i++;
        } else if (strcmp(arg, "--min-len") == 0) {
            size_t v;
            if (i + 1 >= argc)
                return -1;
            if (parse_size_arg(argv[i + 1], &v) < 0) {
                (void)fprintf(stderr, "wc: invalid value for --min-len\n");
                return -1;
            }
            opts->has_min_len = 1;
            opts->min_len = v;
            i++;
        } else if (strcmp(arg, "--max-len") == 0) {
            size_t v;
            if (i + 1 >= argc)
                return -1;
            if (parse_size_arg(argv[i + 1], &v) < 0) {
                (void)fprintf(stderr, "wc: invalid value for --max-len\n");
                return -1;
            }
            opts->has_max_len = 1;
            opts->max_len = v;
            i++;
        } else if (strcmp(arg, "--max-word") == 0) {
            size_t v;
            if (i + 1 >= argc)
                return -1;
            if (parse_size_arg(argv[i + 1], &v) < 0) {
                (void)fprintf(stderr, "wc: invalid value for --max-word\n");
                return -1;
            }
            opts->has_max_word = 1;
            opts->max_word = v;
            i++;
        } else if (strcmp(arg, "--format") == 0) {
            const char *fmt;
            if (i + 1 >= argc)
                return -1;
            fmt = argv[++i];
            if (strcmp(fmt, "table") == 0) {
                opts->format = FORMAT_TABLE;
            } else if (strcmp(fmt, "tsv") == 0) {
                opts->format = FORMAT_TSV;
            } else if (strcmp(fmt, "json") == 0) {
                opts->format = FORMAT_JSON;
            } else {
                diag_arg("wc: unknown format: ", fmt);
                return -1;
            }
        } else if (strcmp(arg, "--color") == 0) {
            const char *val;
            if (i + 1 >= argc)
                return -1;
            val = argv[++i];
            if (strcmp(val, "auto") == 0) {
                opts->color = COLOR_AUTO;
            } else if (strcmp(val, "always") == 0) {
                opts->color = COLOR_ALWAYS;
            } else if (strcmp(val, "never") == 0) {
                opts->color = COLOR_NEVER;
            } else {
                diag_arg("wc: unknown color mode: ", val);
                return -1;
            }
        } else {
            diag_arg("wc: unknown option: ", arg);
            return -1;
        }
    }

    if (opts->has_min_len && opts->has_max_len &&
        opts->min_len > opts->max_len) {
        (void)fprintf(stderr, "wc: --min-len must be <= --max-len\n");
        return -1;
    }
    if (!opts->show_all && opts->topn == 0) {
        (void)fprintf(stderr, "wc: --top must be greater than 0\n");
        return -1;
    }

    *first_file_index = i;
    return 0;
}

/* --- Windows UTF-8 argument/path helpers ------------------------------ */

#ifdef _WIN32

/* Convert UTF-8 path to UTF-16; sets errno precisely on failure. */
static wchar_t *utf8_to_wide(const char *utf8)
{
    int n;
    wchar_t *wstr;

    if (!utf8) {
        errno = EINVAL;
        return NULL;
    }

    /* MB_ERR_INVALID_CHARS causes invalid UTF-8 to fail deterministically. */
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) {
        DWORD e = GetLastError();
        if (e == ERROR_NO_UNICODE_TRANSLATION)
            errno = EINVAL;
        else
            errno = EIO;
        return NULL;
    }

    wstr = (wchar_t *)malloc((size_t)n * sizeof *wstr);
    if (!wstr) {
        errno = ENOMEM;
        return NULL;
    }

    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wstr, n) <=
        0) {
        DWORD e = GetLastError();
        free(wstr);
        if (e == ERROR_NO_UNICODE_TRANSLATION)
            errno = EINVAL;
        else
            errno = EIO;
        return NULL;
    }

    return wstr;
}

static char **win32_get_args_utf8(int *argc_out)
{
    int wargc = 0;
    wchar_t **wargv;
    char **uargv;
    int i;

    if (!argc_out)
        return NULL;

    wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv)
        return NULL;

    uargv = (char **)malloc(((size_t)wargc + 1u) * sizeof *uargv);
    if (!uargv) {
        LocalFree(wargv);
        return NULL;
    }

    for (i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(
                CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0) {
            /* Treat conversion failure as fatal (no silent empty args). */
            int j;
            for (j = 0; j < i; j++)
                free(uargv[j]);
            free(uargv);
            LocalFree(wargv);
            errno = EINVAL;
            return NULL;
        }

        uargv[i] = (char *)malloc((size_t)n);
        if (!uargv[i]) {
            int j;
            for (j = 0; j < i; j++)
                free(uargv[j]);
            free(uargv);
            LocalFree(wargv);
            return NULL;
        }

        if (WideCharToMultiByte(
                    CP_UTF8, 0, wargv[i], -1, uargv[i], n, NULL, NULL) <= 0) {
            int j;
            for (j = 0; j <= i; j++)
                free(uargv[j]);
            free(uargv);
            LocalFree(wargv);
            errno = EINVAL;
            return NULL;
        }
    }

    uargv[wargc] = NULL;
    LocalFree(wargv);
    *argc_out = wargc;
    return uargv;
}

static void win32_free_args_utf8(char **argv, int argc)
{
    int i;
    if (!argv)
        return;
    for (i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

#endif /* _WIN32 */

/* --- Processing -------------------------------------------------------- */

static int process_file(wc *w, const char *path, run_stats *stats)
{
    char buf[STDIN_CHUNK];
    wc_stream *st = NULL;
    FILE *fp = NULL;
    int rc = -1;
    int open_rc = WC_OK;
    uintmax_t total_bytes = 0;

#ifdef _WIN32
    fp = win32_fopen_utf8(path, L"rb");
#else
    fp = fopen(path, "rb");
#endif
    if (!fp) {
        diag_path_errno(path, errno);
        if (stats)
            stats->files_failed++;
        return -1;
    }

    st = wc_stream_open(w, &open_rc);
    if (!st) {
        diag_path_wc(path, open_rc);
        (void)fclose(fp);
        if (stats)
            stats->files_failed++;
        return -1;
    }

    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, fp);
        if (n > 0) {
            size_t consumed = 0;
            int scan_rc = wc_stream_scan_ex(st, buf, n, &consumed);
            if (add_uintmax_checked(&total_bytes, consumed) < 0) {
                diag_path_msg(path, "input too large");
                goto done;
            }
            if (scan_rc != WC_OK) {
                diag_path_wc_after(path, scan_rc, total_bytes);
                goto done;
            }
        }

        if (n < sizeof buf) {
            if (ferror(fp)) {
                int e = errno;
                if (e) {
                    diag_path_errno(path, e);
                } else {
                    diag_path_msg(path, "I/O error");
                }
                goto done;
            }
            break;
        }
    }

    rc = wc_stream_finish(st);
    if (rc != WC_OK) {
        diag_path_wc(path, rc);
        rc = -1;
        goto done;
    }

    rc = 0;

done:
    if (rc == 0 && stats &&
        add_uintmax_checked(&stats->bytes_processed, total_bytes) < 0) {
        diag_path_msg(path, "input too large");
        rc = -1;
    }
    if (st)
        wc_stream_close(st);
    if (fp && fclose(fp) != 0) {
        if (rc == 0) {
            diag_path_errno(path, errno);
            rc = -1;
        }
    }
    if (stats) {
        if (rc == 0)
            stats->files_processed++;
        else
            stats->files_failed++;
    }
    return rc;
}

static int process_stdin(wc *w, run_stats *stats)
{
    char buf[STDIN_CHUNK];
    wc_stream *st = NULL;
    int rc;
    int open_rc = WC_OK;
    uintmax_t total_bytes = 0;

    st = wc_stream_open(w, &open_rc);
    if (!st) {
        (void)fprintf(stderr, "wc: <stdin>: %s\n", wc_errstr(open_rc));
        if (stats)
            stats->files_failed++;
        return -1;
    }

    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, stdin);

        if (n > 0) {
            size_t consumed = 0;
            rc = wc_stream_scan_ex(st, buf, n, &consumed);
            if (add_uintmax_checked(&total_bytes, consumed) < 0) {
                (void)fprintf(stderr, "wc: <stdin>: input too large\n");
                wc_stream_close(st);
                if (stats)
                    stats->files_failed++;
                return -1;
            }
            if (rc != WC_OK) {
                (void)fprintf(stderr,
                              "wc: <stdin>: %s (after %" PRIuMAX " bytes)\n",
                              wc_errstr(rc),
                              total_bytes);
                wc_stream_close(st);
                if (stats)
                    stats->files_failed++;
                return -1;
            }
        }

        if (n < sizeof buf) {
            if (ferror(stdin)) {
                int e = errno;
                if (e)
                    (void)fprintf(stderr, "wc: <stdin>: %s\n", strerror(e));
                else
                    (void)fprintf(stderr, "wc: <stdin>: I/O error\n");
                wc_stream_close(st);
                if (stats)
                    stats->files_failed++;
                return -1;
            }
            break;
        }
    }

    rc = wc_stream_finish(st);
    if (rc != WC_OK) {
        (void)fprintf(stderr, "wc: <stdin>: %s\n", wc_errstr(rc));
        wc_stream_close(st);
        if (stats)
            stats->files_failed++;
        return -1;
    }

    wc_stream_close(st);
    if (stats) {
        if (add_uintmax_checked(&stats->bytes_processed, total_bytes) < 0) {
            (void)fprintf(stderr, "wc: <stdin>: input too large\n");
            stats->files_failed++;
            return -1;
        }
        stats->files_processed++;
    }
    return 0;
}

/* --- Output ------------------------------------------------------------ */

typedef struct {
    size_t total;
    size_t unique;
    size_t filtered;
    size_t displayed;
    uintmax_t bytes;
} summary_info;

typedef struct {
    size_t rank_w;
    size_t count_w;
    size_t word_w;
} column_widths;

static size_t digits_size(size_t v)
{
    size_t d = 1;
    while (v >= 10u) {
        v /= 10u;
        d++;
    }
    return d;
}

static column_widths measure_columns(const wc_word *words, size_t n)
{
    column_widths w = { 1, 5, 4 };

    for (size_t i = 0; i < n; i++) {
        size_t rank_digits = digits_size(i + 1u);
        size_t count_digits = digits_size(words[i].count);
        size_t word_len = strlen(words[i].word);

        if (word_len > TABLE_WORD_WIDTH_LIMIT)
            word_len = TABLE_WORD_WIDTH_LIMIT;
        if (rank_digits > w.rank_w)
            w.rank_w = rank_digits;
        if (count_digits > w.count_w)
            w.count_w = count_digits;
        if (word_len > w.word_w)
            w.word_w = word_len;
    }

    return w;
}

static int use_color(const cli_opts *opts)
{
    if (opts->color == COLOR_ALWAYS)
        return 1;
    if (opts->color == COLOR_NEVER)
        return 0;
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

static int print_spaces(size_t n)
{
    while (n > 0) {
        if (cli_putc(stdout, ' ') < 0)
            return -1;
        n--;
    }
    return 0;
}

static int print_size_right(size_t v, size_t width)
{
    size_t digits = digits_size(v);

    if (width > digits && print_spaces(width - digits) < 0)
        return -1;
    return cli_fprintf(stdout, "%zu", v);
}

static int print_string_right(const char *s, size_t width)
{
    size_t n = strlen(s);

    if (width > n && print_spaces(width - n) < 0)
        return -1;
    return cli_fputs(stdout, s);
}

static int print_table(const wc_word *words, size_t n, int color)
{
    column_widths w = measure_columns(words, n);
    const char *emph = color ? "\x1b[1m" : "";
    const char *reset = color ? "\x1b[0m" : "";
    size_t sep_len = w.rank_w + w.count_w + w.word_w + 4u;

    if (cli_fputs(stdout, emph) < 0 || print_string_right("#", w.rank_w) < 0 ||
        cli_fputs(stdout, "  ") < 0 ||
        print_string_right("count", w.count_w) < 0 ||
        cli_fputs(stdout, "  ") < 0 || cli_fputs(stdout, "word") < 0 ||
        cli_fputs(stdout, reset) < 0 || cli_putc(stdout, '\n') < 0) {
        return -1;
    }

    for (size_t i = 0; i < sep_len; i++) {
        if (cli_putc(stdout, '-') < 0)
            return -1;
    }
    if (cli_putc(stdout, '\n') < 0)
        return -1;

    for (size_t i = 0; i < n; i++) {
        if (print_size_right(i + 1u, w.rank_w) < 0 ||
            cli_fputs(stdout, "  ") < 0 ||
            print_size_right(words[i].count, w.count_w) < 0 ||
            cli_fputs(stdout, "  ") < 0 ||
            cli_fputs(stdout, words[i].word) < 0 ||
            cli_putc(stdout, '\n') < 0) {
            return -1;
        }
    }
    return 0;
}

static int print_tsv(const wc_word *words, size_t n)
{
    if (cli_fputs(stdout, "rank\tcount\tword\n") < 0)
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (cli_fprintf(stdout,
                        "%zu\t%zu\t%s\n",
                        i + 1u,
                        words[i].count,
                        words[i].word) < 0) {
            return -1;
        }
    }
    return 0;
}

static int print_json_string(const char *s)
{
    static const char hex[] = "0123456789abcdef";

    if (cli_putc(stdout, '"') < 0)
        return -1;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        switch (c) {
            case '"':
                if (cli_fputs(stdout, "\\\"") < 0)
                    return -1;
                break;
            case '\\':
                if (cli_fputs(stdout, "\\\\") < 0)
                    return -1;
                break;
            case '\b':
                if (cli_fputs(stdout, "\\b") < 0)
                    return -1;
                break;
            case '\f':
                if (cli_fputs(stdout, "\\f") < 0)
                    return -1;
                break;
            case '\n':
                if (cli_fputs(stdout, "\\n") < 0)
                    return -1;
                break;
            case '\r':
                if (cli_fputs(stdout, "\\r") < 0)
                    return -1;
                break;
            case '\t':
                if (cli_fputs(stdout, "\\t") < 0)
                    return -1;
                break;
            default:
                if (c < 0x20u || c >= 0x80u) {
                    if (cli_fputs(stdout, "\\u00") < 0 ||
                        cli_putc(stdout, hex[(c >> 4) & 0x0fu]) < 0 ||
                        cli_putc(stdout, hex[c & 0x0fu]) < 0) {
                        return -1;
                    }
                } else if (cli_putc(stdout, c) < 0) {
                    return -1;
                }
                break;
        }
    }
    return cli_putc(stdout, '"');
}

static int
print_json(const wc_word *words, size_t n, const summary_info *summary)
{
    if (cli_fputs(stdout, "{\"words\":[") < 0)
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (cli_fprintf(stdout,
                        "%s{\"rank\":%zu,\"count\":%zu,\"word\":",
                        (i == 0) ? "" : ",",
                        i + 1u,
                        words[i].count) < 0 ||
            print_json_string(words[i].word) < 0 || cli_putc(stdout, '}') < 0) {
            return -1;
        }
    }
    return cli_fprintf(stdout,
                       "],\"summary\":{"
                       "\"total\":%zu,"
                       "\"unique\":%zu,"
                       "\"filtered\":%zu,"
                       "\"displayed\":%zu,"
                       "\"bytes\":%" PRIuMAX "}}\n",
                       summary->total,
                       summary->unique,
                       summary->filtered,
                       summary->displayed,
                       summary->bytes);
}

static int print_summary_text(const summary_info *summary, FILE *out)
{
    return cli_fprintf(out,
                       "Total: %zu  Unique: %zu  Filtered: %zu  Shown: %zu  "
                       "Bytes: %" PRIuMAX "\n",
                       summary->total,
                       summary->unique,
                       summary->filtered,
                       summary->displayed,
                       summary->bytes);
}

static int word_passes_filters(const char *word, const cli_opts *opts)
{
    size_t wl = strlen(word);

    if (opts->has_min_len && wl < opts->min_len)
        return 0;
    if (opts->has_max_len && wl > opts->max_len)
        return 0;
    return 1;
}

static size_t count_filtered_cursor(const wc *w, const cli_opts *opts)
{
    wc_cursor cursor;
    size_t kept = 0;

    wc_cursor_init(&cursor, w);
    for (;;) {
        const char *word = NULL;
        if (!wc_cursor_next(&cursor, &word, NULL))
            break;
        if (word_passes_filters(word, opts))
            kept++;
    }
    return kept;
}

static size_t
filter_results(wc_word *words, size_t len, const cli_opts *opts, size_t *shown)
{
    size_t kept = 0;

    for (size_t i = 0; i < len; i++) {
        if (!word_passes_filters(words[i].word, opts))
            continue;
        words[kept++] = words[i];
    }

    if (shown) {
        size_t display = opts->show_all ? kept : opts->topn;
        if (!opts->show_all && display > kept)
            display = kept;
        *shown = display;
    }

    return kept;
}

static const char *yn(int v)
{
    return v ? "yes" : "no";
}

static int
build_cfg_has_field(const wc_build_config *cfg, size_t offset, size_t size)
{
    return cfg != NULL && offset <= WC_SIZE_MAX - size &&
           cfg->struct_size >= offset + size;
}

#define BUILD_CFG_HAS(cfg_, field_) \
    build_cfg_has_field(            \
            (cfg_), offsetof(wc_build_config, field_), sizeof((cfg_)->field_))

/* --- Main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    wc *w = NULL;
    int i;
    int err = 0;
    int rc = 1;
    wc_limits lim;
    int have_limits = 0;
    int open_rc = WC_OK;
    cli_opts opts;
    const wc_build_config *build = NULL;
    int first_file = 1;
    size_t max_word = 0;
    run_stats stats = { 0, 0, 0 };
    wc_word *words = NULL;
    size_t words_len = 0;
    size_t filtered_len = 0;
    size_t display_len = 0;
    summary_info summary = { 0, 0, 0, 0, 0 };

#if !defined(_WIN32) && defined(SIGPIPE)
    (void)signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _WIN32
    int argc_win = 0;
    char **argv_win = NULL;

    argv_win = win32_get_args_utf8(&argc_win);
    if (!argv_win) {
        (void)fprintf(stderr,
                      "wc: initialization failed (%s)\n",
                      strerror(errno ? errno : ENOMEM));
        return 1;
    }
    argc = argc_win;
    argv = argv_win;
#endif

    build = wc_build_info();
    wc_limits_init(&lim);

    if (parse_cli_opts(argc, argv, &opts, &first_file) < 0) {
        (void)print_usage(stderr);
        rc = 2;
        goto cleanup;
    }

    if (opts.show_help) {
        rc = (print_usage(stdout) == 0 && cli_flush(stdout) == 0) ? 0 : 1;
        goto cleanup;
    }

    if (opts.show_version) {
        int out_rc = cli_fprintf(stdout, "%s\n", wc_version());
        if (BUILD_CFG_HAS(build, trust_static_buffer_alignment)) {
            if (out_rc == 0) {
                out_rc = cli_fprintf(
                        stdout,
                        "features: hosted=%s, libc-string=%s, libc-qsort=%s, "
                        "errno=%s, no-heap=%s, hash-strong=%s, uintptr=%s, "
                        "trust-static-alignment=%s\n",
                        yn(build->hosted),
                        yn(build->use_libc_string),
                        yn(build->use_libc_qsort),
                        yn(build->have_errno),
                        yn(build->no_heap),
                        yn(build->hash_strong),
                        yn(build->have_uintptr),
                        yn(build->trust_static_buffer_alignment));
            }
        }
        rc = (out_rc == 0 && cli_flush(stdout) == 0) ? 0 : 1;
        goto cleanup;
    }

    if ((BUILD_CFG_HAS(build, hosted) && !build->hosted) ||
        (BUILD_CFG_HAS(build, no_heap) && build->no_heap)) {
        (void)fprintf(stderr,
                      "wc: CLI requires a hosted heap-enabled wordcount "
                      "library\n");
        rc = 1;
        goto cleanup;
    }

    if (opts.has_max_bytes) {
        lim.max_bytes = opts.max_bytes;
        have_limits = 1;
    } else {
        have_limits = parse_wc_limits_from_env(&lim);
        if (have_limits < 0) {
            (void)fprintf(stderr,
                          "wc: invalid WC_MAX_BYTES value (must be "
                          "non-negative integer)\n");
            rc = 1;
            goto cleanup;
        }
    }
    if (opts.strict_max_bytes && lim.max_bytes == 0) {
        (void)fprintf(stderr,
                      "wc: --strict-max-bytes requires --max-bytes N or "
                      "WC_MAX_BYTES with N > 0\n");
        rc = 2;
        goto cleanup;
    }
    if (opts.strict_max_bytes) {
        lim.strict_max_bytes = 1;
        have_limits = 1;
    }
    if (opts.has_max_word)
        max_word = opts.max_word;

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    w = wc_open_ex(max_word, (have_limits > 0) ? &lim : NULL, &open_rc);
    if (!w) {
        (void)fprintf(stderr, "wc: %s\n", wc_errstr(open_rc));
        goto cleanup;
    }

    if (first_file >= argc) {
        if (process_stdin(w, &stats) < 0)
            err = 1;
    } else {
        for (i = first_file; i < argc; i++) {
            if (process_file(w, argv[i], &stats) < 0)
                err = 1;
        }
    }

    if (err)
        goto finalize;

    summary.total = wc_total(w);
    summary.unique = wc_unique(w);
    summary.bytes = stats.bytes_processed;

    if (opts.quiet) {
        filtered_len = (opts.has_min_len || opts.has_max_len)
                               ? count_filtered_cursor(w, &opts)
                               : summary.unique;
        display_len = 0;
    } else if (!opts.show_all && !opts.has_min_len && !opts.has_max_len) {
        int topn_rc = wc_topn(w, opts.topn, &words, &words_len);
        if (topn_rc != WC_OK) {
            (void)fprintf(stderr, "wc: %s\n", wc_errstr(topn_rc));
            err = 1;
            goto finalize;
        }
        filtered_len = summary.unique;
        display_len = words_len;
    } else {
        int results_rc = wc_results(w, &words, &words_len);
        if (results_rc != WC_OK) {
            (void)fprintf(stderr, "wc: %s\n", wc_errstr(results_rc));
            err = 1;
            goto finalize;
        }
        filtered_len = filter_results(words, words_len, &opts, &display_len);
    }
    summary.filtered = filtered_len;
    summary.displayed = opts.quiet ? 0 : display_len;

    if (!opts.quiet && filtered_len == 0 && opts.format != FORMAT_JSON &&
        cli_fputs(stderr, "No words found.\n") < 0) {
        err = 1;
        goto finalize;
    }

    {
        int color = use_color(&opts);
        size_t emit = opts.quiet ? 0 : display_len;

        switch (opts.format) {
            case FORMAT_TABLE:
                if (emit > 0 && print_table(words, emit, color) < 0)
                    err = 1;
                if (!err && print_summary_text(&summary, stdout) < 0)
                    err = 1;
                break;
            case FORMAT_TSV:
                if (emit > 0 && print_tsv(words, emit) < 0)
                    err = 1;
                if (!err && print_summary_text(&summary, stderr) < 0)
                    err = 1;
                break;
            case FORMAT_JSON: {
                summary_info json_summary = summary;
                json_summary.displayed = emit;
                json_summary.filtered = filtered_len;
                if (print_json(words, emit, &json_summary) < 0)
                    err = 1;
                break;
            }
        }
    }
    if (!err && (cli_flush(stdout) < 0 || cli_flush(stderr) < 0))
        err = 1;

finalize:
    wc_results_free(words);
    rc = err ? 1 : 0;

cleanup:
    wc_close(w);
#ifdef _WIN32
    if (argv_win)
        win32_free_args_utf8(argv_win, argc_win);
#endif
    return rc;
}

#else
extern int wc_main_disabled_translation_unit;
int wc_main_disabled_translation_unit = 0;
#endif /* WC_NO_HOSTED_MAIN */

/*
** wc_main.c - Command-line interface
**
** Public domain.
**
** DESIGN
**
**   Uses memory-mapped I/O for zero-copy file access, enabling
**   processing of files larger than physical RAM. Platform-specific
**   code is isolated in the os_* functions.
**
**   Stdin is processed via the library's streaming API (wc_stream_*),
**   keeping host memory bounded and ensuring tokenization stays in sync
**   with wc_scan().
**
**   NOTE ABOUT BUILD MISMATCHES
**
**   wc_main.c is compiled against wordcount.h but may be linked against a
**   separately-built wordcount.c with different build macros. The stdin path
**   uses the library streaming API (wc_stream_*), avoiding duplicated
**   tokenization logic in the CLI.
**
** Usage: wc [file ...]
**   Reads stdin if no files given. Top 10 to stdout, summary to stderr.
**
** Environment:
**   WC_MAX_BYTES  - Optional hard cap on internal heap usage for
**                   the wc object, in bytes (e.g. "8388608" for 8MB).
**                   Must be a non-negative decimal integer.
*/
#ifndef WC_NO_HOSTED_MAIN

#include "wordcount.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <wchar.h>

static wchar_t *utf8_to_wide(const char *utf8);
#else
#include <unistd.h>
#endif

#define TOPN 25
#define STDIN_CHUNK 65536

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
    int no_mmap;
    int show_version;
    int show_help;
    size_t max_bytes;
    int has_max_bytes;
    int strict_max_bytes;
    int summary;
    int quiet;
    output_format format;
    color_mode color;
} cli_opts;

typedef struct {
    size_t bytes_processed;
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

static void print_usage(void)
{
    (void)fprintf(stdout,
                  "Usage: wc [OPTIONS] [FILE|-]\n"
                  "  Reads FILEs (or stdin) and reports word frequencies.\n"
                  "\n"
                  "Output:\n"
                  "  -n, --top N           Show top N words (default %d)\n"
                  "      --all             Show all unique words\n"
                  "      --format {table,tsv,json}\n"
                  "      --summary         Print totals after the list\n"
                  "  -q, --quiet           Summary only (implies --summary)\n"
                  "      --color {auto,always,never}\n"
                  "      --no-color        Disable color entirely\n"
                  "\n"
                  "Parsing:\n"
                  "      --min-len N       Exclude words shorter than N\n"
                  "      --max-len N       Exclude words longer than N\n"
                  "      --max-word N      Set library max_word limit\n"
                  "\n"
                  "Memory:\n"
                  "      --max-bytes N     Limit internal bytes (overrides "
                  "WC_MAX_BYTES)\n"
                  "      --strict-max-bytes Enforce a hard cap (no transient "
                  "spikes)\n"
                  "      --no-mmap         Stream files instead of mmap\n"
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
    opts->no_mmap = 0;
    opts->show_version = 0;
    opts->show_help = 0;
    opts->max_bytes = 0;
    opts->has_max_bytes = 0;
    opts->strict_max_bytes = 0;
    opts->summary = 1;
    opts->quiet = 0;
    opts->format = FORMAT_TABLE;
    opts->color = COLOR_AUTO;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-')
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
        } else if (strcmp(arg, "--no-mmap") == 0) {
            opts->no_mmap = 1;
        } else if (strcmp(arg, "--all") == 0) {
            opts->show_all = 1;
        } else if (strcmp(arg, "--summary") == 0) {
            opts->summary = 1;
        } else if (strcmp(arg, "--strict-max-bytes") == 0) {
            opts->strict_max_bytes = 1;
        } else if (strcmp(arg, "--no-color") == 0) {
            opts->color = COLOR_NEVER;
        } else if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0) {
            opts->quiet = 1;
            opts->summary = 1;
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
                (void)fprintf(stderr, "wc: unknown format: %s\n", fmt);
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
                (void)fprintf(stderr, "wc: unknown color mode: %s\n", val);
                return -1;
            }
        } else {
            (void)fprintf(stderr, "wc: unknown option: %s\n", arg);
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

/* stdin streaming is now implemented by the library via wc_stream_* */

/* --- Platform abstraction for memory-mapped files --------------------- */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>

typedef struct {
    void *data;
    size_t size;
    HANDLE hFile;
    HANDLE hMap;
} MappedFile;

static void set_errno_from_win32(void)
{
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        errno = ENOENT;
    } else if (err == ERROR_ACCESS_DENIED) {
        errno = EACCES;
    } else if (err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_OUTOFMEMORY) {
        errno = ENOMEM;
    } else if (err == ERROR_NO_UNICODE_TRANSLATION) {
        errno = EINVAL;
    } else {
        errno = EIO;
    }
}

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

static int os_map(MappedFile *mf, const char *path)
{
    LARGE_INTEGER sz;
    wchar_t *wpath;

    if (!mf || !path) {
        errno = EINVAL;
        return -1;
    }

    memset(mf, 0, sizeof *mf);
    mf->hFile = INVALID_HANDLE_VALUE;
    mf->hMap = NULL;

    wpath = utf8_to_wide(path);
    if (!wpath)
        return -1;

    mf->hFile = CreateFileW(wpath,
                            GENERIC_READ,
                            FILE_SHARE_READ,
                            NULL,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);
    free(wpath);

    if (mf->hFile == INVALID_HANDLE_VALUE) {
        set_errno_from_win32();
        return -1;
    }

    if (!GetFileSizeEx(mf->hFile, &sz)) {
        set_errno_from_win32();
        CloseHandle(mf->hFile);
        mf->hFile = INVALID_HANDLE_VALUE;
        return -1;
    }

    if (sz.QuadPart == 0) {
        CloseHandle(mf->hFile);
        mf->hFile = INVALID_HANDLE_VALUE;
        return 0;
    }

    if (sz.QuadPart < 0 ||
        (unsigned long long)sz.QuadPart > (unsigned long long)SIZE_MAX) {
        CloseHandle(mf->hFile);
        mf->hFile = INVALID_HANDLE_VALUE;
        errno = EFBIG;
        return -1;
    }

    mf->hMap = CreateFileMappingW(mf->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mf->hMap) {
        set_errno_from_win32();
        CloseHandle(mf->hFile);
        mf->hFile = INVALID_HANDLE_VALUE;
        return -1;
    }

    mf->data = MapViewOfFile(mf->hMap, FILE_MAP_READ, 0, 0, 0);
    if (!mf->data) {
        set_errno_from_win32();
        CloseHandle(mf->hMap);
        CloseHandle(mf->hFile);
        mf->hMap = NULL;
        mf->hFile = INVALID_HANDLE_VALUE;
        return -1;
    }

    mf->size = (size_t)sz.QuadPart;
    return 0;
}

static void os_unmap(MappedFile *mf)
{
    if (!mf)
        return;

    if (mf->data)
        UnmapViewOfFile(mf->data);
    if (mf->hMap)
        CloseHandle(mf->hMap);
    if (mf->hFile != INVALID_HANDLE_VALUE)
        CloseHandle(mf->hFile);

    memset(mf, 0, sizeof *mf);
    mf->hFile = INVALID_HANDLE_VALUE;
}

#else /* POSIX */

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    void *data;
    size_t size;
    int fd;
} MappedFile;

static int os_map(MappedFile *mf, const char *path)
{
    struct stat st;
    int saved_errno;

    if (!mf || !path) {
        errno = EINVAL;
        return -1;
    }

    memset(mf, 0, sizeof *mf);
    mf->fd = -1;

#ifdef O_CLOEXEC
    mf->fd = open(path, O_RDONLY | O_CLOEXEC);
#else
    mf->fd = open(path, O_RDONLY);
#endif
    if (mf->fd < 0)
        return -1;

#if !defined(O_CLOEXEC) && defined(FD_CLOEXEC)
    /* Best-effort fallback; ignore failures. */
    (void)fcntl(mf->fd, F_SETFD, FD_CLOEXEC);
#endif

    if (fstat(mf->fd, &st) < 0) {
        saved_errno = errno;
        close(mf->fd);
        mf->fd = -1;
        errno = saved_errno;
        return -1;
    }

    if (st.st_size < 0) {
        close(mf->fd);
        mf->fd = -1;
        errno = EFBIG;
        return -1;
    }

    if (st.st_size == 0) {
        close(mf->fd);
        mf->fd = -1;
        return 0;
    }

    if ((off_t)(size_t)st.st_size != st.st_size) {
        close(mf->fd);
        mf->fd = -1;
        errno = EFBIG;
        return -1;
    }

    mf->data =
            mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, mf->fd, 0);
    if (mf->data == MAP_FAILED) {
        saved_errno = errno;
        mf->data = NULL;
        close(mf->fd);
        mf->fd = -1;
        errno = saved_errno;
        return -1;
    }

#ifdef MADV_SEQUENTIAL
    (void)madvise(mf->data, (size_t)st.st_size, MADV_SEQUENTIAL);
#endif

    mf->size = (size_t)st.st_size;
    return 0;
}

static void os_unmap(MappedFile *mf)
{
    if (!mf)
        return;

    if (mf->data && mf->size > 0)
        munmap(mf->data, mf->size);
    if (mf->fd >= 0)
        close(mf->fd);

    memset(mf, 0, sizeof *mf);
    mf->fd = -1;
}

#endif /* _WIN32 */

/* --- Processing -------------------------------------------------------- */

static int process_file_streaming(wc *w, const char *path, run_stats *stats);

static int process_mapped(wc *w,
                          const char *data,
                          size_t size,
                          const char *name,
                          run_stats *stats)
{
    int rc = wc_scan(w, data, size);
    if (rc != WC_OK) {
        (void)fprintf(stderr, "wc: %s: %s\n", name, wc_errstr(rc));
        return -1;
    }
    if (stats)
        stats->bytes_processed += size;
    return 0;
}

static int
process_file(wc *w, const char *path, int allow_mmap, run_stats *stats)
{
    MappedFile mf;
    int rc = -1;

    memset(&mf, 0, sizeof mf);

    if (!allow_mmap || os_map(&mf, path) < 0) {
        rc = process_file_streaming(w, path, stats);
        if (stats) {
            if (rc == 0)
                stats->files_processed++;
            else
                stats->files_failed++;
        }
        return rc;
    }

    if (mf.size == 0) {
        rc = 0;
        goto cleanup;
    }

    rc = process_mapped(w, (const char *)mf.data, mf.size, path, stats);

cleanup:
    os_unmap(&mf);
    if (stats) {
        if (rc == 0)
            stats->files_processed++;
        else
            stats->files_failed++;
    }
    return rc;
}

static int process_file_streaming(wc *w, const char *path, run_stats *stats)
{
    char buf[STDIN_CHUNK];
    wc_stream *st = NULL;
    FILE *fp = NULL;
    int rc = -1;
    int open_rc = WC_OK;
    size_t total_bytes = 0;

#ifdef _WIN32
    fp = win32_fopen_utf8(path, L"rb");
#else
    fp = fopen(path, "rb");
#endif
    if (!fp) {
        (void)fprintf(stderr, "wc: %s: %s\n", path, strerror(errno));
        return -1;
    }

    st = wc_stream_open(w, &open_rc);
    if (!st) {
        (void)fprintf(stderr, "wc: %s: %s\n", path, wc_errstr(open_rc));
        fclose(fp);
        return -1;
    }

    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, fp);
        if (n > 0) {
            size_t consumed = 0;
            int scan_rc = wc_stream_scan_ex(st, buf, n, &consumed);
            total_bytes += consumed;
            if (scan_rc != WC_OK) {
                (void)fprintf(stderr,
                              "wc: %s: %s (after %zu bytes)\n",
                              path,
                              wc_errstr(scan_rc),
                              total_bytes);
                goto done;
            }
        }

        if (n < sizeof buf) {
            if (ferror(fp)) {
                int e = errno;
                (void)fprintf(stderr,
                              "wc: %s: %s\n",
                              path,
                              e ? strerror(e) : "I/O error");
                goto done;
            }
            break;
        }
    }

    rc = wc_stream_finish(st);
    if (rc != WC_OK) {
        (void)fprintf(stderr, "wc: %s: %s\n", path, wc_errstr(rc));
        rc = -1;
        goto done;
    }

    rc = 0;

done:
    if (rc == 0 && stats)
        stats->bytes_processed += total_bytes;
    if (st)
        wc_stream_close(st);
    if (fp)
        fclose(fp);
    return rc;
}

static int process_stdin(wc *w, run_stats *stats)
{
    char buf[STDIN_CHUNK];
    wc_stream *st = NULL;
    int rc;
    int open_rc = WC_OK;
    size_t total_bytes = 0;

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
            total_bytes += consumed;
            if (rc != WC_OK) {
                (void)fprintf(stderr,
                              "wc: <stdin>: %s (after %zu bytes)\n",
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
        stats->bytes_processed += total_bytes;
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
    size_t bytes;
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

static void print_table(const wc_word *words, size_t n, int color)
{
    column_widths w = measure_columns(words, n);
    const char *emph = color ? "\x1b[1m" : "";
    const char *reset = color ? "\x1b[0m" : "";
    size_t sep_len = w.rank_w + w.count_w + w.word_w + 4u;

    (void)printf("%s%*s  %*s  %-*s%s\n",
                 emph,
                 (int)w.rank_w,
                 "#",
                 (int)w.count_w,
                 "count",
                 (int)w.word_w,
                 "word",
                 reset);
    for (size_t i = 0; i < sep_len; i++)
        putchar('-');
    putchar('\n');

    for (size_t i = 0; i < n; i++) {
        (void)printf("%*zu  %*zu  %-*s\n",
                     (int)w.rank_w,
                     i + 1u,
                     (int)w.count_w,
                     words[i].count,
                     (int)w.word_w,
                     words[i].word);
    }
}

static void print_tsv(const wc_word *words, size_t n)
{
    (void)printf("rank\tcount\tword\n");
    for (size_t i = 0; i < n; i++) {
        (void)printf("%zu\t%zu\t%s\n", i + 1u, words[i].count, words[i].word);
    }
}

static void
print_json(const wc_word *words, size_t n, const summary_info *summary)
{
    (void)printf("{\"words\":[");
    for (size_t i = 0; i < n; i++) {
        (void)printf("%s{\"rank\":%zu,\"count\":%zu,\"word\":\"%s\"}",
                     (i == 0) ? "" : ",",
                     i + 1u,
                     words[i].count,
                     words[i].word);
    }
    (void)printf("],\"summary\":{"
                 "\"total\":%zu,"
                 "\"unique\":%zu,"
                 "\"filtered\":%zu,"
                 "\"displayed\":%zu,"
                 "\"bytes\":%zu}}\n",
                 summary->total,
                 summary->unique,
                 summary->filtered,
                 summary->displayed,
                 summary->bytes);
}

static void print_summary_text(const summary_info *summary, FILE *out)
{
    (void)fprintf(out,
                  "Total: %zu  Unique: %zu  Filtered: %zu  Shown: %zu  Bytes: "
                  "%zu\n",
                  summary->total,
                  summary->unique,
                  summary->filtered,
                  summary->displayed,
                  summary->bytes);
}

static size_t
filter_results(wc_word *words, size_t len, const cli_opts *opts, size_t *shown)
{
    size_t kept = 0;

    for (size_t i = 0; i < len; i++) {
        size_t wl = strlen(words[i].word);
        if (opts->has_min_len && wl < opts->min_len)
            continue;
        if (opts->has_max_len && wl > opts->max_len)
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

/* --- Main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    wc *w = NULL;
    int i;
    int err = 0;
    int rc = 1;
    wc_limits lim;
    int have_limits;
    int open_rc = WC_OK;
    cli_opts opts;
    int first_file = 1;
    size_t max_word = 0;
    run_stats stats = { 0, 0, 0 };
    wc_word *words = NULL;
    size_t words_len = 0;
    size_t filtered_len = 0;
    size_t display_len = 0;
    summary_info summary = { 0, 0, 0, 0, 0 };

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

#if !WC_BOOL(WC_ASCII_ONLY)
    /* Force deterministic ctype classification; not Unicode-aware. */
    setlocale(LC_CTYPE, "C");
#endif

    have_limits = parse_wc_limits_from_env(&lim);
    if (have_limits < 0) {
        (void)fprintf(stderr,
                      "wc: invalid WC_MAX_BYTES value (must be non-negative "
                      "integer)\n");
        rc = 1;
        goto cleanup;
    }

    if (parse_cli_opts(argc, argv, &opts, &first_file) < 0) {
        print_usage();
        rc = 2;
        goto cleanup;
    }

    if (opts.show_help) {
        print_usage();
        rc = 0;
        goto cleanup;
    }

    if (opts.show_version) {
        (void)printf("%s\n", wc_version());
        (void)printf("features: hosted=%s, libc-string=%s, libc-qsort=%s, "
                     "errno=%s, ascii-only=%s, stream-reuse=%s, no-heap=%s\n",
                     yn(WC_STDC_HOSTED != 0),
                     yn(WC_USE_LIBC_STRING != 0),
                     yn(WC_USE_LIBC_QSORT != 0),
                     yn(WC_HAVE_ERRNO != 0),
                     yn(WC_ASCII_ONLY != 0),
                     yn(WC_STREAM_REUSE_SCANBUF != 0),
                     yn(WC_NO_HEAP != 0));
        rc = 0;
        goto cleanup;
    }

    if (opts.has_max_bytes) {
        lim.max_bytes = opts.max_bytes;
        have_limits = 1;
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
            if (process_file(w, argv[i], opts.no_mmap ? 0 : 1, &stats) < 0)
                err = 1;
        }
    }

    summary.total = wc_total(w);
    summary.unique = wc_unique(w);
    summary.bytes = stats.bytes_processed;

    {
        int results_rc = wc_results(w, &words, &words_len);
        if (results_rc != WC_OK) {
            (void)fprintf(stderr, "wc: %s\n", wc_errstr(results_rc));
            err = 1;
            goto finalize;
        }
    }

    filtered_len = filter_results(words, words_len, &opts, &display_len);
    summary.filtered = filtered_len;
    summary.displayed = opts.quiet ? 0 : display_len;

    if (!opts.quiet && filtered_len == 0 && opts.format != FORMAT_JSON)
        (void)fprintf(stderr, "No words found.\n");

    {
        int color = use_color(&opts);
        size_t emit = opts.quiet ? 0 : display_len;

        switch (opts.format) {
            case FORMAT_TABLE:
                if (emit > 0)
                    print_table(words, emit, color);
                if (opts.summary)
                    print_summary_text(&summary, stdout);
                break;
            case FORMAT_TSV:
                if (emit > 0)
                    print_tsv(words, emit);
                if (opts.summary)
                    print_summary_text(&summary, stderr);
                break;
            case FORMAT_JSON: {
                summary_info json_summary = summary;
                json_summary.displayed = emit;
                json_summary.filtered = filtered_len;
                print_json(words, emit, &json_summary);
                break;
            }
            default:
                break;
        }
    }

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

#endif /* WC_NO_HOSTED_MAIN */

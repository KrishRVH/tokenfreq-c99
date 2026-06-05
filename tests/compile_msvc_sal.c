/*
** Force the public header through its MSVC/SAL warn-unused-result branch with a
** prefix-only stand-in. This catches declarations that incorrectly place WC_WUR
** after a function declarator.
*/
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#undef WC_STDC_HOSTED
#define WC_STDC_HOSTED 0
#undef WC_NO_HEAP
#define WC_NO_HEAP 1
#undef WC_USE_LIBC_QSORT
#define WC_USE_LIBC_QSORT 0
#undef WC_USE_LIBC_STRING
#define WC_USE_LIBC_STRING 0
#undef WC_HAVE_ERRNO
#define WC_HAVE_ERRNO 0

#ifdef __GNUC__
#undef __GNUC__
#endif
#ifdef __clang__
#undef __clang__
#endif

#ifndef _MSC_VER
/* NOLINTNEXTLINE(bugprone-reserved-identifier): intentionally simulates MSVC. */
#define _MSC_VER 1930
#endif
#ifndef _Check_return_
/* NOLINTNEXTLINE(bugprone-reserved-identifier): intentionally simulates SAL. */
#define _Check_return_ extern
#endif

#include "wordcount.h"

int wc_compile_msvc_sal_probe(void)
{
    return 0;
}

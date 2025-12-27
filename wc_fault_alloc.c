#include "wc_fault_alloc.h"

#include <errno.h>
#include <stdlib.h>

static int g_fail_target = 0;
static int g_alloc_count = 0;
static int g_active = 0;

void fault_reset(void)
{
    g_fail_target = 0;
    g_alloc_count = 0;
    g_active = 0;
}

void fault_arm(int n)
{
    g_fail_target = n;
    g_alloc_count = 0;
    g_active = 1;
}

static int fault_should_fail(void)
{
    if (!g_active)
        return 0;
    g_alloc_count++;
    if (g_alloc_count == g_fail_target) {
        g_active = 0;
        return 1;
    }
    return 0;
}

void *wc_fault_malloc(size_t n)
{
    if (fault_should_fail()) {
        errno = ENOMEM;
        return NULL;
    }
    return malloc(n);
}

void *wc_fault_realloc(void *p, size_t n)
{
    if (fault_should_fail()) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, n);
}

void wc_fault_free(void *p)
{
    free(p);
}

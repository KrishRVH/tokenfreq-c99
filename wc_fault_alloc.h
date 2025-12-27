#ifndef WC_FAULT_ALLOC_H
#define WC_FAULT_ALLOC_H

#include <stddef.h>

void fault_reset(void);
void fault_arm(int n);

void *wc_fault_malloc(size_t n);
void *wc_fault_realloc(void *p, size_t n);
void wc_fault_free(void *p);

#endif /* WC_FAULT_ALLOC_H */

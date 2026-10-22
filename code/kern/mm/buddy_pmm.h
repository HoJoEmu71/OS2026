#ifndef __KERN_MM_BUDDY_PMM_H__
#define __KERN_MM_BUDDY_PMM_H__

#include <pmm.h>

extern const struct pmm_manager buddy_pmm_manager;

// Run the self-test suite of the buddy system allocator.
void buddy_test(void);

#endif /* !__KERN_MM_BUDDY_PMM_H__ */

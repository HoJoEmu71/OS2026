#include <pmm.h>
#include <list.h>
#include <string.h>
#include <buddy_pmm.h>
#include <stdio.h>

//lab2 function3 2413238

// A block is described by its head page: property stores the block order,
// i.e. the block contains (1 << order) pages. Every block starts at an
// address aligned to its own size, so the buddy of a block can be found by
// flipping the address bit that corresponds to the block size.
#define MAX_ORDER 10

static free_area_t buddy_free_area[MAX_ORDER + 1];
static unsigned int nr_free = 0;
// Page range handed to this allocator; merging must never leave it.
static struct Page *buddy_region_base = NULL;
static size_t buddy_region_pages = 0;

#define free_list(order) (buddy_free_area[order].free_list)

static size_t block_size(size_t order) {
    return (size_t)1 << order;
}

static size_t size_to_order(size_t n) {
    size_t order = 0;
    while (block_size(order) < n) {
        order++;
    }
    return order;
}

static void
buddy_init(void) {
    for (size_t i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_list(i));
        buddy_free_area[i].nr_free = 0;
    }
    nr_free = 0;
}

// Insert the range [base, base + n) as big aligned blocks.
static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }

    size_t remaining = n;
    struct Page *cur = base;
    buddy_region_base = base;
    buddy_region_pages = n;
    while (remaining > 0) {
        uintptr_t pa = page2pa(cur);
        size_t order = MAX_ORDER;
        while (order > 0) {
            size_t size = block_size(order);
            uintptr_t mask = ((uintptr_t)size << PGSHIFT) - 1;
            if (size <= remaining && (pa & mask) == 0) {
                break;
            }
            order--;
        }

        size_t size = block_size(order);
        cur->property = order;
        SetPageProperty(cur);
        list_add(&free_list(order), &(cur->page_link));
        buddy_free_area[order].nr_free += size;
        nr_free += size;

        cur += size;
        remaining -= size;
    }
}

static struct Page *
buddy_alloc_pages(size_t n) {
    if (n == 0 || n > nr_free) {
        return NULL;
    }

    size_t order = size_to_order(n);
    if (order > MAX_ORDER) {
        return NULL;
    }

    size_t cur = order;
    while (cur <= MAX_ORDER && list_empty(&free_list(cur))) {
        cur++;
    }
    if (cur > MAX_ORDER) {
        return NULL;
    }

    list_entry_t *le = list_next(&free_list(cur));
    struct Page *page = le2page(le, page_link);
    list_del(le);
    buddy_free_area[cur].nr_free -= block_size(cur);

    // Split the block until it reaches the requested order.
    while (cur > order) {
        cur--;
        struct Page *right = page + block_size(cur);
        right->property = cur;
        SetPageProperty(right);
        list_add(&free_list(cur), &(right->page_link));
        buddy_free_area[cur].nr_free += block_size(cur);
    }

    for (struct Page *p = page; p != page + block_size(order); p++) {
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    nr_free -= block_size(order);
    return page;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);

    size_t order = size_to_order(n);
    assert(order <= MAX_ORDER);
    size_t size = block_size(order);

    for (struct Page *p = base; p != base + size; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    base->property = order;
    SetPageProperty(base);
    list_add(&free_list(order), &(base->page_link));
    buddy_free_area[order].nr_free += size;
    nr_free += size;

    // Merge with the buddy as long as it is free and of the same order.
    while (order < MAX_ORDER) {
        uintptr_t pa = page2pa(base);
        uintptr_t buddy_pa = pa ^ ((uintptr_t)block_size(order) << PGSHIFT);
        struct Page *buddy = base + ((intptr_t)(buddy_pa - pa) >> PGSHIFT);

        if (buddy < buddy_region_base ||
            buddy + block_size(order) > buddy_region_base + buddy_region_pages) {
            break;
        }
        if (!PageProperty(buddy) || buddy->property != order) {
            break;
        }

        // Both halves are free and of order `order`: move them up one order.
        list_del(&(buddy->page_link));
        buddy_free_area[order].nr_free -= block_size(order);
        ClearPageProperty(buddy);
        buddy->property = 0;

        list_del(&(base->page_link));
        buddy_free_area[order].nr_free -= block_size(order);

        if (buddy < base) {
            base = buddy;
        }
        order++;
        base->property = order;
        SetPageProperty(base);
        list_add(&free_list(order), &(base->page_link));
        buddy_free_area[order].nr_free += block_size(order);
    }
}

static size_t
buddy_nr_free_pages(void) {
    return nr_free;
}

static void
buddy_check(void) {
    size_t total = 0;
    for (size_t order = 0; order <= MAX_ORDER; order++) {
        size_t counted = 0;
        list_entry_t *le = &free_list(order);
        while ((le = list_next(le)) != &free_list(order)) {
            struct Page *p = le2page(le, page_link);
            assert(PageProperty(p));
            assert(p->property == order);
            assert((page2pa(p) & (((uintptr_t)block_size(order) << PGSHIFT) - 1)) == 0);
            counted += block_size(order);
        }
        assert(counted == buddy_free_area[order].nr_free);
        total += counted;
    }
    assert(total == nr_free);
}

// ------------------------------------------------------------------
// Self-test: the suite runs the allocator on a private pool of page
// descriptors so that it never disturbs the physical memory manager
// that is currently registered in pmm_manager.
#define BUDDY_TEST_PAGES 1024

static struct Page buddy_test_pool[BUDDY_TEST_PAGES] __attribute__((aligned(PGSIZE)));
static struct Page *buddy_test_allocated[BUDDY_TEST_PAGES];
static size_t buddy_test_sizes[BUDDY_TEST_PAGES];
static size_t init_block_count[MAX_ORDER + 1];

static size_t test_block_count(size_t order) {
    size_t count = 0;
    list_entry_t *le = &free_list(order);
    while ((le = list_next(le)) != &free_list(order)) {
        count++;
    }
    return count;
}

static size_t test_total_free(void) {
    size_t total = 0;
    for (size_t order = 0; order <= MAX_ORDER; order++) {
        total += test_block_count(order) * block_size(order);
    }
    return total;
}

static void test_snapshot(void) {
    for (size_t order = 0; order <= MAX_ORDER; order++) {
        init_block_count[order] = test_block_count(order);
    }
}

static void test_expect_restored(void) {
    assert(test_total_free() == BUDDY_TEST_PAGES);
    for (size_t order = 0; order <= MAX_ORDER; order++) {
        assert(test_block_count(order) == init_block_count[order]);
        assert(buddy_free_area[order].nr_free ==
               init_block_count[order] * block_size(order));
    }
}

static void test_reset_pool(void) {
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i++) {
        buddy_test_pool[i].flags = 0;
        buddy_test_pool[i].property = 0;
        buddy_test_pool[i].ref = 0;
        SetPageReserved(&buddy_test_pool[i]);
    }
    buddy_init();
    buddy_init_memmap(buddy_test_pool, BUDDY_TEST_PAGES);
}

void buddy_test(void) {
    // Test 1: the whole pool is registered and every block is well formed.
    test_reset_pool();
    assert(nr_free == BUDDY_TEST_PAGES);
    assert(test_total_free() == BUDDY_TEST_PAGES);
    buddy_check();
    cprintf("[Buddy] Test 1 init/registration: PASS\n");

    test_snapshot();

    // Test 2: invalid requests and over-allocation must fail.
    assert(buddy_alloc_pages(0) == NULL);
    assert(buddy_alloc_pages(BUDDY_TEST_PAGES + 1) == NULL);
    cprintf("[Buddy] Test 2 invalid requests: PASS\n");

    // Test 3: allocate one page at a time until the pool is exhausted.
    size_t count = 0;
    struct Page *p;
    while ((p = buddy_alloc_pages(1)) != NULL) {
        buddy_test_allocated[count++] = p;
    }
    assert(count == BUDDY_TEST_PAGES);
    assert(nr_free == 0);
    for (size_t order = 0; order <= MAX_ORDER; order++) {
        assert(list_empty(&free_list(order)));
    }
    for (size_t i = 0; i < count; i++) {
        assert(!PageProperty(buddy_test_allocated[i]));
        assert(buddy_test_allocated[i]->property == 0);
    }
    cprintf("[Buddy] Test 3 exhaust by single pages: PASS\n");

    // Test 4: free everything back and check the pool is fully coalesced.
    for (size_t i = 0; i < count; i++) {
        buddy_free_pages(buddy_test_allocated[i], 1);
    }
    assert(nr_free == BUDDY_TEST_PAGES);
    test_expect_restored();
    buddy_check();
    cprintf("[Buddy] Test 4 free/merge all: PASS\n");

    // Test 5: take the largest block, then give it back.
    size_t order = MAX_ORDER;
    while (order > 0 && list_empty(&free_list(order))) {
        order--;
    }
    assert(!list_empty(&free_list(order)));
    struct Page *head = le2page(list_next(&free_list(order)), page_link);
    p = buddy_alloc_pages(block_size(order));
    assert(p == head);
    assert(nr_free == BUDDY_TEST_PAGES - block_size(order));
    assert(p->property == 0 && !PageProperty(p));
    buddy_free_pages(p, block_size(order));
    assert(nr_free == BUDDY_TEST_PAGES);
    test_expect_restored();
    cprintf("[Buddy] Test 5 largest block alloc/free: PASS\n");

    // Test 6: consume every block below the largest order, so the next
    // request must split a larger block; then free everything back.
    size_t stored = 0;
    for (size_t k = 0; k < order; k++) {
        while (!list_empty(&free_list(k))) {
            struct Page *q = buddy_alloc_pages(block_size(k));
            assert(q != NULL);
            buddy_test_allocated[stored] = q;
            buddy_test_sizes[stored] = block_size(k);
            stored++;
        }
    }
    assert(nr_free == test_block_count(order) * block_size(order));
    p = buddy_alloc_pages(1);
    assert(p != NULL);
    if (PageProperty(p)) {
        // The returned page must not stay on a free list.
        assert(0);
    }
    buddy_free_pages(p, 1);
    for (size_t i = stored; i > 0; i--) {
        buddy_free_pages(buddy_test_allocated[i - 1], buddy_test_sizes[i - 1]);
    }
    assert(nr_free == BUDDY_TEST_PAGES);
    test_expect_restored();
    cprintf("[Buddy] Test 6 split and cascade merge: PASS\n");

    // Test 7: requests that are not powers of two round up to a block.
    p = buddy_alloc_pages(3);
    assert(p != NULL);
    assert(nr_free == BUDDY_TEST_PAGES - 4);
    buddy_free_pages(p, 3);
    assert(nr_free == BUDDY_TEST_PAGES);
    test_expect_restored();

    p = buddy_alloc_pages(5);
    assert(p != NULL);
    assert(nr_free == BUDDY_TEST_PAGES - 8);
    buddy_free_pages(p, 5);
    assert(nr_free == BUDDY_TEST_PAGES);
    test_expect_restored();
    cprintf("[Buddy] Test 7 non power of two requests: PASS\n");

    // Test 8: after taking everything, a small request still fails.
    p = buddy_alloc_pages(BUDDY_TEST_PAGES / 2);
    assert(p != NULL);
    for (;;) {
        struct Page *q = buddy_alloc_pages(1);
        if (q == NULL) {
            break;
        }
    }
    assert(nr_free == 0);
    assert(buddy_alloc_pages(1) == NULL);
    cprintf("[Buddy] Test 8 no free page left: PASS\n");

    test_reset_pool();
    cprintf("[Buddy] All tests passed\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};

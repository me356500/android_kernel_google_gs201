// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/pagevec.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/swap_slots.h>
#include <linux/huge_mm.h>
#include <linux/shmem_fs.h>
#include "internal.h"
#include <linux/mm_types.h>
#include <linux/rmap.h>
#include <linux/hyswp_migrate.h> // add by ycc
/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_page_list.
 */
static const struct address_space_operations swap_aops = {
	.writepage	= swap_writepage,
	.set_page_dirty	= swap_set_page_dirty,
#ifdef CONFIG_MIGRATION
	.migratepage	= migrate_page,
#endif
};

struct address_space *swapper_spaces[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces[MAX_SWAPFILES] __read_mostly;
static bool enable_vma_readahead __read_mostly = true;

static bool swap_ra_break_flag = false; // ycc add
extern bool hyswp_enable;

#define SWAP_RA_WIN_SHIFT	(PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK	((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX	SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK	(~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v)		((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v)		(((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v)		((v) & PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)				\
	(((addr) & PAGE_MASK) |					\
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |	\
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)					\
	(atomic_long_read(&(vma)->swap_readahead_info) ? : 4)

#define INC_CACHE_INFO(x)	data_race(swap_cache_info.x++)
#define ADD_CACHE_INFO(x, nr)	data_race(swap_cache_info.x += (nr))

static struct {
	unsigned long add_total;
	unsigned long del_total;
	unsigned long find_success;
	unsigned long find_total;
} swap_cache_info;

static atomic_t seq_id = ATOMIC_INIT(0);

unsigned long total_swapcache_pages(void)
{
	unsigned int i, j, nr;
	unsigned long ret = 0;
	struct address_space *spaces;
	struct swap_info_struct *si;

	for (i = 0; i < MAX_SWAPFILES; i++) {
		swp_entry_t entry = swp_entry(i, 1);

		/* Avoid get_swap_device() to warn for bad swap entry */
		if (!swp_swap_info(entry))
			continue;
		/* Prevent swapoff to free swapper_spaces */
		si = get_swap_device(entry);
		if (!si)
			continue;
		nr = nr_swapper_spaces[i];
		spaces = swapper_spaces[i];
		for (j = 0; j < nr; j++)
			ret += spaces[j].nrpages;
		put_swap_device(si);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(total_swapcache_pages);

// ycc add
unsigned long check_hybird_swap(void)
{
	unsigned long swp_dev_cnt = 0, i;

	if (!get_hyswp_enable_flag()) // disable hybrid swap
		return 0;

	for (i = 0; i < MAX_SWAPFILES; i++) {
		swp_entry_t entry = swp_entry(i, 1);
		/* Avoid get_swap_device() to warn for bad swap entry */
		if (!swp_swap_info(entry))
			continue;
		swp_dev_cnt++;
	}
	if (swp_dev_cnt >= 2)
		return 1; // multiple swap enable
	return 0;
}
EXPORT_SYMBOL_GPL(check_hybird_swap);
// ycc add
unsigned long get_zram_usage(void)
{
	unsigned int i, ratio;
	struct swap_info_struct *si;
	ratio = 200;
	for (i = 0; i < MAX_SWAPFILES; i++) {
		swp_entry_t entry = swp_entry(i, 1);

		/* Avoid get_swap_device() to warn for bad swap entry */
		if (!swp_swap_info(entry))
			break;
		/* Prevent swapoff to free swapper_spaces */
		si = get_swap_device(entry);
		if (!si)
			break;
		// printk("ycc zram usage %d, %d",si->pages,si->inuse_pages);
		ratio = si->inuse_pages * 100 / si->pages;
		put_swap_device(si);
		break;
	}
	return ratio;
}
EXPORT_SYMBOL_GPL(get_zram_usage);

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);

void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Swap cache stats: add %lu, delete %lu, find %lu/%lu\n",
		swap_cache_info.add_total, swap_cache_info.del_total,
		swap_cache_info.find_success, swap_cache_info.find_total);
	printk("Free swap  = %ldkB\n",
		get_nr_swap_pages() << (PAGE_SHIFT - 10));
	printk("Total swap = %lukB\n", total_swap_pages << (PAGE_SHIFT - 10));
}

void *get_shadow_from_swap_cache(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	struct page *page;

	page = find_get_entry(address_space, idx);
	if (xa_is_value(page))
		return page;
	if (page)
		put_page(page);
	return NULL;
}

/*
 * add_to_swap_cache resembles add_to_page_cache_locked on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
int add_to_swap_cache(struct page *page, swp_entry_t entry,
			gfp_t gfp, void **shadowp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, compound_order(page));
	unsigned long i, nr = thp_nr_pages(page);
	void *old;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageSwapCache(page), page);
	VM_BUG_ON_PAGE(!PageSwapBacked(page), page);

	page_ref_add(page, nr);
	SetPageSwapCache(page);

	do {
		unsigned long nr_shadows = 0;

		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_PAGE(xas.xa_index != idx + i, page);
			old = xas_load(&xas);
			if (xa_is_value(old)) {
				nr_shadows++;
				if (shadowp)
					*shadowp = old;
			}
			set_page_private(page + i, entry.val + i);
			xas_store(&xas, page);
			xas_next(&xas);
		}
		address_space->nrexceptional -= nr_shadows;
		address_space->nrpages += nr;
		__mod_node_page_state(page_pgdat(page), NR_FILE_PAGES, nr);
		ADD_CACHE_INFO(add_total, nr);
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	ClearPageSwapCache(page);
	page_ref_sub(page, nr);
	return xas_error(&xas);
}

/*
 * This must be called only on pages that have
 * been verified to be in the swap cache.
 */
void __delete_from_swap_cache(struct page *page,
			swp_entry_t entry, void *shadow)
{
	struct address_space *address_space = swap_address_space(entry);
	int i, nr = thp_nr_pages(page);
	pgoff_t idx = swp_offset(entry);
	XA_STATE(xas, &address_space->i_pages, idx);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	VM_BUG_ON_PAGE(PageWriteback(page), page);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, shadow);
		VM_BUG_ON_PAGE(entry != page, entry);
		set_page_private(page + i, 0);
		xas_next(&xas);
	}
	ClearPageSwapCache(page);
	if (shadow)
		address_space->nrexceptional += nr;
	address_space->nrpages -= nr;
	__mod_node_page_state(page_pgdat(page), NR_FILE_PAGES, -nr);
	ADD_CACHE_INFO(del_total, nr);
}

// add by tyc
void set_swap_rmap(struct page *page, swp_entry_t entry)
{
	struct swap_info_struct *si = get_swap_device(entry);
	unsigned long offset = swp_offset(entry);

	if (!si) {
		printk(KERN_INFO "[tyc] set_swap_rmap: get_swap_device failed\n");
		return;
	}

	si->rmap[offset].mapping = page->mapping;
	si->rmap[offset].index = page->index;
	si->rmap[offset].seq_id = atomic_read(&seq_id);
	atomic_inc(&seq_id);

	put_swap_device(si);

	return;
}


/**
 * add_to_swap - allocate swap space for a page
 * @page: page we want to move to swap
 *
 * Allocate swap space for the page and add the page to the
 * swap cache.  Caller needs to hold the page lock. 
 */
int add_to_swap(struct page *page)
{
	swp_entry_t entry;
	int err;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageUptodate(page), page);

	entry = get_swap_page(page);
	if (!entry.val)
		return 0;
	// add by tyc
	set_swap_rmap(page, entry);

	/*
	 * XArray node allocations from PF_MEMALLOC contexts could
	 * completely exhaust the page allocator. __GFP_NOMEMALLOC
	 * stops emergency reserves from being allocated.
	 *
	 * TODO: this could cause a theoretical memory reclaim
	 * deadlock in the swap out path.
	 */
	/*
	 * Add it to the swap cache.
	 */
	err = add_to_swap_cache(page, entry,
			__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN, NULL);
	if (err)
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		goto fail;
	/*
	 * Normally the page will be dirtied in unmap because its pte should be
	 * dirty. A special case is MADV_FREE page. The page's pte could have
	 * dirty bit cleared but the page's SwapBacked bit is still set because
	 * clearing the dirty bit and SwapBacked bit has no lock protected. For
	 * such page, unmap will not set dirty bit for it, so page reclaim will
	 * not write the page out. This can cause data corruption when the page
	 * is swap in later. Always setting the dirty bit for the page solves
	 * the problem.
	 */
	set_page_dirty(page);

	return 1;

fail:
	put_swap_page(page, entry);
	return 0;
}

/*
 * This must be called only on pages that have
 * been verified to be in the swap cache and locked.
 * It will never put the page into the free list,
 * the caller has a reference on the page.
 */
void delete_from_swap_cache(struct page *page)
{
	swp_entry_t entry = { .val = page_private(page) };
	struct address_space *address_space = swap_address_space(entry);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache(page, entry, NULL);
	xa_unlock_irq(&address_space->i_pages);

	put_swap_page(page, entry);
	page_ref_sub(page, thp_nr_pages(page));
}

void clear_shadow_from_swap_cache(int type, unsigned long begin,
				unsigned long end)
{
	unsigned long curr = begin;
	void *old;

	for (;;) {
		unsigned long nr_shadows = 0;
		swp_entry_t entry = swp_entry(type, curr);
		struct address_space *address_space = swap_address_space(entry);
		XA_STATE(xas, &address_space->i_pages, curr);

		xa_lock_irq(&address_space->i_pages);
		xas_for_each(&xas, old, end) {
			if (!xa_is_value(old))
				continue;
			xas_store(&xas, NULL);
			nr_shadows++;
		}
		address_space->nrexceptional -= nr_shadows;
		xa_unlock_irq(&address_space->i_pages);

		/* search the next swapcache until we meet end */
		curr >>= SWAP_ADDRESS_SPACE_SHIFT;
		curr++;
		curr <<= SWAP_ADDRESS_SPACE_SHIFT;
		if (curr > end)
			break;
	}
}

/* 
 * If we are the only user, then try to free up the swap cache. 
 * 
 * Its ok to check for PageSwapCache without the page lock
 * here because we are going to recheck again inside
 * try_to_free_swap() _with_ the lock.
 * 					- Marcelo
 */
static inline void free_swap_cache(struct page *page)
{
	if (PageSwapCache(page) && !page_mapped(page) && trylock_page(page)) {
		try_to_free_swap(page);
		unlock_page(page);
	}
}

/* 
 * Perform a free_page(), also freeing any swap cache associated with
 * this page if it is the last user of the page.
 */
void free_page_and_swap_cache(struct page *page)
{
	free_swap_cache(page);
	if (!is_huge_zero_page(page))
		put_page(page);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct page **pages, int nr)
{
	struct page **pagep = pages;
	int i;

	lru_add_drain();
	for (i = 0; i < nr; i++)
		free_swap_cache(pagep[i]);
	release_pages(pagep, nr);
}

static inline bool swap_use_vma_readahead(void)
{
	return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

/*
 * Lookup a swap entry in the swap cache. A found page will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the page
 * lock before returning.
 */
struct page *lookup_swap_cache(swp_entry_t entry, struct vm_area_struct *vma, unsigned long addr,
			       unsigned dev_flag)
{
	struct page *page;
	struct swap_info_struct *si;

	// ycc add
	/*find page uid, pid, pgd*/
	int page_uid, page_pid;
	unsigned long page_pgd;
	unsigned long refault_activate_ratio = 200;

	si = get_swap_device(entry);
	if (!si)
		return NULL;
	page = find_get_page(swap_address_space(entry), swp_offset(entry));
	put_swap_device(si);

	/*find page uid, pid, pgd*/
	page_uid = page_pid = -1;
	page_pgd = 0;
	if (vma && vma->vm_mm) {
		page_pgd = vma->vm_mm->pgd->pgd;
		if (vma->vm_mm->nr_anon_fault)
			refault_activate_ratio =
				vma->vm_mm->nr_anon_refault * 100 / vma->vm_mm->nr_anon_fault;
	}
	if (vma && vma->vm_mm && vma->vm_mm->owner)
		page_pid = vma->vm_mm->owner->pid;
	if (vma && vma->vm_mm && vma->vm_mm->owner && vma->vm_mm->owner->cred)
		page_uid = vma->vm_mm->owner->cred->uid.val;

	// ycc modify
	if (dev_flag) {
		unsigned si_type = swp_type(entry);
		/* swap in log */
		/* section 3.d: evict correlation */
		// swap-in event for evict correlation
		// if (page_uid >= 10220 && page_uid <= 10241 && page_uid==10236) // set the app uid that we want to test (must match to swap-out part)
		// 	if (vma) {
		// 		printk("ycc swp-in_log uid>pid>vma>swp_dev>swp_offset>pfn,%d,%d,%llu,%llu,%llu,%llu",
		// 			page_uid, page_pid, (unsigned long)vma, swp_type(entry),
		// 			swp_offset(entry), PFN_DOWN(addr));
		// 	}

		/* page fault in zram or swap */
		/* need to flash all ROM when adding a parameter to vmstat */
		// if(swp_offset(entry))
		// 	count_vm_event(SWPIN_FLASH); // need to flash ROM to mobile upon adding parameters to /proc/vmstat
		// else
		// 	count_vm_event(SWPIN_ZRAM);

		put_app_swap_in_pattern(page_uid, si_type);
		if (swp_type(entry)) // mark: temp to count page fault in zram,swp
			count_vm_event(THP_SWPOUT_FALLBACK); // page fault on zram
		else
			count_vm_event(THP_SWPOUT); // page fault on flash
		/* page fault in which mm_struct */
		if (show_fault_distribution)
			put_mm_fault_distribution(refault_activate_ratio);
		/* get zram access time */
		if (fault_zram_acc_time && page_uid >= 10200 && page_uid < 10250) {
			unsigned acc_time, lifetime, avg_lifetime;
			bool long_lifetime = false;
			if (si && !swp_type(entry)) {
				unsigned long offset;
				offset = swp_offset(entry);
				if (si->swap_map[offset] && si->swap_map[offset] < SWAP_MAP_MAX) {
					struct timespec64 ts;
					acc_time = call_zram_access_time(offset);
					ts = ktime_to_timespec64(ktime_get_boottime());
					lifetime = (unsigned)ts.tv_sec - acc_time;
					put_refault_duration(page_uid, lifetime);
				}
			} else if (si && swp_type(entry)) {
				/* flash swap access time*/
				unsigned long offset;
				offset = swp_offset(entry);
				if (si->swap_map[offset] && si->swap_map[offset] < SWAP_MAP_MAX) {
					struct timespec64 ts;
					acc_time = get_flash_ac_time(offset);
					ts = ktime_to_timespec64(ktime_get_boottime());
					lifetime = (unsigned)ts.tv_sec - acc_time;
					// put to swap in lifetime
					if (lifetime < (unsigned)10000 && acc_time)
						put_refault_duration(page_uid, lifetime);
					update_flash_ac_time(offset);
				}
			}

			/* count long lifetime swap-in ratio*/
			avg_lifetime = get_avg_refault_duration(page_uid);
			if (avg_lifetime && acc_time) {
				unsigned lifetime_th = avg_lifetime * 2;
				lifetime_th = min((unsigned)1500, lifetime_th);
				lifetime_th = max((unsigned)300, lifetime_th);
				atomic_long_inc(&all_lifetime_swap_in);
				if (lifetime > lifetime_th) {
					atomic_long_inc(&long_lifetime_swap_in);
					long_lifetime = true;
				}

				/* section 3.c: Page Migration */
				/* Dormant pages threshold: large proportion of swap in < 2 * app refault duration (only enable zRAM Page Admission) */
				if (lifetime < avg_lifetime) // < 1 * avg lifetime
					atomic_long_inc(&swap_in_refault_duration[0]);
				else if (lifetime < avg_lifetime * 2) // 1 ~ 2 * avg lifetime
					atomic_long_inc(&swap_in_refault_duration[1]);
				else if (lifetime >= avg_lifetime * 2) // > 2 * avg lifetime
					atomic_long_inc(&swap_in_refault_duration[2]);
				else // xx: reduntant entry
					atomic_long_inc(&swap_in_refault_duration[3]);

				put_app_lifetime_swap_in(page_uid, long_lifetime);
			}
		}
	}

	INC_CACHE_INFO(find_total);
	if (page) {
		bool vma_ra = swap_use_vma_readahead();
		bool readahead;
		bool same_vma_ra;
		INC_CACHE_INFO(find_success);
		/*
		 * At the moment, we don't support PG_readahead for anon THP
		 * so let's bail out rather than confusing the readahead stat.
		 */
		if (unlikely(PageTransCompound(page)))
			return page;

		readahead = TestClearPageReadahead(page);
		if (vma && vma_ra) {
			unsigned long ra_val;
			int win, hits;

			ra_val = GET_SWAP_RA_VAL(vma);
			win = SWAP_RA_WIN(ra_val);
			hits = SWAP_RA_HITS(ra_val);
			if (readahead)
				hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
			atomic_long_set(&vma->swap_readahead_info,
					SWAP_RA_VAL(addr, win, hits));
		}

		if (readahead) {
			count_vm_event(SWAP_RA_HIT);
			if (!vma || !vma_ra)
				atomic_inc(&swapin_readahead_hits);
			// ycc modify
			put_swap_ra_count(page_uid, page_pid, 1, swp_type(entry));
		}
		
		same_vma_ra = TestClearPageSameVMA(page);
		if (same_vma_ra) {
			count_vm_event(FLASH_RA_SAME_VMA_HIT);
			put_swap_ra_count(page_uid, page_pid, 3, swp_type(entry));
		}

		if (TestClearPageExtend(page)) {
			count_vm_event(EXTEND_ACTUAL_RA_HIT);
		}

		if (TestClearPageExtendSameVMA(page)) {
			count_vm_event(EXTEND_ACTUAL_RA_SAME_VMA_HIT);
		}

		if (TestClearPageOldPage(page)) {
			count_vm_event(SWAP_RA_OLD_PAGE_HIT);
		}
	}

	return page;
}

/**
 * find_get_incore_page - Find and get a page from the page or swap caches.
 * @mapping: The address_space to search.
 * @index: The page cache index.
 *
 * This differs from find_get_page() in that it will also look for the
 * page in the swap cache.
 *
 * Return: The found page or %NULL.
 */
struct page *find_get_incore_page(struct address_space *mapping, pgoff_t index)
{
	swp_entry_t swp;
	struct swap_info_struct *si;
	struct page *page = find_get_entry(mapping, index);

	if (!page)
		return page;
	if (!xa_is_value(page))
		return find_subpage(page, index);
	if (!shmem_mapping(mapping))
		return NULL;

	swp = radix_to_swp_entry(page);
	/* Prevent swapoff from happening to us */
	si = get_swap_device(swp);
	if (!si)
		return NULL;
	page = find_get_page(swap_address_space(swp), swp_offset(swp));
	put_swap_device(si);
	return page;
}

struct page *__read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask, struct vm_area_struct *vma,
				     unsigned long addr, bool *new_page_allocated,
				     unsigned skip_cnt, bool readhole)
{
	struct swap_info_struct *si;
	struct page *page;
	void *shadow = NULL;
	// ycc modify
	unsigned long refault;
	refault = -1;

	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after lookup_swap_cache() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si)
			return NULL;
		page = find_get_page(swap_address_space(entry), swp_offset(entry));
		put_swap_device(si);
		if (page)
			return page;

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!readhole && !__swp_swapcount(entry) && swap_slot_cache_enabled)
			return NULL;

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		page = alloc_page_vma(gfp_mask, vma, addr);
		if (!page)
			return NULL;

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (!err)
			break;

		// wyc: read unused slot
		if (readhole && err == -ENOENT)
			break;

		put_page(page);
		if (err != -EEXIST)
			return NULL;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		schedule_timeout_uninterruptible(1);
	}

	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */

	__SetPageLocked(page);
	__SetPageSwapBacked(page);

	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(page, entry, gfp_mask & GFP_RECLAIM_MASK, &shadow)) {
		put_swap_page(page, entry);
		goto fail_unlock;
	}

	if (mem_cgroup_charge(page, NULL, gfp_mask)) {
		delete_from_swap_cache(page);
		goto fail_unlock;
	}

	if (shadow && !readhole)
		refault = workingset_refault(page, shadow, skip_cnt);
	// ycc modify
	if (!skip_cnt && refault != -1) {
		if (vma && vma->vm_mm) {
			if (refault)
				vma->vm_mm->nr_anon_refault++;
			vma->vm_mm->nr_anon_fault++;
		}
	}

	/* Caller will initiate read into locked page */
	lru_cache_add(page);
	*new_page_allocated = true;
	return page;

fail_unlock:
	unlock_page(page);
	put_page(page);
	return NULL;
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 */
struct page *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask, struct vm_area_struct *vma,
				   unsigned long addr, bool do_poll, unsigned skip_cnt)
{
	bool page_was_allocated;
	struct page *retpage =
		__read_swap_cache_async(entry, gfp_mask, vma, addr, &page_was_allocated, skip_cnt, 0);

	if (page_was_allocated)
		swap_readpage(retpage, do_poll);

	return retpage;
}

static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(READ_ONCE(prev_offset), offset, hits,
				  max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

static struct vm_area_struct *get_swap_vma(struct swap_info_struct *si, unsigned long offset)
{
	struct anon_vma *anon_vma;
	struct address_space *file_mapping;
	pgoff_t pgoff_start, pgoff_end;
	struct anon_vma_chain *avc = NULL;
	struct vm_area_struct *vma = NULL;
	unsigned long mapping;
	unsigned char swap_map = si->swap_map[offset];

	if (!swap_map || swap_map == SWAP_MAP_BAD) {
		return vma;
	}

	mapping = (unsigned long)si->rmap[offset].mapping;
	pgoff_start = si->rmap[offset].index;
	pgoff_end = pgoff_start;
	if ((mapping & PAGE_MAPPING_FLAGS) == PAGE_MAPPING_ANON) {
		anon_vma = (void *)(mapping & (~PAGE_MAPPING_FLAGS));
		avc = anon_vma_interval_tree_iter_first(&anon_vma->rb_root,
							pgoff_start, pgoff_end);
		if (avc)
			vma = avc->vma;
	} else {
		file_mapping = (void *)(mapping & (~PAGE_MAPPING_FLAGS));
		vma = vma_interval_tree_iter_first(&file_mapping->i_mmap, pgoff_start, pgoff_end);
	}

	return vma;
}
/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * This has been extended to use the NUMA policies from the mm triggering
 * the readahead.
 *
 * Caller must hold down_read on the vma->vm_mm if vmf->vma is not NULL.
 * This is needed to ensure the VMA will not be freed in our back. In the case
 * of the speculative page fault handler, this cannot happen, even if we don't
 * hold the mmap_sem. Callees are assumed to take care of reading VMA's fields
 * using READ_ONCE() to read consistent values.
 */
struct page *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask, struct vm_fault *vmf,
				    unsigned skip_cnt)
{
	struct page *page;
	unsigned long entry_offset = swp_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = swp_swap_info(entry);
	struct blk_plug plug;
	bool do_poll = true, page_allocated;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address;
	// add by tyc
	struct vm_area_struct *vma_tmp = NULL, *vma_cur = NULL;

	// add by ycc
	unsigned long skipra = 0, readra = 0;
	int page_uid, page_pid;
	unsigned ra_window_size = 0, ra_flag = 0;
	unsigned actual_ra_read = 0, io_count = 0;

	// add by wyc
	unsigned same_vma_cnt = 0, same_vma_window = 0, same_vma_tmp = 0;
	unsigned overflow_cnt = 0;
	unsigned long window_limit = 16 - 1, pre_end_offset = 0;
	bool readhole = 0;
	unsigned long pf_seq_id = 0, ra_seq_id = 0;
	int ra_page_pid = -1;

	page_uid = page_pid = -1;
	if (vma && vma->vm_mm && vma->vm_mm->owner && vma->vm_mm->owner->cred)
		page_uid = vma->vm_mm->owner->cred->uid.val;
	if (vma && vma->vm_mm && vma->vm_mm->owner)
		page_pid = vma->vm_mm->owner->pid;
	
	// skip zram_ra
	if (per_app_vma_prefetch && swp_type(entry) == 0)
		goto skip;

	if (swp_type(entry) == 1) {
		pf_seq_id = si->rmap[entry_offset].seq_id;
		vma_cur = get_swap_vma(si, entry_offset);
	}

	mask = swapin_nr_pages(offset) - 1;
	if (fixed_prefetch == 1) {
		mask = prefetch_window_size - 1;
	}
	// wyc: per_app_prefetch
	if (per_app_ra_prefetch) {
		mask = get_app_ra_window(page_uid, page_pid) - 1;
	}
	// wyc: overflow_prefetch
	if (per_app_vma_prefetch) {
		// mask = get_app_ra_window(page_uid, page_pid) - 1;
		same_vma_window = get_app_same_vma_window(page_uid, page_pid);
	}

	if (overflow_fixed_window) {
		window_limit = overflow_fixed_window - 1;
	}

	skipra = 0;
	// add by ycc
	ra_window_size = mask + 1;
	total_ra_cnt++;
	while (ra_window_size) {
		if (ra_window_size & 1 || ra_flag >= 9)
			break;
		ra_window_size >>= 1;
		ra_flag++;
	}
	if (ra_flag >= 0 && ra_flag < 10)
		total_ra_size_cnt[ra_flag]++;

	if (!mask) {
		actual_ra_page[1]++;
		no_prefetch_cnt++;
	}

	if (!mask)
		goto skip;

	/* Test swap type to make sure the dereference is safe */
	if (likely(si->flags & (SWP_BLKDEV | SWP_FS_OPS))) {
		struct inode *inode = si->swap_file->f_mapping->host;
		if (inode_read_congested(inode))
			goto skip;
	}

	do_poll = false;
	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset)	/* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	/* wyc extend prefetch window */
	pre_end_offset = end_offset;
	/* adjust vma readahead */
	if (per_app_vma_prefetch && (mask != 15 || extend_large_window) && swp_type(entry) == 1) {
		// traverse current window
		for (offset = start_offset; offset <= end_offset && offset < si->max ; offset++) {
			vma_tmp = get_swap_vma(si, offset);
			if (vma_tmp == vma_cur)
				same_vma_cnt++;
		}

		same_vma_tmp = same_vma_cnt;
		// collect same vma page count
		for ( ; offset - start_offset <= window_limit && same_vma_tmp < same_vma_window && offset < si->max ; offset++) {
			vma_tmp = get_swap_vma(si, offset);
			if (vma_tmp == vma_cur)
				same_vma_tmp++;
			if (vma_tmp)
				overflow_cnt++;
		}

		// reach limit or 50% same vma
		if (same_vma_tmp >= same_vma_window || (same_vma_tmp - same_vma_cnt) * 2 >= (overflow_cnt)) {
			__count_vm_events(EXTEND_RA, overflow_cnt);
			__count_vm_events(EXTEND_RA_SAME_VMA, same_vma_tmp - same_vma_cnt);
			end_offset = offset - 1;
		}
	}
	
	if (swp_type(entry) != 0)
		count_vm_event(SWAP_RA_CNT);
	swap_ra_break_flag = true;
	blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset ; offset++) {
		if (skip_zram_ra && swp_type(entry) == 0) {
			// ycc modify : skip zram_ra
			skipra++;
			continue;
		}
		// ra page seq_id
		ra_seq_id = si->rmap[offset].seq_id;
		if (skip_old_page && ra_seq_id + old_page_threshold < pf_seq_id) {
			count_vm_event(SWAP_RA_OLD_PAGE);
			continue;
		}
		// read unused slot
		if (readahead_unused_slot) {
			readhole = __swp_swapcount(swp_entry(swp_type(entry), offset)) == 0;
		}
		virt_prefetch++;
		/* Ok, do the async read-ahead now */
		page = __read_swap_cache_async(swp_entry(swp_type(entry), offset), gfp_mask, vma,
					       addr, &page_allocated, skip_cnt, readhole);
		if (!page) {
			swap_ra_break_flag = true;
			count_vm_event(SWAP_RA_HOLE);
			continue;
		}
		// prefetch page in swap cache
		if (!page_allocated) {
			swap_ra_break_flag = true;
			count_vm_event(SWAP_RA_HAS_CACHE);
		}
		if (page_allocated) {
			if (swp_type(entry) == 1 && (!readahead_unused_slot || !readhole)) {
				vma_tmp = get_swap_vma(si, offset);
				if (offset != entry_offset) {
					count_vm_event(FLASH_RA);
					if (vma_cur && vma_tmp && vma_tmp == vma_cur) {
						count_vm_event(FLASH_RA_SAME_VMA);
						if (offset > pre_end_offset) {
							count_vm_event(EXTEND_ACTUAL_RA_SAME_VMA);
							SetPageExtendSameVMA(page);
						}
						// adjust same vma window
						put_swap_ra_count(page_uid, page_pid, 2, swp_type(entry));
						SetPageSameVMA(page);				
					}
					// drop different vma page
					else if (drop_diff_vma_page && vma_cur && vma_tmp && vma_cur != vma_tmp) {
						count_vm_event(DROP_DIFF_VMA_PAGE);
						SetPageDropPage(page);
					}
					// drop diff pid page
					if (drop_diff_pid_page && page_pid != -1 && vma_tmp && vma_tmp->vm_mm && vma_tmp->vm_mm->owner) {
						ra_page_pid = vma_tmp->vm_mm->owner->pid;
						if (ra_page_pid != page_pid) {
							count_vm_event(DROP_DIFF_PID_PAGE);
							SetPageDropPage(page);
						}
					}
				}
				// get ra page age
				if (get_ra_page_age && pf_seq_id > ra_seq_id) {
					set_page_age((pf_seq_id - ra_seq_id));
				}
				// old page threshold
				if (ra_seq_id + old_page_threshold < pf_seq_id) {
					count_vm_event(SWAP_RA_OLD_PAGE);
					// drop old page
					if (drop_old_page)
						SetPageDropPage(page);
					SetPageOldPage(page);
				}
				if (offset > pre_end_offset) {
					count_vm_event(EXTEND_ACTUAL_RA);
					SetPageExtend(page);
				}
			}
			swap_readpage(page, false);
			if (offset != entry_offset && (!readahead_unused_slot || !readhole)) {
				SetPageReadahead(page);
				count_vm_event(SWAP_RA);
				put_swap_ra_count(page_uid, page_pid, 0, swp_type(entry));
			}
			if (swp_type(entry) != 0) {
				actual_prefetch++;
				actual_ra_read++;
			}
			if (swap_ra_break_flag) {
				swap_ra_break_flag = false;
				swap_ra_io++;
				io_count++;
			}
			// drop unused slot
			if (readahead_unused_slot && readhole) {
				count_vm_event(SWAP_RA_HOLE);
				SetPageDropPage(page);
			}
			// disable bio merge
			if (shatter_prefetch_bio) {
				blk_finish_plug(&plug);
				blk_start_plug(&plug);
			}
		}
		readra++;
		put_page(page);
	}
	blk_finish_plug(&plug);

	/* swap_ra statistic */
#if !defined swap_alloc_swap_ra_enable && !defined swap_alloc_enable
	if (actual_ra_read < max_ra_page) {
		if (actual_ra_read > 2)
			prefetch_cnt++;
		else
			no_prefetch_cnt++;
		if (actual_ra_read)
			actual_ra_page[actual_ra_read]++;
		else
			actual_ra_page[1]++;
	}
#endif          	          	
	if (io_count < max_ra_page)
		ra_io_cnt[io_count]++;
	if (actual_ra_read)
		swap_ra_cnt++;
	lru_add_drain();	/* Push any new pages onto the LRU now */
skip:
	return read_swap_cache_async(entry, gfp_mask, vma, addr, do_poll, skip_cnt);
}

int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space;
	unsigned int i, nr;

	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;

	return 0;
}

void exit_swap_address_space(unsigned int type)
{
	kvfree(swapper_spaces[type]);
	nr_swapper_spaces[type] = 0;
	swapper_spaces[type] = NULL;
}

static inline void swap_ra_clamp_pfn(struct vm_area_struct *vma,
				     unsigned long faddr,
				     unsigned long lpfn,
				     unsigned long rpfn,
				     unsigned long *start,
				     unsigned long *end)
{
	*start = max3(lpfn, PFN_DOWN(READ_ONCE(vma->vm_start)),
		      PFN_DOWN(faddr & PMD_MASK));
	*end = min3(rpfn, PFN_DOWN(READ_ONCE(vma->vm_end)),
		    PFN_DOWN((faddr & PMD_MASK) + PMD_SIZE));
}

static void swap_ra_info(struct vm_fault *vmf,
			struct vma_swap_readahead *ra_info)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	swp_entry_t entry;
	unsigned long faddr, pfn, fpfn;
	unsigned long start, end;
	pte_t *pte, *orig_pte;
	unsigned int max_win, hits, prev_win, win, left;
#ifndef CONFIG_64BIT
	pte_t *tpte;
#endif

	max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster),
			     SWAP_RA_ORDER_CEILING);
	if (max_win == 1) {
		ra_info->win = 1;
		return;
	}

	faddr = vmf->address;
	orig_pte = pte = pte_offset_map(vmf->pmd, faddr);
	entry = pte_to_swp_entry(*pte);
	if ((unlikely(non_swap_entry(entry)))) {
		pte_unmap(orig_pte);
		return;
	}

	fpfn = PFN_DOWN(faddr);
	ra_val = GET_SWAP_RA_VAL(vma);
	pfn = PFN_DOWN(SWAP_RA_ADDR(ra_val));
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	ra_info->win = win = __swapin_nr_pages(pfn, fpfn, hits,
					       max_win, prev_win);
	if (fixed_prefetch == 1) {
		ra_info->win = win = prefetch_window_size;
	}
	atomic_long_set(&vma->swap_readahead_info,
			SWAP_RA_VAL(faddr, win, 0));

	if (win == 1) {
		pte_unmap(orig_pte);
		return;
	}

	/* Copy the PTEs because the page table may be unmapped */
	if (fpfn == pfn + 1)
		swap_ra_clamp_pfn(vma, faddr, fpfn, fpfn + win, &start, &end);
	else if (pfn == fpfn + 1)
		swap_ra_clamp_pfn(vma, faddr, fpfn - win + 1, fpfn + 1,
				  &start, &end);
	else {
		left = (win - 1) / 2;
		swap_ra_clamp_pfn(vma, faddr, fpfn - left, fpfn + win - left,
				  &start, &end);
	}
	ra_info->nr_pte = end - start;
	ra_info->offset = fpfn - start;
	pte -= ra_info->offset;
#ifdef CONFIG_64BIT
	ra_info->ptes = pte;
#else
	tpte = ra_info->ptes;
	for (pfn = start; pfn != end; pfn++)
		*tpte++ = *pte++;
#endif
	pte_unmap(orig_pte);
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @fentry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whoes
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 *
 */
static struct page *swap_vma_readahead(swp_entry_t fentry, gfp_t gfp_mask, struct vm_fault *vmf,
				       unsigned skip_cnt)
{
	struct blk_plug plug;
	struct vm_area_struct *vma = vmf->vma;
	struct page *page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i;
	bool page_allocated;
	struct vma_swap_readahead ra_info = {0,};
	int page_uid, page_pid;
	unsigned long skipra = 0, readra = 0; // ycc modify
	// add by wyc: io count
	unsigned long offset = 0, pre_offset = 0;
	skipra = 0;
	page_uid = page_pid = -1;
	if (vma && vma->vm_mm && vma->vm_mm->owner && vma->vm_mm->owner->cred)
		page_uid = vma->vm_mm->owner->cred->uid.val;
	if (vma && vma->vm_mm && vma->vm_mm->owner)
		page_pid = vma->vm_mm->owner->pid;

	swap_ra_info(vmf, &ra_info);
	if (ra_info.win == 1)
		goto skip;

	blk_start_plug(&plug);
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte;
	     i++, pte++) {
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		if (skip_zram_ra && swp_type(entry) == 0)
			continue;

		page = __read_swap_cache_async(entry, gfp_mask, vma, vmf->address, &page_allocated,
					       skip_cnt, 0);
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage(page, false);
			if (i != ra_info.offset) {
				SetPageReadahead(page);
				count_vm_event(SWAP_RA);
				put_swap_ra_count(page_uid, page_pid, 0, swp_type(entry));
			}
			// collect entry offset & io count
			offset = swp_offset(entry);
			if (swp_type(entry) != 0) {
				actual_prefetch++;
			}
			if (!(offset == pre_offset + 1 || offset == pre_offset - 1)) {
				swap_ra_io++;
			}
			pre_offset = offset;
		}
		// ycc modify
		readra++;
		put_page(page);
	}
	blk_finish_plug(&plug);
	lru_add_drain();
skip:
	return read_swap_cache_async(fentry, gfp_mask, vma, vmf->address, ra_info.win == 1,
				     skip_cnt);
}

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
struct page *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask, struct vm_fault *vmf,
			      unsigned skip_cnt)
{
	return swap_use_vma_readahead() ? swap_vma_readahead(entry, gfp_mask, vmf, skip_cnt) :
					  swap_cluster_readahead(entry, gfp_mask, vmf, skip_cnt);
}

#ifdef CONFIG_SYSFS
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", enable_vma_readahead ? "true" : "false");
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	if (!strncmp(buf, "true", 4) || !strncmp(buf, "1", 1))
		enable_vma_readahead = true;
	else if (!strncmp(buf, "false", 5) || !strncmp(buf, "0", 1))
		enable_vma_readahead = false;
	else
		return -EINVAL;

	return count;
}
static struct kobj_attribute vma_ra_enabled_attr =
	__ATTR(vma_ra_enabled, 0644, vma_ra_enabled_show,
	       vma_ra_enabled_store);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	NULL,
};

static struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init_sysfs(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init_sysfs);
#endif

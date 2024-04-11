// SPDX-License-Identifier: GPL-2.0
/*
 * Manage cache of swap slots to be used for and returned from
 * swap.
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * Author: Tim Chen <tim.c.chen@linux.intel.com>
 *
 * We allocate the swap slots from the global pool and put
 * it into local per cpu caches.  This has the advantage
 * of no needing to acquire the swap_info lock every time
 * we need a new slot.
 *
 * There is also opportunity to simply return the slot
 * to local caches without needing to acquire swap_info
 * lock.  We do not reuse the returned slots directly but
 * move them back to the global pool in a batch.  This
 * allows the slots to coaellesce and reduce fragmentation.
 *
 * The swap entry allocated is marked with SWAP_HAS_CACHE
 * flag in map_count that prevents it from being allocated
 * again from the global pool.
 *
 * The swap slots cache is protected by a mutex instead of
 * a spin lock as when we search for slots with scan_swap_map,
 * we can possibly sleep.
 */

#include <linux/swap_slots.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/rmap.h> // ycc add
#include <linux/hyswp_migrate.h> // ycc add

extern volatile int zram_usage;

static DEFINE_PER_CPU(struct swap_slots_cache, swp_slots);
static bool	swap_slot_cache_active;
bool	swap_slot_cache_enabled;
static bool	swap_slot_cache_initialized;
static DEFINE_MUTEX(swap_slots_cache_mutex);
/* Serialize swap slots cache enable/disable operations */
static DEFINE_MUTEX(swap_slots_cache_enable_mutex);

static void __drain_swap_slots_cache(unsigned int type);
static void deactivate_swap_slots_cache(void);
static void reactivate_swap_slots_cache(void);

#define use_swap_slot_cache (swap_slot_cache_active && swap_slot_cache_enabled)
#define SLOTS_CACHE 0x1
#define SLOTS_CACHE_RET 0x2

static void deactivate_swap_slots_cache(void)
{
	mutex_lock(&swap_slots_cache_mutex);
	swap_slot_cache_active = false;
	__drain_swap_slots_cache(SLOTS_CACHE|SLOTS_CACHE_RET);
	mutex_unlock(&swap_slots_cache_mutex);
}

static void reactivate_swap_slots_cache(void)
{
	mutex_lock(&swap_slots_cache_mutex);
	swap_slot_cache_active = true;
	mutex_unlock(&swap_slots_cache_mutex);
}

/* Must not be called with cpu hot plug lock */
void disable_swap_slots_cache_lock(void)
{
	mutex_lock(&swap_slots_cache_enable_mutex);
	swap_slot_cache_enabled = false;
	if (swap_slot_cache_initialized) {
		/* serialize with cpu hotplug operations */
		get_online_cpus();
		__drain_swap_slots_cache(SLOTS_CACHE|SLOTS_CACHE_RET);
		put_online_cpus();
	}
}

static void __reenable_swap_slots_cache(void)
{
	swap_slot_cache_enabled = has_usable_swap();
}

void reenable_swap_slots_cache_unlock(void)
{
	__reenable_swap_slots_cache();
	mutex_unlock(&swap_slots_cache_enable_mutex);
}

static bool check_cache_active(void)
{
	long pages;

	if (!swap_slot_cache_enabled)
		return false;

	pages = get_nr_swap_pages();
	if (!swap_slot_cache_active) {
		if (pages > num_online_cpus() *
		    THRESHOLD_ACTIVATE_SWAP_SLOTS_CACHE)
			reactivate_swap_slots_cache();
		goto out;
	}

	/* if global pool of slot caches too low, deactivate cache */
	if (pages < num_online_cpus() * THRESHOLD_DEACTIVATE_SWAP_SLOTS_CACHE)
		deactivate_swap_slots_cache();
out:
	return swap_slot_cache_active;
}

static int alloc_swap_slot_cache(unsigned int cpu)
{
	struct swap_slots_cache *cache;
	swp_entry_t *slots, *slots_ret;

	/*
	 * Do allocation outside swap_slots_cache_mutex
	 * as kvzalloc could trigger reclaim and get_swap_page,
	 * which can lock swap_slots_cache_mutex.
	 */
	slots = kvcalloc(SWAP_SLOTS_CACHE_SIZE, sizeof(swp_entry_t),
			 GFP_KERNEL);
	if (!slots)
		return -ENOMEM;

	slots_ret = kvcalloc(SWAP_SLOTS_CACHE_SIZE, sizeof(swp_entry_t),
			     GFP_KERNEL);
	if (!slots_ret) {
		kvfree(slots);
		return -ENOMEM;
	}

	mutex_lock(&swap_slots_cache_mutex);
	cache = &per_cpu(swp_slots, cpu);
	if (cache->slots || cache->slots_ret) {
		/* cache already allocated */
		mutex_unlock(&swap_slots_cache_mutex);

		kvfree(slots);
		kvfree(slots_ret);

		return 0;
	}

	if (!cache->lock_initialized) {
		mutex_init(&cache->alloc_lock);
		spin_lock_init(&cache->free_lock);
		cache->lock_initialized = true;
	}
	cache->nr = 0;
	cache->cur = 0;
	cache->n_ret = 0;
	/*
	 * We initialized alloc_lock and free_lock earlier.  We use
	 * !cache->slots or !cache->slots_ret to know if it is safe to acquire
	 * the corresponding lock and use the cache.  Memory barrier below
	 * ensures the assumption.
	 */
	mb();
	cache->slots = slots;
	cache->slots_ret = slots_ret;
	mutex_unlock(&swap_slots_cache_mutex);
	return 0;
}

static void drain_slots_cache_cpu(unsigned int cpu, unsigned int type,
				  bool free_slots)
{
	struct swap_slots_cache *cache;
	swp_entry_t *slots = NULL;

	cache = &per_cpu(swp_slots, cpu);
	if ((type & SLOTS_CACHE) && cache->slots) {
		mutex_lock(&cache->alloc_lock);
		swapcache_free_entries(cache->slots + cache->cur, cache->nr);
		cache->cur = 0;
		cache->nr = 0;
		if (free_slots && cache->slots) {
			kvfree(cache->slots);
			cache->slots = NULL;
		}
		mutex_unlock(&cache->alloc_lock);
	}
	if ((type & SLOTS_CACHE_RET) && cache->slots_ret) {
		spin_lock_irq(&cache->free_lock);
		swapcache_free_entries(cache->slots_ret, cache->n_ret);
		cache->n_ret = 0;
		if (free_slots && cache->slots_ret) {
			slots = cache->slots_ret;
			cache->slots_ret = NULL;
		}
		spin_unlock_irq(&cache->free_lock);
		if (slots)
			kvfree(slots);
	}
}

static void __drain_swap_slots_cache(unsigned int type)
{
	unsigned int cpu;

	/*
	 * This function is called during
	 *	1) swapoff, when we have to make sure no
	 *	   left over slots are in cache when we remove
	 *	   a swap device;
	 *      2) disabling of swap slot cache, when we run low
	 *	   on swap slots when allocating memory and need
	 *	   to return swap slots to global pool.
	 *
	 * We cannot acquire cpu hot plug lock here as
	 * this function can be invoked in the cpu
	 * hot plug path:
	 * cpu_up -> lock cpu_hotplug -> cpu hotplug state callback
	 *   -> memory allocation -> direct reclaim -> get_swap_page
	 *   -> drain_swap_slots_cache
	 *
	 * Hence the loop over current online cpu below could miss cpu that
	 * is being brought online but not yet marked as online.
	 * That is okay as we do not schedule and run anything on a
	 * cpu before it has been marked online. Hence, we will not
	 * fill any swap slots in slots cache of such cpu.
	 * There are no slots on such cpu that need to be drained.
	 */
	for_each_online_cpu(cpu)
		drain_slots_cache_cpu(cpu, type, false);
}

static int free_slot_cache(unsigned int cpu)
{
	mutex_lock(&swap_slots_cache_mutex);
	drain_slots_cache_cpu(cpu, SLOTS_CACHE | SLOTS_CACHE_RET, true);
	mutex_unlock(&swap_slots_cache_mutex);
	return 0;
}

void enable_swap_slots_cache(void)
{
	mutex_lock(&swap_slots_cache_enable_mutex);
	if (!swap_slot_cache_initialized) {
		int ret;

		ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "swap_slots_cache",
					alloc_swap_slot_cache, free_slot_cache);
		if (WARN_ONCE(ret < 0, "Cache allocation failed (%s), operating "
				       "without swap slots cache.\n", __func__))
			goto out_unlock;

		swap_slot_cache_initialized = true;
	}

	__reenable_swap_slots_cache();
out_unlock:
	mutex_unlock(&swap_slots_cache_enable_mutex);
}

/* called with swap slot cache's alloc lock held */
static int refill_swap_slots_cache(struct swap_slots_cache *cache)
{
	if (!use_swap_slot_cache || cache->nr)
		return 0;

	cache->cur = 0;
	if (swap_slot_cache_active)
		cache->nr = get_swap_pages(SWAP_SLOTS_CACHE_SIZE,
					   cache->slots, 1, 1, NULL);

	return cache->nr;
}

int free_swap_slot(swp_entry_t entry)
{
	struct swap_slots_cache *cache;

	cache = raw_cpu_ptr(&swp_slots);
	if (likely(use_swap_slot_cache && cache->slots_ret)) {
		spin_lock_irq(&cache->free_lock);
		/* Swap slots cache may be deactivated before acquiring lock */
		if (!use_swap_slot_cache || !cache->slots_ret) {
			spin_unlock_irq(&cache->free_lock);
			goto direct_free;
		}
		if (cache->n_ret >= SWAP_SLOTS_CACHE_SIZE) {
			/*
			 * Return slots to global pool.
			 * The current swap_map value is SWAP_HAS_CACHE.
			 * Set it to 0 to indicate it is available for
			 * allocation in global pool
			 */
			swapcache_free_entries(cache->slots_ret, cache->n_ret);
			cache->n_ret = 0;
		}
		cache->slots_ret[cache->n_ret++] = entry;
		spin_unlock_irq(&cache->free_lock);
	} else {
direct_free:
		swapcache_free_entries(&entry, 1);
	}

	return 0;
}

swp_entry_t get_swap_page(struct page *page)
{
	swp_entry_t entry;
	struct swap_slots_cache *cache;
	// ycc add
	int hySwpCheck;
	bool disable_swap_slot_cache = false;
	/*select uid to swap*/
	struct anon_vma *anon_vma; 
	struct anon_vma_chain *avc;
	struct vm_area_struct *vma = NULL;
	pgoff_t pgoff_start;
	unsigned long page_uid;
	unsigned int refault_ratio; // ,zram_usage;
	// int zram_fullness, refault_th;
	unsigned long anon_size, swap_size;
	unsigned long large_vma_size = (1UL) << PMD_SHIFT;
	page_uid = 0;
	refault_ratio = 99;
	hySwpCheck = 0;
	anon_size = swap_size = 0;

	entry.val = 0;

	if (PageTransHuge(page)) {
		if (IS_ENABLED(CONFIG_THP_SWAP))
			get_swap_pages(1, &entry, HPAGE_PMD_NR, 1, vma); // ycc modify
		goto out;
	}

	// ycc find vma to swap allocation
	anon_vma = page_anon_vma(page);
	if (anon_vma) {
		pgoff_start = page_to_pgoff(page);
		anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff_start, pgoff_start)
		{
			vma = avc->vma;
			if (vma)
				break;
		}
		if (vma)
			if (vma->vm_start + large_vma_size > vma->vm_end)
				vma = NULL;
	}

	// ycc page to uid or pid
	// page_get_anon_vma(page);
	hySwpCheck = check_hybird_swap();
	// zram_usage=get_zram_usage();
	// hySwpCheck=0; //disable hybrid swap
	if (hySwpCheck) {
		// // adaptive hyswp policy
		// zram_fullness = zram_usage;
		// // zram_fullness=get_zram_usage();
		// if (zram_fullness >= 60 && zram_fullness <= 100)
		// 	refault_th = 15 + (10 * (zram_fullness - 60) / (100 - 60));
		// else {
		// 	refault_th = 15;
		// 	// printk("ycc zram fail %d",zram_usage);
		// }

		// printk("ycc zram usage, %d",zram_usage);

		/* zram idle page migrate -> need to downgrade */
		if(PageReswapin(page)){
			// printk("ycc zram idle page swap-out");
			ClearPageReswapin(page);
			get_swap_pages(1, &entry, 1, 0, vma);
			count_vm_event(THP_ZERO_PAGE_ALLOC);
			goto out;
		}

		/*select mm_struct to swap*/
		anon_vma = page_anon_vma(page);
		if (anon_vma) {
			pgoff_start = page_to_pgoff(page);
			anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff_start,
						       pgoff_start)
			{
				vma = avc->vma;
				if (vma)
					break;
			}
			// // print log
			if (vma && vma->vm_mm && vma->vm_mm->owner && vma->vm_mm->owner->cred) {
				page_uid = vma->vm_mm->owner->cred->uid.val;
				// printk("ycc mm_struct_refault %u %u %u %u", page_uid, vma->vm_mm->nr_anon_refault, vma->vm_mm->nr_anon_fault, vma->vm_mm->nr_anon_refault*100/vma->vm_mm->nr_anon_fault);
			}
			// else if(vma&&vma->vm_mm){
			// 	printk("ycc mm_struct_refault -1 %u %u %u", vma->vm_mm->nr_anon_refault, vma->vm_mm->nr_anon_fault, vma->vm_mm->nr_anon_refault*100/vma->vm_mm->nr_anon_fault);
			// }
			refault_ratio =
				vma->vm_mm->nr_anon_refault * 100 / vma->vm_mm->nr_anon_fault;

			anon_size = get_mm_counter(vma->vm_mm, MM_ANONPAGES); // unit : page
			swap_size = get_mm_counter(vma->vm_mm, MM_SWAPENTS);

			// if(page_uid>=10200&&page_uid<=10245){ // print anon size log
			// 	printk("ycc swp_out %u %lu %u %u %u",page_uid, anon_size+swap_size, refault_ratio, anon_size, swap_size);
			// }
			if (refault_ratio <= anon_refault_active_th &&
			    anon_size + swap_size > 5000) {
				// printk("ycc downgrade_f %u th(%u)", page_uid, refault_ratio);
				get_swap_pages(1, &entry, 1, 0, vma);
				count_vm_event(THP_ZERO_PAGE_ALLOC);
				goto out;
			}
			// if(anon_size+swap_size>100000){
			// 	printk("ycc downgrade_s %u anon(%u)",page_uid,anon_size+swap_size);
			// 	get_swap_pages(1, &entry, 1,0);
			// 	count_vm_event(THP_ZERO_PAGE_ALLOC_FAILED);
			// 	goto out;
			// }
			// if((refault_ratio<=refault_th&&anon_size+swap_size>70000)){
			// 	printk("ycc downgrade %u th(%u), ratio(%d)",page_uid,refault_th,refault_ratio);
			// 	get_swap_pages(1, &entry, 1,0);
			// 	count_vm_event(THP_DEFERRED_SPLIT_PAGE);
			// 	goto out;
			// }
		}
	}

	/*
	 * Preemption is allowed here, because we may sleep
	 * in refill_swap_slots_cache().  But it is safe, because
	 * accesses to the per-CPU data structure are protected by the
	 * mutex cache->alloc_lock.
	 *
	 * The alloc path here does not touch cache->slots_ret
	 * so cache->free_lock is not taken.
	 */
	cache = raw_cpu_ptr(&swp_slots);

#ifdef swap_alloc_enable // ycc modify: disable swap slot cache
	disable_swap_slot_cache = true;
#endif

	if(!disable_swap_slot_cache){
	if (likely(check_cache_active() && cache->slots)) {
		mutex_lock(&cache->alloc_lock);
		if (cache->slots) {
repeat:
			if (cache->nr) {
				entry = cache->slots[cache->cur];
				cache->slots[cache->cur++].val = 0;
				cache->nr--;
			} else if (refill_swap_slots_cache(cache)) {
				goto repeat;
			}
		}
		mutex_unlock(&cache->alloc_lock);
		if (entry.val)
			goto out;
	}
	}


	get_swap_pages(1, &entry, 1, 1, vma);
out:
	if (mem_cgroup_try_charge_swap(page, entry)) {
		put_swap_page(page, entry);
		entry.val = 0;
	}
	return entry;
}

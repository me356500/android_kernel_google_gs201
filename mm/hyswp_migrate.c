#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/gfp.h>
#include <linux/types.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/swapfile.h>
#include <linux/hyswp_migrate.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/string.h>

int hyswp_scan_sec = 30;
static unsigned short scan_round;
gfp_t my_gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_CMA;
volatile int zram_usage = 200;
static int local_zram_usage;
#define zran_usage_th 5
int page_demote_cnt = 0, page_promote_cnt = 0;
int PMD_cnt = 0, vma_cnt = 0, large_vma_cnt = 0;
enum si_dev { zram_dev = 0, flash_dev };

/* statistic */
// static DEFINE_MUTEX(distribution_lock);
static DEFINE_SPINLOCK(distribution_lock);
#define distribution 20
int mm_distribution[distribution];
/* statistic zram, flash page*/
bool swap_page_distribution = false;
unsigned refault_value_global;
unsigned zram_distribution[distribution];
unsigned flash_distribution[distribution];

/* anon page fault latency */
unsigned long anon_fault = 0, anon_fault_lat = 0;

/* swap on zram or flash */
unsigned long zram_in = 0, flash_in = 0;

/* swap slot hole effect */
unsigned long virt_prefetch = 0, actual_prefetch = 0;

/* app swap cache hit and ra */
#define total_app_slot 50
unsigned app_total_ra[total_app_slot];
unsigned app_swap_cache_hit[total_app_slot];


/* app swap cache hit and ra */
void put_swap_ra_count(int app_uid, int ra_hit_flag)
{
	spin_lock(&distribution_lock);
	if (app_uid >= 10200 && app_uid < 10300) {
		int slot = app_uid % total_app_slot;
		if (ra_hit_flag)
			app_swap_cache_hit[slot]++;
		else
			app_total_ra[slot]++;
	}
	spin_unlock(&distribution_lock);
}

// statistic init
static void init_statistic()
{
	int i;
	// mutex_lock(&distribution_lock);
	spin_lock(&distribution_lock);
	for (i = 0; i < distribution; i++) {
		mm_distribution[i] = 0;
		zram_distribution[i] = 0;
		flash_distribution[i] = 0;
	}
	// mutex_unlock(&distribution_lock);
	for (i = 0; i < total_app_slot; i++) {
		app_total_ra[i] = 0;
		app_swap_cache_hit[i] = 0;
	}

	spin_unlock(&distribution_lock);
}

static void reset_swap_page_distribution()
{
	int i;
	spin_lock(&distribution_lock);
	for (i = 0; i < distribution; i++) {
		zram_distribution[i] = 0;
		flash_distribution[i] = 0;
	}
	spin_unlock(&distribution_lock);
}


/* statistic anon fault in each mm */
void put_mm_fault_distribution(unsigned value)
{
	unsigned bucket = value / 5;
	spin_lock(&distribution_lock);
	bucket = bucket >= distribution ? distribution - 1 : bucket;
	mm_distribution[bucket]++;
	spin_unlock(&distribution_lock);
}

static void show_mm_distribution()
{
	char msg[1024] = { 0 };
	int i;
	spin_lock(&distribution_lock);
	sprintf(msg, "mm_value");
	for (i = 0; i < distribution; i++)
		sprintf(msg, "%s, %d", msg, mm_distribution[i]);
	spin_unlock(&distribution_lock);
	printk("ycc_each_mm fault,scan_round,%d, %s", scan_round, msg);
}

/* statistic zram, flash page in each mm */
static void put_swap_page_distribution(unsigned si_type)
{
	unsigned bucket = refault_value_global / 5;
	bucket = bucket >= distribution ? distribution - 1 : bucket;
	if (si_type == zram_dev)
		zram_distribution[bucket]++;
	else
		flash_distribution[bucket]++;
}

static void show_swap_page_distribution()
{
	char msg[1024] = { 0 };
	int i;
	sprintf(msg, "mm_value");
	for (i = 0; i < distribution; i++)
		sprintf(msg, "%s, %u", msg, zram_distribution[i]);
	printk("ycc hyswp_psuedo zram,scan_round,%d, %s", scan_round, msg);

	sprintf(msg, "mm_value");
	for (i = 0; i < distribution; i++)
		sprintf(msg, "%s, %u", msg, flash_distribution[i]);
	printk("ycc hyswp_psuedo flash,scan_round,%d, %s", scan_round, msg);
}

// mm_struct migrate
static void scan_pte(struct vm_area_struct *vma, pmd_t *pmd, unsigned long addr, unsigned long end,
		     unsigned si_type)
{
	struct page *page;
	swp_entry_t entry;
	pte_t *pte;
	struct swap_info_struct *si;
	unsigned long offset;
	volatile unsigned char *swap_map;
	int ret = 0;
	unsigned page_ref_cnt = 0;

	if (si_type <= 1)
		si = swap_info[si_type];
	pte = pte_offset_map(pmd, addr);
	do {
		if (!is_swap_pte(*pte))
			continue;

		entry = pte_to_swp_entry(*pte);
		put_swap_page_distribution(swp_type(entry));
		if (swp_type(entry) != si_type)
			continue;

		offset = swp_offset(entry);
		page_ref_cnt = __swp_swapcount(swp_entry(swp_type(entry), offset));
		if (page_ref_cnt != 1 && si_type == zram_dev)
			continue;

		pte_unmap(pte);
		swap_map = &si->swap_map[offset];
		page = lookup_swap_cache(entry, vma, addr, 0);
		if (!page) {
			struct vm_fault vmf = {
				.vma = vma,
				.address = addr,
				.pmd = pmd,
			};

			page = swapin_readahead(entry, GFP_HIGHUSER_MOVABLE, &vmf, 1);
		}
		if (!page) {
			if (*swap_map == 0 || *swap_map == SWAP_MAP_BAD)
				goto try_next;
			// return -ENOMEM;
			goto try_next;
		}

		lock_page(page);
		wait_on_page_writeback(page);
		ret = unuse_pte(vma, pmd, addr, entry, page);
		if (ret < 0) {
			unlock_page(page);
			put_page(page);
			// goto out;
			goto try_next;
		}

		try_to_free_swap(page);
		unlock_page(page);
		put_page(page);
		// if (si_type == zram_dev) {
		// 	page_demote_cnt++;
		// 	count_vm_event(PG_DEMOTE);
		// } else {
		// 	page_promote_cnt++;
		// 	count_vm_event(PG_PROMOTE);
		// }
		if (si_type == zram_dev) { // mark: tmp to count demote, promote
			page_demote_cnt++;
			count_vm_event(BALLOON_DEFLATE);
		} else {
			page_promote_cnt++;
			count_vm_event(BALLOON_INFLATE);
		}
		count_vm_event(SWAP_RA);

	try_next:
		pte = pte_offset_map(pmd, addr);
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static inline void scan_pmd(struct vm_area_struct *vma, pud_t *pud, unsigned long addr,
			    unsigned long end, unsigned si_type)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_offset(pud, addr);
	do {
		cond_resched();
		next = pmd_addr_end(addr, end);
		if (pmd_none_or_trans_huge_or_clear_bad(pmd))
			continue;

		scan_pte(vma, pmd, addr, next, si_type);
		PMD_cnt++;
	} while (pmd++, addr = next, addr != end);
}

static inline void scan_pud(struct vm_area_struct *vma, p4d_t *p4d, unsigned long addr,
			    unsigned long end, unsigned si_type)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;

		scan_pmd(vma, pud, addr, next, si_type);
	} while (pud++, addr = next, addr != end);
}

static inline void scan_p4d(struct vm_area_struct *vma, pgd_t *pgd, unsigned long addr,
			    unsigned long end, unsigned si_type)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d))
			continue;

		scan_pud(vma, p4d, addr, next, si_type);
	} while (p4d++, addr = next, addr != end);
}

static void scan_pgd(struct vm_area_struct *vma, unsigned si_type)
{
	pgd_t *pgd;
	unsigned long addr, end, next;
	addr = vma->vm_start;
	end = vma->vm_end;

	pgd = pgd_offset(vma->vm_mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;

		scan_p4d(vma, pgd, addr, next, si_type);
	} while (pgd++, addr = next, addr != end);
}

static void scan_vma(struct mm_struct *mm, unsigned si_type)
{
	struct vm_area_struct *vma;

	mmap_read_lock(mm);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma->anon_vma) {
			scan_pgd(vma, si_type);
		}
		cond_resched();
	}
	mmap_read_unlock(mm);
}

static void pseudo_scan_vma(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	unsigned long large_vma_size = (1UL) << PMD_SHIFT;

	mmap_read_lock(mm);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma->anon_vma) {
			vma_cnt++;
			if (vma->vm_start + large_vma_size < vma->vm_end)
				large_vma_cnt++;
		}
	}
	mmap_read_unlock(mm);
}

static void start_zram_migrate()
{
	struct mm_struct *mm;
	struct mm_struct *prev_mm;
	struct list_head *p;
	unsigned long refault_ratio = 200;
	unsigned mm_cnt = 0, mm_demote_cnt = 0;
	// unsigned mm_promote_cnt = 0;
	unsigned mm_demote_1 = 0, mm_demote_2 = 0, mm_demote_3 = 0;
	unsigned long anon_size = 0, swap_size = 0;
	unsigned long avg_anon_fault = 0;

	prev_mm = &init_mm;
	mmget(prev_mm);

	spin_lock(&mmlist_lock);
	p = &init_mm.mmlist;
	page_demote_cnt = 0;
	page_promote_cnt = 0;
	PMD_cnt = vma_cnt = large_vma_cnt = 0;

	while ((p = p->next) != &init_mm.mmlist) {
		mm = list_entry(p, struct mm_struct, mmlist);
		if (!mmget_not_zero(mm))
			continue;
		spin_unlock(&mmlist_lock);
		mmput(prev_mm);
		prev_mm = mm;
		anon_size = get_mm_counter(mm, MM_ANONPAGES); // unit : page
		swap_size = get_mm_counter(mm, MM_SWAPENTS);

		if (mm->nr_anon_fault)
			refault_ratio = mm->nr_anon_refault * 100 / mm->nr_anon_fault;
		mm_cnt++;
		avg_anon_fault += mm->nr_anon_fault;
		if (refault_ratio <= anon_refault_active_th && anon_size + swap_size > 5000) {
			mm_demote_cnt++;
			mm_demote_1++;
			scan_vma(mm, zram_dev);
		} else if (anon_size + swap_size > 100000) {
			mm_demote_cnt++;
			mm_demote_2++;
			// scan_vma(mm, zram_dev);
		} else if ((refault_ratio <= 15 && anon_size + swap_size > 70000)) {
			mm_demote_cnt++;
			mm_demote_3++;
			// scan_vma(mm, zram_dev);
		}
		pseudo_scan_vma(mm);
		cond_resched();
		spin_lock(&mmlist_lock);
	}
	spin_unlock(&mmlist_lock);
	mmput(prev_mm);

	avg_anon_fault /= mm_cnt;
	printk("ycc hyswp_migrate: scan_mm(%d), demote_mm(%d), demote_page(%d), demote_1(%d), demote_2(%d), avg_anon_fault(%llu)",
	       mm_cnt, mm_demote_cnt, page_demote_cnt, mm_demote_1, mm_demote_2, avg_anon_fault);
	printk("ycc hyswp scan_pmd(%d), scan_vma(%d), large_vma(%d)", PMD_cnt, vma_cnt,
	       large_vma_cnt);
	if (show_fault_distribution && scan_round >= 3)
		show_mm_distribution();
}

static void scan_mm_swap_page_count() // count zram, flash page in each mm
{
	/* statistic zram, flash page in each mm */
	struct mm_struct *mm;
	struct mm_struct *prev_mm;
	struct list_head *p;
	unsigned long refault_ratio = 200;
	unsigned mm_cnt = 0;
	// unsigned mm_promote_cnt = 0;
	unsigned long avg_anon_fault = 0;

	prev_mm = &init_mm;
	mmget(prev_mm);

	spin_lock(&mmlist_lock);
	p = &init_mm.mmlist;
	page_demote_cnt = 0;
	page_promote_cnt = 0;
	PMD_cnt = vma_cnt = large_vma_cnt = 0;

	reset_swap_page_distribution();

	while ((p = p->next) != &init_mm.mmlist) {
		mm = list_entry(p, struct mm_struct, mmlist);
		if (!mmget_not_zero(mm))
			continue;
		spin_unlock(&mmlist_lock);
		mmput(prev_mm);
		prev_mm = mm;

		if (mm->nr_anon_fault)
			refault_ratio = mm->nr_anon_refault * 100 / mm->nr_anon_fault;
		refault_value_global = refault_ratio;

		mm_cnt++;
		avg_anon_fault += mm->nr_anon_fault;

		scan_vma(mm, 5); // si_type is invalid
		cond_resched();
		spin_lock(&mmlist_lock);
	}
	spin_unlock(&mmlist_lock);
	mmput(prev_mm);

	avg_anon_fault /= mm_cnt;
	printk("ycc hyswp_psuedo_scan: scan_mm(%d)", mm_cnt);
	if ((swap_page_distribution))
		show_swap_page_distribution();
}

static void show_info()
{
	char msg[1024] = { 0 };
	int i;
	// return;
	/* vma anon page fault latency */
	printk("ycc hyswp_info anon_fault_lat fault>avg lat>lat (10^-6 sec), %llu, %llu, %llu", anon_fault,
	       anon_fault_lat / anon_fault, anon_fault_lat);
	
	/* app swap cache hit and ra */
	spin_lock(&distribution_lock);
	sprintf(msg, "swap_ra_hit");
	for (i = 0; i < total_app_slot; i++)
		sprintf(msg, "%s, %u", msg, app_swap_cache_hit[i]);
	printk("ycc hyswp_info swap_ra_hit,scan_round,%d, %s", scan_round, msg);	

	sprintf(msg, "swap_total_ra");
	for (i = 0; i < total_app_slot; i++)
		sprintf(msg, "%s, %u", msg, app_total_ra[i]);
	printk("ycc hyswp_info swap_total_ra,scan_round,%d, %s", scan_round, msg);

	/* swap on zram or flash */
	printk("ycc hyswp_info swap_in zram/flash scan_round(%d), %u, %u", scan_round, zram_in, flash_in);
	
	/* swap slot hole effect */
	printk("ycc hyswp_info swap_hole_effect scan_round(%d), %u, %u", scan_round, actual_prefetch, virt_prefetch);

	spin_unlock(&distribution_lock);
}

static int hyswp_migrate(void *p)
{
	// int nid;
	int scan_secs;
	zram_usage = 200;
	// allow_signal(SIGUSR1);

	scan_round = 0;
	init_statistic();

	/* app swap cache hit and ra */
	// memset(app_swap_cache_hit, 0, sizeof(app_swap_cache_hit));
	// memset(app_total_ra, 0, sizeof(app_total_ra));

	for (;;) {
		if (kthread_should_stop())
			break;
		// if (signal_pending(current)) {
		//     flush_signals(current);
		//     printk("ycc hyswp_migrate: userspace explicitly wants to scan...");
		// }

		scan_secs = (hyswp_scan_sec == 0 ? 120 : hyswp_scan_sec);
		BUG_ON(scan_secs <= 0);
		zram_usage = get_zram_usage();
		local_zram_usage = zram_usage;
		printk("ycc hyswp_migrate: (%d) zram_usage (%d), swapcache(%d)", scan_round,
		       local_zram_usage, total_swapcache_pages());
		scan_round++;
		if (dispatch_enable && check_hybird_swap()) {
			// for_each_node_state(nid, N_MEMORY)
			//     knewanond_scan_node(NODE_DATA(nid));
			if (local_zram_usage >= zran_usage_th && local_zram_usage <= 100) {
				start_zram_migrate();
			}
		} else if (swap_page_distribution) {
			scan_mm_swap_page_count();
		}
		show_info();
		schedule_timeout_interruptible(scan_secs * HZ);
	}

	return 0;
}

struct task_struct *thread = NULL;

void hyswp_migrate_stop(void)
{
	if (thread) {
		kthread_stop(thread);
		thread = NULL;
		printk("ycc hyswp_migrate stop successfully!\n");
		return;
	}
	pr_err("ycc hyswp_migrate not exist.........\n");
}

static int __init hyswp_migrate_init(void)
{
	if (thread) {
		pr_err("ycc hyswp_migrate already start.........\n");
		return 0;
	}

	thread = kthread_run(hyswp_migrate, NULL, "hyswp_migrate");
	if (IS_ERR(thread)) {
		pr_err("ycc hyswp_migrate failed to start\n");
		return PTR_ERR(thread);
	}
	printk("ycc hyswp_migrate init successfully!\n");
	return 0;
}

module_init(hyswp_migrate_init);

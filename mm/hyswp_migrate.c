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

int hyswp_scan_sec = 30;
static unsigned short scan_round;
gfp_t my_gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_CMA;
volatile int zram_usage = 200;
static int local_zram_usage;
#define zran_usage_th 5
int page_demote_cnt = 0, page_promote_cnt = 0;
enum si_dev { zram_dev = 0, flash_dev };

/* statistic */
// static DEFINE_MUTEX(distribution_lock);
static DEFINE_SPINLOCK(distribution_lock);
bool show_page_distribution = false;
#define distribution 7
int mm_distribution[distribution];

// statistic
static void init_statistic()
{
	int i;
	// mutex_lock(&distribution_lock);
	spin_lock(&distribution_lock);
	for (i = 0; i < distribution; i++)
		mm_distribution[i] = 0;
	// mutex_unlock(&distribution_lock);
	spin_unlock(&distribution_lock);
}

static void put_mm_page_distribution(unsigned value, unsigned anon_pages, unsigned swap_pages)
{
	unsigned bucket = value / 5;
	spin_lock(&distribution_lock);
	bucket = bucket >= distribution ? distribution - 1 : bucket;
	mm_distribution[bucket] = mm_distribution[bucket] + anon_pages + swap_pages;
	spin_unlock(&distribution_lock);
}

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
	spin_lock(&distribution_lock);
	printk("ycc_each_mm,scan_round,%d,mm_value,%d,%d,%d,%d,%d,%d,%d", scan_round,
	       mm_distribution[0], mm_distribution[1], mm_distribution[2], mm_distribution[3],
	       mm_distribution[4], mm_distribution[5], mm_distribution[6]);
	spin_unlock(&distribution_lock);
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

	si = swap_info[si_type];
	pte = pte_offset_map(pmd, addr);
	do {
		if (!is_swap_pte(*pte))
			continue;

		entry = pte_to_swp_entry(*pte);
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

	if (show_page_distribution && scan_round >= 10)
		init_statistic();

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
		if (refault_ratio <= 10 && anon_size + swap_size > 5000) {
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

		cond_resched();
		spin_lock(&mmlist_lock);

		if (show_page_distribution && scan_round >= 10)
			put_mm_page_distribution(refault_ratio, anon_size, swap_size);
	}
	spin_unlock(&mmlist_lock);
	mmput(prev_mm);

	avg_anon_fault /= mm_cnt;
	printk("ycc hyswp_migrate: scan_mm(%d), demote_mm(%d), demote_page(%d), demote_1(%d), demote_2(%d), avg_anon_fault(%llu)",
	       mm_cnt, mm_demote_cnt, page_demote_cnt, mm_demote_1, mm_demote_2, avg_anon_fault);
	if ((show_page_distribution || show_fault_distribution) && scan_round >= 10)
		show_mm_distribution();
}

static int hyswp_migrate(void *p)
{
	// int nid;
	int scan_secs;
	zram_usage = 200;
	// allow_signal(SIGUSR1);

	scan_round = 0;
	show_page_distribution = show_fault_distribution ? false : show_page_distribution;
	init_statistic();

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
		}
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

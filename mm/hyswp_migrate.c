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
#include <linux/vmalloc.h>

int hyswp_scan_sec = 30;
static unsigned short scan_round;
gfp_t my_gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_CMA;
volatile int zram_usage = 200;
static int local_zram_usage;
#define zran_usage_th 5
int page_demote_cnt = 0, page_promote_cnt = 0;
int PMD_cnt = 0, vma_cnt = 0, large_vma_cnt = 0;
enum si_dev { zram_dev = 0, flash_dev };

/* migration */
struct sysinfo i;
unsigned free_page_cnt = 0, avail_page_cnt = 0;

/* zram idle */
static DEFINE_SPINLOCK(zram_idle_lock);
unsigned this_idle_cnt = 0, pre_idle_cnt = 0;
unsigned char *pre_zram_idle = NULL;
unsigned char *this_round_page_idle = NULL;
static bool (*zram_idle_check_ptr)(unsigned) = NULL;
enum zram_idle_stat_enum { zram_idle_NOOP = 0, zram_idle_wait, zram_idle_migration };
#define wait_stat_round 10 * 2
static unsigned mark_idle_round = 0, idle_scan_mm_value = 20, pre_idle_scan_mm_value = 10;
static unsigned zram_idle_stat = zram_idle_NOOP;
static unsigned zram_idle_migration_cnt = 0;
static bool zram_idle_migration_flag = false;
/* get zram access time */
static unsigned (*get_zram_access_time_ptr)(unsigned) = NULL;

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
unsigned long swap_ra_io = 0, swap_ra_cnt = 0;

/* avg swap_ra size */
unsigned long total_ra_cnt = 0;
unsigned long total_ra_size_cnt[10];
unsigned long actual_ra_page[max_ra_page];
unsigned long ra_io_cnt[max_ra_page];

/* app swap cache hit and ra */
#define total_app_slot 50
unsigned app_total_ra[total_app_slot];
unsigned app_swap_cache_hit[total_app_slot];

/* get zram access time */
unsigned long app_zram_age[total_app_slot];
unsigned long app_zram_cnt[total_app_slot];
int mm_uid;

/* app swap cache hit and ra */
void put_swap_ra_count(int app_uid, int ra_hit_flag)
{
	spin_lock(&distribution_lock);
	if (app_uid >= 10200 && app_uid < 10250) {
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
		app_zram_cnt[i] = app_zram_age[i] = 0;
	}

	/* avg swap_ra size */
	for (i = 0; i < 10; i++)
		total_ra_size_cnt[i] = 0;
	for (i = 0; i < max_ra_page; i++) {
		actual_ra_page[i] = 0;
		ra_io_cnt[i] = 0;
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

/* get zram access time */
void register_zram_access(unsigned (*zram_access_time)(unsigned))
{
	get_zram_access_time_ptr = zram_access_time;
	if (!get_zram_access_time_ptr)
		printk("ycc register zram access time fail");
	else
		printk("ycc register zram access time success");
}
EXPORT_SYMBOL(register_zram_access);

unsigned call_zram_access_time(unsigned index)
{
	if (get_zram_access_time_ptr)
		return get_zram_access_time_ptr(index);
	printk("ycc non of zram_access_ptr");
	return 0;
}

void put_zram_acc_time(int uid, unsigned acc_time)
{
	spin_lock(&distribution_lock);

	if (uid >= 10200 && uid < 10250) {
		int slot = uid % total_app_slot;
		app_zram_cnt[slot]++;
		app_zram_age[slot] += acc_time;
	}
	spin_unlock(&distribution_lock);
}

static void reset_zram_acc_time()
{
	int i;
	spin_lock(&distribution_lock);
	for (i = 0; i < total_app_slot; i++)
		app_zram_cnt[i] = app_zram_age[i] = 0;
	mm_uid = -1;
	spin_unlock(&distribution_lock);
}

/* zram idle */
void register_zram_idle(bool (*zram_idle_check)(unsigned))
{
	zram_idle_check_ptr = zram_idle_check;
	if (!zram_idle_check_ptr)
		printk("ycc register zram idle check fail");
	else
		printk("ycc register zram idle check success");
}
EXPORT_SYMBOL(register_zram_idle);

bool call_zram_idle_check(unsigned index)
{
	if (zram_idle_check_ptr)
		return zram_idle_check_ptr(index);
	printk("ycc non of zram_idle_ptr");
	return false;
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
	else
		si = swap_info[0];
	pte = pte_offset_map(pmd, addr);
	do {
		if (!is_swap_pte(*pte))
			continue;
		entry = pte_to_swp_entry(*pte);
		put_swap_page_distribution(swp_type(entry));
		/* get zram access time */
		if (swap_page_distribution)
			if (si && !swp_type(entry) && mm_uid >= 10200 && mm_uid < 10250) {
				offset = swp_offset(entry);
				if (si->swap_map[offset] && si->swap_map[offset] < SWAP_MAP_MAX) {
					unsigned acc_time = call_zram_access_time(offset);
					put_zram_acc_time(mm_uid, acc_time);
				}
			}
		if (swp_type(entry) != si_type)
			continue;
		offset = swp_offset(entry);
		page_ref_cnt = __swp_swapcount(swp_entry(swp_type(entry), offset));
		if (page_ref_cnt != 1 && si_type == zram_dev)
			continue;
		if (zram_idle_migration_flag) { // zram idle page migrate
			if (offset >= max_zram_idle_index - 200)
				continue;
			spin_lock(&zram_idle_lock);
			if (!call_zram_idle_check(offset)) {
				spin_unlock(&zram_idle_lock);
				continue;
			}
			spin_unlock(&zram_idle_lock);
		}
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
		if (!zram_idle_migration_flag) { // mark: tmp to count demote, promote
			page_demote_cnt++;
			count_vm_event(BALLOON_DEFLATE);
		} else {
			zram_idle_migration_cnt++; // zram idle page migration
			count_vm_event(BALLOON_INFLATE);
		}

		rotate_reclaimable_page(page);

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

static void pseudo_scan_vma(struct mm_struct *mm) // count large/small vma
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
		mm_uid = -1;
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

static void start_zram_idle_migrate()
{
	struct mm_struct *mm;
	struct mm_struct *prev_mm;
	struct list_head *p;
	unsigned long refault_ratio = 200;
	unsigned mm_cnt = 0, mm_demote_cnt = 0;
	unsigned long anon_size = 0, swap_size = 0;

	if (zram_idle_stat != zram_idle_migration) {
		printk("ycc hyswp_migrate: skip zram_idle_migration zram_idle_stat(%u)",
		       zram_idle_stat);
		return;
	}

	zram_idle_migration_flag = true;
	zram_idle_migration_cnt = 0;

	// start_idle_migrate:
	prev_mm = &init_mm;
	mmget(prev_mm);

	spin_lock(&mmlist_lock);
	p = &init_mm.mmlist;

	while ((p = p->next) != &init_mm.mmlist) {
		mm_uid = -1;
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
		if (zram_idle_migration_cnt < free_page_cnt) {
			if (refault_ratio > pre_idle_scan_mm_value &&
			    refault_ratio <= idle_scan_mm_value && swap_size) {
				mm_demote_cnt++;
				scan_vma(mm, zram_dev);
			}
		}
		cond_resched();
		spin_lock(&mmlist_lock);
	}
	spin_unlock(&mmlist_lock);
	mmput(prev_mm);

	if (zram_idle_migration_cnt < free_page_cnt) {
		pre_idle_scan_mm_value = idle_scan_mm_value;
		idle_scan_mm_value += 10;
		if (idle_scan_mm_value > 95)
			zram_idle_stat = zram_idle_NOOP;
		idle_scan_mm_value = min((unsigned)90, idle_scan_mm_value);
	}

	zram_idle_migration_flag = false;

	printk("ycc hyswp_migrate: scan_mm(%d), zram_idle_demote_mm(%d), mm_th(%d/%d), zram_idle_page_demote(%d)",
	       mm_cnt, mm_demote_cnt, pre_idle_scan_mm_value, idle_scan_mm_value,
	       zram_idle_migration_cnt);
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
	reset_zram_acc_time();
	while ((p = p->next) != &init_mm.mmlist) {
		mm_uid = -1;
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

		/* get zram access time */
		if (mm->owner && mm->owner->cred)
			mm_uid = mm->owner->cred->uid.val;

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

static void zram_idle_update()
{
	int i;
	swp_entry_t entry;
	/* zram idle */
	entry = swp_entry(0, 1);
	if (swp_swap_info(entry)) {
		struct swap_info_struct *si;
		si = get_swap_device(entry);
		if (si) {
			unsigned idle = 0, scan = 0, match_last_round_idle = 0;
			unsigned non_swp = 0;
			for (i = 1; i < si->pages - 1; i++) {
				if (si->swap_map[i]) {
					bool idle_flag;
					scan++;
					if (si->swap_map[i] >= SWAP_MAP_MAX) {
						non_swp++;
						continue;
					}
					spin_lock(&zram_idle_lock);
					idle_flag = call_zram_idle_check(i);
					spin_unlock(&zram_idle_lock);
					if (idle_flag)
						idle++;
					if (idle_flag && pre_zram_idle[i]) /* idle page update*/
						match_last_round_idle++;
				}
			}
			printk("ycc hyswp_info zram_idle_reg scan_round(%d), %u, %u, %u, %u, %u, bad, %u",
			       scan_round, idle, scan, match_last_round_idle, pre_idle_cnt,
			       this_idle_cnt, non_swp);

			/* idle page update*/
			if (idle > this_idle_cnt) {
				mark_idle_round = scan_round;
				zram_idle_stat = zram_idle_wait;
				idle_scan_mm_value = 20;
				pre_idle_scan_mm_value = 10;
				printk("ycc hyswp_info idle_zram_update mark_idle_round(%u)",
				       mark_idle_round);
				for (i = 1; i < si->pages - 1; i++) {
					pre_zram_idle[i] = this_round_page_idle[i];
					pre_idle_cnt = this_idle_cnt;
				}
			}
			this_idle_cnt = 0;
			for (i = 1; i < si->pages - 1; i++) {
				spin_lock(&zram_idle_lock);
				if (si->swap_map[i] && call_zram_idle_check(i)) {
					this_idle_cnt++;
					this_round_page_idle[i] = 1;
				} else
					this_round_page_idle[i] = 0;
				spin_unlock(&zram_idle_lock);
			}
			/* idle page update*/
			put_swap_device(si);
		}
	}

	if (scan_round % 30 == 0 && fault_zram_acc_time) {
		reset_zram_acc_time();
	}
}

static void show_info()
{
	char msg[1024] = { 0 };
	int i;
	// return;
	/* vma anon page fault latency */
	printk("ycc hyswp_info anon_fault_lat fault>avg lat>lat (10^-6 sec), %llu, %llu, %llu",
	       anon_fault, anon_fault_lat / anon_fault, anon_fault_lat);

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
	printk("ycc hyswp_info swap_in zram/flash scan_round(%d), %u, %u", scan_round, zram_in,
	       flash_in);

	/* swap slot hole effect */
	printk("ycc hyswp_info swap_hole_effect scan_round(%d), %u, %u", scan_round,
	       actual_prefetch, virt_prefetch);
	printk("ycc hyswp_info flash_swap_io_effect scan_round(%d), %u, %u", scan_round, swap_ra_io,
	       swap_ra_cnt);

	/* avg swap_ra size */
	if (total_ra_cnt != 0) {
		sprintf(msg, "ra_size");
		for (i = 0; i < 10; i++)
			sprintf(msg, "%s, %u", msg, total_ra_size_cnt[i]);
		printk("ycc hyswp_info swap_ra_cnt scan_round(%d), %s", scan_round, msg);
	}

	sprintf(msg, "actual_ra_page");
	for (i = 0; i < max_ra_page; i++)
		sprintf(msg, "%s, %u", msg, actual_ra_page[i]);
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);

	sprintf(msg, "ra_io_cnt");
	for (i = 0; i < max_ra_page; i++)
		sprintf(msg, "%s, %u", msg, ra_io_cnt[i]);
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);

	/* get zram access time */
	sprintf(msg, "zram_acc");
	for (i = 20; i < total_app_slot; i++) {
		if (app_zram_cnt[i])
			sprintf(msg, "%s, %llu", msg, app_zram_age[i] / app_zram_cnt[i]);
		else
			sprintf(msg, "%s, -1", msg);
	}
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);

	spin_unlock(&distribution_lock);
	printk("ycc hyswp_info -----------------------------------------------");
}

static int hyswp_migrate(void *p)
{
	// int nid;
	int scan_secs;
	zram_usage = 200;
	// allow_signal(SIGUSR1);

	scan_round = 0;

	/* zram idle */
	pre_zram_idle = vzalloc(max_zram_idle_index);
	this_round_page_idle = vzalloc(max_zram_idle_index);
	if (!pre_zram_idle || !this_round_page_idle)
		printk("ycc fail to alloc pre_zram_idle");

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
		// free page info
		si_meminfo(&i);
		free_page_cnt = i.freeram;
		avail_page_cnt = si_mem_available();

		printk("ycc hyswp_migrate: (%d) zram_usage (%d), swapcache(%d) free_pg(%u), avail_pg(%u)",
		       scan_round, local_zram_usage, total_swapcache_pages(), free_page_cnt,
		       avail_page_cnt);
		scan_round++;

		free_page_cnt = min((unsigned int)5000, free_page_cnt);
		zram_idle_update();
		if (dispatch_enable && check_hybird_swap()) {
			// for_each_node_state(nid, N_MEMORY)
			//     knewanond_scan_node(NODE_DATA(nid));
			if (local_zram_usage >= zran_usage_th && local_zram_usage <= 100) {
				zram_idle_migration_flag = false;
				start_zram_migrate();
				if (zram_idle_stat == zram_idle_migration)
					start_zram_idle_migrate();
			}
		} else if (swap_page_distribution) {
			scan_mm_swap_page_count();
		}
		if (zram_idle_stat == zram_idle_wait) {
			if (mark_idle_round + wait_stat_round <= scan_round)
				zram_idle_stat = zram_idle_migration;
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

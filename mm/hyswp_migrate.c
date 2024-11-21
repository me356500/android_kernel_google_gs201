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
#include <linux/swapfile.h>

int hyswp_scan_sec = 30;
static unsigned short scan_round;
gfp_t my_gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_CMA;
volatile int zram_usage = 200;
static int local_zram_usage;
#define zran_usage_th 5
int page_demote_cnt = 0, page_promote_cnt = 0;
int PMD_cnt = 0, vma_cnt = 0, large_vma_cnt = 0;
enum si_dev { zram_dev = 0, flash_dev };

unsigned long long total_compaction_cnt = 0;
unsigned long long total_valid_slot = 0;
unsigned long long min_valid_slot = 256;
unsigned long long max_valid_slot = 0;
unsigned long long comp_page = 0;
unsigned long long res_page = 0;
unsigned long long swp_out_page = 0;
unsigned long long swp_out_adj = 0;
unsigned long long swp_out_nadj = 0;
/* compaction setting: module parameter*/
unsigned flash_swap_block = 4096;
unsigned per_app_swap_slot = 256;
unsigned COMP_THRESHOLD = 50;

module_param_named(flash_swap_block, flash_swap_block, uint, 0644);
module_param_named(per_app_swap_slot, per_app_swap_slot, uint, 0644);
module_param_named(COMP_THRESHOLD, COMP_THRESHOLD, uint, 0644);
/* hybrid swap setting: module parameter */
static bool hyswp_enable = false, hyswp_migrate_enable = false;
/* sensitivity study */
static unsigned cold_app_threshold = 10;
static unsigned dormant_page_threshold = 2;
static unsigned dormant_page_th_devide = 1;
/* hybrid swap setting: module parameter */
module_param_named(hyswp_enable, hyswp_enable, bool, 0644);
module_param_named(hyswp_migrate_enable, hyswp_migrate_enable, bool, 0644);
/* sensitivity study */
module_param_named(cold_app_threshold, cold_app_threshold, uint, 0644);
module_param_named(dormant_page_threshold, dormant_page_threshold, uint, 0644);
module_param_named(dormant_page_th_devide, dormant_page_th_devide, uint, 0644);

bool get_hyswp_enable_flag(void)
{
	return hyswp_enable;
}

bool cold_app_identification(unsigned WA_ratio, unsigned long anon_size, unsigned long swap_size)
{
	if (WA_ratio > 100 || cold_app_threshold > 100)
		return false;
	if (WA_ratio <= cold_app_threshold && anon_size + swap_size > 5000)
		return true;
	return false;
}

#define total_app_slot 50

/* migration */
struct sysinfo mem_i;
unsigned free_page_cnt = 0, avail_page_cnt = 0;

/* zram idle */
unsigned this_idle_cnt = 0, pre_idle_cnt = 0;
static bool (*zram_idle_check_ptr)(unsigned) = NULL;
// Page Migration: number of page are migrated each round
static unsigned zram_idle_migration_cnt = 0;
// To identify whether Page Migrator is migrating cold apps or dormant pages
static bool zram_idle_migration_flag = false;
/* get zram access time */
static unsigned (*get_zram_access_time_ptr)(unsigned) = NULL;
/* flash swap access time*/
static DEFINE_SPINLOCK(swpin_ac_lock);
unsigned *flash_swap_ac_time = NULL;
atomic_long_t all_lifetime_swap_in, long_lifetime_swap_in;
atomic_long_t swap_in_refault_duration[4]; // <1, 1~2, >2 * avg lifetime

#define number_time_slot 20
#define time_slot 100
atomic_long_t app_swap_pages_of_time[20][number_time_slot];
atomic_long_t app_swap_in_of_time[20][number_time_slot];

/* statistic */
static DEFINE_SPINLOCK(distribution_lock);
#define distribution 20
int mm_distribution[distribution];
/* statistic zram, flash page*/
bool swap_page_distribution = true;
unsigned refault_value_global;
unsigned zram_distribution[distribution];
unsigned flash_distribution[distribution];

/* anon page fault latency */
unsigned long anon_fault = 0, anon_fault_lat = 0;
unsigned long anon_flash_lat = 0, anon_flash_lat_cnt = 0;
unsigned long anon_zram_lat = 0, anon_zram_lat_cnt = 0;

/* swap on zram or flash */
unsigned long zram_in = 0, flash_in = 0;
/* app swap in pattern */
atomic_long_t app_swap_in_zram[total_app_slot], app_swap_in_flash[total_app_slot];
unsigned long app_zram_page_cnt[total_app_slot], app_flash_page_cnt[total_app_slot];

/* swap slot hole effect */
unsigned long virt_prefetch = 0, actual_prefetch = 0;
unsigned long swap_ra_io = 0, swap_ra_cnt = 0;

/* avg swap_ra size */
unsigned long total_ra_cnt = 0;
unsigned long total_ra_size_cnt[10];
unsigned long actual_ra_page[max_ra_page];
unsigned long ra_io_cnt[max_ra_page];
unsigned long no_prefetch_cnt = 0, prefetch_cnt = 0;
unsigned long same_app_adj = 0, diff_app_adj = 0, same_app, diff_app = 0;
unsigned long pre_pid = -1, pre_offset = 0;
/* app swap cache hit and ra */
unsigned app_total_ra[total_app_slot];
unsigned app_swap_cache_hit[total_app_slot];
unsigned app_flash_ra[total_app_slot];
unsigned app_flash_ra_hit[total_app_slot];
atomic_long_t zram_ra_hit, flash_ra_hit, zram_ra_page, flash_ra_page;
/* per-app workingset_activate */
unsigned app_workingset_activate_ratio[total_app_slot];
unsigned app_mm_struct_count[total_app_slot];
/* system anon workingset_activate */
atomic_long_t anon_refault_page, anon_wa_refault;

/* get zram access time */
unsigned long app_zram_age[total_app_slot]; // reside in zram time
unsigned long app_zram_cnt[total_app_slot];
unsigned long app_refault_duration[total_app_slot]; // refault duration
unsigned long app_refault_cnt[total_app_slot];
unsigned long app_long_lifetime[total_app_slot]; // long refault duration count
unsigned long app_all_lifetime[total_app_slot]; // all refault duration count
int mm_uid;

#define page_zram_slot 4
unsigned long app_zram_distribution[page_zram_slot];
unsigned long per_app_swap_distribution
	[20]
	[page_zram_slot]; // in each app, active page = arr[x][0] + arr[x][1], dormant page = arr[x][2] + arr[x][3]

/* app-based swap readahead*/
#define total_proc_slot 10000
atomic_long_t app_ra_page[total_app_slot], app_ra_hit[total_app_slot],
	app_ra_window[total_app_slot];
atomic_long_t proc_ra_page[total_proc_slot], proc_ra_hit[total_proc_slot],
	proc_ra_window[total_proc_slot];

#ifdef swap_alloc_swap_ra_enable
/* app-based swap readahead*/
void set_app_ra_window(void)
{
	int i, ra_page, hit_page, ra_window;
	if (scan_round % 2 == 0) {
		for (i = 0; i < total_app_slot; i++) {
			ra_page = atomic_long_read(&app_ra_page[i]);
			hit_page = atomic_long_read(&app_ra_hit[i]);
			ra_window = atomic_long_read(&app_ra_window[i]);
			if (ra_page > 1000) {
				unsigned hit_rate = hit_page * 100 / ra_page;
				atomic_long_set(&app_ra_page[i], ra_page / 2);
				atomic_long_set(&app_ra_hit[i], hit_page / 2);
				if (ra_window <= 16 && ra_window >= 2) {
					if (hit_rate > 60)
						ra_window = min(16, ra_window * 2);
					else if (hit_rate < 40)
						ra_window = max(2, ra_window / 2);
					else
						continue;
					atomic_long_set(&app_ra_window[i], ra_window);
				}
			}
		}

		for (i = 0; i < total_proc_slot; i++) {
			ra_page = atomic_long_read(&proc_ra_page[i]);
			hit_page = atomic_long_read(&proc_ra_hit[i]);
			ra_window = atomic_long_read(&proc_ra_window[i]);
			if (ra_page > 1000) {
				unsigned hit_rate = hit_page * 100 / ra_page;
				atomic_long_set(&proc_ra_page[i], ra_page / 2);
				atomic_long_set(&proc_ra_hit[i], hit_page / 2);
				if (ra_window <= 16 && ra_window >= 2) {
					if (hit_rate > 60)
						ra_window = min(16, ra_window * 2);
					else if (hit_rate < 40)
						ra_window = max(2, ra_window / 2);
					else
						continue;
					atomic_long_set(&proc_ra_window[i], ra_window);
				}
			}
		}
	}
}

unsigned get_app_ra_window(int app_uid, int app_pid)
{
	unsigned ra_window = 1;
	if (app_uid >= 10220 && app_uid < 10245) {
		int slot = app_uid % total_app_slot;
		ra_window = atomic_long_read(&app_ra_window[slot]);
		return ra_window;
	}
	if (app_pid >= 0 && app_pid < total_proc_slot) {
		ra_window = atomic_long_read(&proc_ra_window[app_pid]);
		return ra_window;
	}
	return ra_window;
}
#endif

void put_app_swap_in_pattern(int page_uid, unsigned si_type)
{
	if (page_uid >= 10220 && page_uid < 10245) {
		int slot = page_uid % total_app_slot;
		if (!si_type)
			atomic_long_inc(&app_swap_in_zram[slot]);
		else
			atomic_long_inc(&app_swap_in_flash[slot]);
	}
}

/* app swap cache hit and ra */
void put_swap_ra_count(int app_uid, int app_pid, int ra_hit_flag, int swap_type)
{
	/* global ra statistic */
	if (ra_hit_flag) {
		if (swap_type)
			atomic_long_inc(&flash_ra_hit);
		else
			atomic_long_inc(&zram_ra_hit);
	} else {
		if (swap_type)
			atomic_long_inc(&flash_ra_page);
		else
			atomic_long_inc(&zram_ra_page);
	}

	if (app_uid >= 10200 && app_uid < 10250) {
		int slot = app_uid % total_app_slot;
		spin_lock(&distribution_lock);
		if (ra_hit_flag)
			app_swap_cache_hit[slot]++;
		else
			app_total_ra[slot]++;
		if (swap_type) {
			if (ra_hit_flag)
				app_flash_ra_hit[slot]++;
			else
				app_flash_ra[slot]++;
		}
		spin_unlock(&distribution_lock);
		if (ra_hit_flag)
			atomic_long_inc(&app_ra_hit[slot]);
		else
			atomic_long_inc(&app_ra_page[slot]);
	}
	if (app_pid >= 0 && app_pid < total_proc_slot) {
		if (ra_hit_flag)
			atomic_long_inc(&proc_ra_hit[app_pid]);
		else
			atomic_long_inc(&proc_ra_page[app_pid]);
	}
}

/* per-app workingset_activate */
static void reset_app_workingset_activate()
{
	int i;
	for (i = 0; i < total_app_slot; i++)
		app_mm_struct_count[i] = app_workingset_activate_ratio[i] = 0;
}
void put_app_workingset_activate(int app_uid, unsigned value)
{
	if (app_uid >= 10200 && app_uid < 10250) {
		int slot = app_uid % total_app_slot;
		app_mm_struct_count[slot]++;
		if (app_workingset_activate_ratio[slot] == 0)
			app_workingset_activate_ratio[slot] = value;
		if (value < app_workingset_activate_ratio[slot])
			app_workingset_activate_ratio[slot] = value;
	}
}

// statistic init
static void init_statistic()
{
	int i;
	int j;
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
		app_workingset_activate_ratio[i] = 0;

		app_flash_ra[i] = app_flash_ra_hit[i] = 0;

		app_zram_page_cnt[i] = app_flash_page_cnt[i] = 0;
	}

	/* avg swap_ra size */
	for (i = 0; i < 10; i++)
		total_ra_size_cnt[i] = 0;
	for (i = 0; i < max_ra_page; i++) {
		actual_ra_page[i] = 0;
		ra_io_cnt[i] = 0;
	}

	spin_unlock(&distribution_lock);

	spin_lock(&swpin_ac_lock);
	for (i = 0; i < total_app_slot; i++) {
		app_zram_cnt[i] = app_zram_age[i] = 0;
		app_refault_duration[i] = app_refault_cnt[i] = 0;
		app_long_lifetime[i] = app_all_lifetime[i] = 0;
	}
	for (i = 0; i < page_zram_slot; i++)
		app_zram_distribution[i] = 0;
	for (j = 0; j < 20; j++) {
		for (i = 0; i < page_zram_slot; i++)
			per_app_swap_distribution[j][i] = 0;
	}
	spin_unlock(&swpin_ac_lock);
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
	unsigned avg_lifetime = 0;
	int page_slot = -1;
	if (uid >= 10200 && uid < 10250) {
		avg_lifetime = get_avg_refault_duration(uid);
		if (avg_lifetime)
			page_slot = acc_time / avg_lifetime;
		page_slot = min(page_slot, page_zram_slot - 1);
	}
	/* put zram page acc_time */
	if (uid >= 10200 && uid < 10250) {
		int slot = uid % total_app_slot;
		spin_lock(&swpin_ac_lock);
		app_zram_cnt[slot]++;
		app_zram_age[slot] += acc_time;
		if (page_slot != -1 && page_slot >= 0)
			app_zram_distribution[page_slot]++;
		if (page_slot != -1 && page_slot >= 0) {
			int app_slot = uid % 10220;
			if (app_slot < 20 && app_slot >= 0) {
				unsigned slot = acc_time / time_slot;
				slot = min(slot, (unsigned)number_time_slot - 1);
				per_app_swap_distribution[app_slot][page_slot]++;
				atomic_long_inc(&app_swap_pages_of_time[app_slot][slot]);
			}
		}
		spin_unlock(&swpin_ac_lock);
	}
}

void put_flash_acc_time(int uid, unsigned acc_time)
{
	unsigned avg_lifetime = 0;
	int page_slot = -1;
	if (uid >= 10200 && uid < 10250) {
		avg_lifetime = get_avg_refault_duration(uid);
		if (avg_lifetime)
			page_slot = acc_time / avg_lifetime;
		page_slot = min(page_slot, page_zram_slot - 1);
	}
	/* put zram page acc_time */
	if (uid >= 10200 && uid < 10250) {
		if (page_slot != -1 && page_slot >= 0) {
			int app_slot = uid % 10220;
			if (app_slot < 20 && app_slot >= 0) {
				unsigned slot = acc_time / time_slot;
				slot = min(slot, (unsigned)number_time_slot - 1);
				atomic_long_inc(&app_swap_pages_of_time[app_slot][slot]);
			}
		}
	}
}

void put_refault_duration(int uid, unsigned lifetime)
{
	/* put swap in lifetime*/
	spin_lock(&swpin_ac_lock);

	if (uid >= 10200 && uid < 10250) {
		int app_slot = uid % total_app_slot;
		app_refault_cnt[app_slot]++;
		app_refault_duration[app_slot] += lifetime;
		if (uid < 10240) {
			unsigned slot = lifetime / time_slot;
			app_slot = app_slot % 20;
			slot = min(slot, (unsigned)number_time_slot - 1);
			atomic_long_inc(&app_swap_in_of_time[app_slot][slot]);
		}
	}
	spin_unlock(&swpin_ac_lock);
}

/* flash swap access time*/
void update_flash_ac_time(unsigned long slot)
{
	struct timespec64 ts;
	ts = ktime_to_timespec64(ktime_get_boottime());
	spin_lock(&swpin_ac_lock);
	if (slot >= 0 && slot < max_flash_swap_slot)
		flash_swap_ac_time[slot] = (unsigned)ts.tv_sec;
	spin_unlock(&swpin_ac_lock);
}

unsigned get_flash_ac_time(unsigned long slot)
{
	unsigned ac_time = 0;
	spin_lock(&swpin_ac_lock);
	if (slot >= 0 && slot < max_flash_swap_slot)
		ac_time = flash_swap_ac_time[slot];
	spin_unlock(&swpin_ac_lock);
	// printk("ycc ac_time %u", ac_time);
	return ac_time;
}

unsigned get_avg_refault_duration(int uid)
{
	unsigned avg_lifetime = 0;
	spin_lock(&swpin_ac_lock);

	if (uid >= 10200 && uid < 10250) {
		int slot = uid % total_app_slot;
		if (app_refault_cnt[slot])
			avg_lifetime = app_refault_duration[slot] / app_refault_cnt[slot];
	}
	spin_unlock(&swpin_ac_lock);
	return avg_lifetime;
}

void put_app_lifetime_swap_in(int uid, bool long_lifetime)
{
	spin_lock(&swpin_ac_lock);
	if (uid >= 10200 && uid < 10250) {
		int slot = uid % total_app_slot;
		if (long_lifetime)
			app_long_lifetime[slot]++;
		app_all_lifetime[slot]++;
	}
	spin_unlock(&swpin_ac_lock);
}

static void reset_zram_acc_time()
{
	int i;
	int j;
	spin_lock(&swpin_ac_lock);
	for (i = 0; i < total_app_slot; i++)
		app_zram_cnt[i] = app_zram_age[i] = 0;
	for (i = 0; i < page_zram_slot; i++)
		app_zram_distribution[i] = 0;
	for (j = 0; j < 20; j++) {
		for (i = 0; i < page_zram_slot; i++)
			per_app_swap_distribution[j][i] = 0;
		for (i = 0; i < number_time_slot; i++)
			atomic_long_set(&app_swap_pages_of_time[j][i], 0);
	}
	mm_uid = -1;
	spin_unlock(&swpin_ac_lock);
	for (i = 0; i < total_app_slot; i++)
		app_zram_page_cnt[i] = app_flash_page_cnt[i] = 0;
}

static void reset_zram_lifetime()
{
	int i;
	if (scan_round % 30 == 0 && fault_zram_acc_time) {
		spin_lock(&swpin_ac_lock);
		for (i = 0; i < total_app_slot; i++) {
			if (scan_round == 30)
				app_refault_cnt[i] = app_refault_duration[i] = 0;
			else {
				app_refault_cnt[i] /= 2;
				app_refault_duration[i] /= 2;
			}
		}
		for (i = 0; i < total_app_slot; i++)
			app_long_lifetime[i] = app_all_lifetime[i] = 0;
		spin_unlock(&swpin_ac_lock);
		atomic_long_set(&all_lifetime_swap_in, 0);
		atomic_long_set(&long_lifetime_swap_in, 0);
		if (scan_round == 30)
			for (i = 0; i < 4; i++)
				atomic_long_set(&swap_in_refault_duration[i], 0);
	}
}

/* zram idle */
void register_zram_idle(bool (*zram_idle_check)(unsigned)) // unused, but compiler need it
{
	zram_idle_check_ptr = zram_idle_check;
	if (!zram_idle_check_ptr)
		printk("ycc register zram idle check fail");
	else
		printk("ycc register zram idle check success");
}
EXPORT_SYMBOL(register_zram_idle);

bool call_zram_idle_check(unsigned index) // unused, but compiler need it
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
	sprintf(msg, "WA_value");
	for (i = 0; i < distribution; i++)
		sprintf(msg, "%s, %d", msg, mm_distribution[i]);
	spin_unlock(&distribution_lock);
	printk("ycc hyswp_info swap-in,scan_round,%d, %s", scan_round, msg);
}

/* statistic zram, flash page in each mm */
static void put_swap_page_distribution(int page_uid, unsigned si_type)
{
	unsigned bucket = refault_value_global / 5;
	bucket = bucket >= distribution ? distribution - 1 : bucket;
	if (si_type == zram_dev)
		zram_distribution[bucket]++;
	else
		flash_distribution[bucket]++;

	if (page_uid >= 10220 && page_uid < 10245) {
		int slot = page_uid % total_app_slot;
		if (!si_type)
			app_zram_page_cnt[slot]++;
		else
			app_flash_page_cnt[slot]++;
	}
}

static void show_swap_page_distribution()
{
	char msg[1024] = { 0 };
	int i;
	sprintf(msg, "WA_value");
	for (i = 0; i < distribution; i++)
		sprintf(msg, "%s, %u", msg, zram_distribution[i]);
	printk("ycc hyswp_info zram,scan_round,%d, %s", scan_round, msg);

	sprintf(msg, "WA_value");
	for (i = 0; i < distribution; i++)
		sprintf(msg, "%s, %u", msg, flash_distribution[i]);
	printk("ycc hyswp_info flash,scan_round,%d, %s", scan_round, msg);
}

/* zRAM Page Migration */
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
		put_swap_page_distribution(mm_uid, swp_type(entry));
		/* get zram access time */
		if (show_app_zram_acctime) {
			struct swap_info_struct *si_for_count;
			if (swp_type(entry) <= 1)
				si_for_count = swap_info[swp_type(entry)];
			else
				continue;
			if (si_for_count && !swp_type(entry) && mm_uid >= 10200 && mm_uid < 10250) {
				offset = swp_offset(entry);
				if (si_for_count->swap_map[offset] &&
				    si_for_count->swap_map[offset] < SWAP_MAP_MAX) {
					struct timespec64 ts;
					unsigned acc_time, lifetime;
					acc_time = call_zram_access_time(offset);
					ts = ktime_to_timespec64(ktime_get_boottime());
					lifetime = (unsigned)ts.tv_sec - acc_time;
					put_zram_acc_time(mm_uid, lifetime);
				}
			} else if (si_for_count && swp_type(entry) == 1 && mm_uid >= 10220 &&
				   mm_uid <= 10250) {
				offset = swp_offset(entry);
				if (si_for_count->swap_map[offset] &&
				    si_for_count->swap_map[offset] < SWAP_MAP_MAX) {
					struct timespec64 ts;
					unsigned acc_time, lifetime;
					acc_time = get_flash_ac_time(offset);
					ts = ktime_to_timespec64(ktime_get_boottime());
					lifetime = (unsigned)ts.tv_sec - acc_time;
					put_flash_acc_time(mm_uid, lifetime);
				}
			}
		}
		if (swp_type(entry) != si_type)
			continue;
		offset = swp_offset(entry);
		page_ref_cnt = __swp_swapcount(swp_entry(swp_type(entry), offset));
		if (page_ref_cnt != 1 && si_type == zram_dev)
			continue;
		if (zram_idle_migration_flag) { // zram idle page migrate
			struct timespec64 ts;
			unsigned acc_time = 0, lifetime = 0, lifetime_th = 0, avg_lifetime = 0;
			// ycc test
			if (si && !swp_type(entry) && mm_uid >= 10200 && mm_uid < 10250) {
				if (si->swap_map[offset] && si->swap_map[offset] < SWAP_MAP_MAX) {
					acc_time = call_zram_access_time(offset);
					ts = ktime_to_timespec64(ktime_get_boottime());
					lifetime = (unsigned)ts.tv_sec - acc_time;
				}
				/* count long lifetime swap-in ratio*/
				avg_lifetime = get_avg_refault_duration(mm_uid);
				lifetime_th = avg_lifetime * dormant_page_threshold /
					      dormant_page_th_devide;
				lifetime_th = min((unsigned)3000, lifetime_th);
				lifetime_th = max((unsigned)300, lifetime_th);
				// lifetime_th = 600;
				if (lifetime < lifetime_th)
					continue;
			}
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
		if (!zram_idle_migration_flag) { // mark: count number of demoted pages
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

static void pseudo_scan_vma(struct mm_struct *mm) // unused: count large/small vma
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

static void start_zram_migrate() // evicts cold app
{
	struct mm_struct *mm;
	struct mm_struct *prev_mm;
	struct list_head *p;
	unsigned long anon_WA_ratio = 200;
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
	reset_zram_acc_time();
	while ((p = p->next) != &init_mm.mmlist) {
		mm_uid = -1;
		anon_WA_ratio = 200;
		mm = list_entry(p, struct mm_struct, mmlist);
		if (!mmget_not_zero(mm))
			continue;
		spin_unlock(&mmlist_lock);
		mmput(prev_mm);
		prev_mm = mm;
		anon_size = get_mm_counter(mm, MM_ANONPAGES); // unit : page
		swap_size = get_mm_counter(mm, MM_SWAPENTS);

		if (mm->nr_anon_fault)
			anon_WA_ratio = mm->nr_anon_refault * 100 / mm->nr_anon_fault;
		mm_cnt++;
		avg_anon_fault += mm->nr_anon_fault;
		if (cold_app_identification(anon_WA_ratio, anon_size, swap_size)) {
			mm_demote_cnt++;
			mm_demote_1++;
			scan_vma(mm, zram_dev);
		} else if (anon_size + swap_size > 100000) {
			mm_demote_cnt++;
			mm_demote_2++;
			// scan_vma(mm, zram_dev);
		} else if ((anon_WA_ratio <= 15 && anon_size + swap_size > 70000)) {
			mm_demote_cnt++;
			mm_demote_3++;
			// scan_vma(mm, zram_dev);
		}

		/* per-app workingset_activate */
		if (mm->owner && mm->owner->cred)
			mm_uid = mm->owner->cred->uid.val;
		put_app_workingset_activate(mm_uid, anon_WA_ratio);

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
}

static void start_zram_idle_migrate() // evicts dormant page
{
	struct mm_struct *mm;
	struct mm_struct *prev_mm;
	struct list_head *p;
	unsigned long anon_WA_ratio = 200;
	unsigned mm_cnt = 0, mm_demote_cnt = 0;
	unsigned long anon_size = 0, swap_size = 0;

	zram_idle_migration_flag = true;
	zram_idle_migration_cnt = 0;

	// start_idle_migrate:
	prev_mm = &init_mm;
	mmget(prev_mm);

	spin_lock(&mmlist_lock);
	p = &init_mm.mmlist;
	reset_zram_acc_time();
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
			anon_WA_ratio = mm->nr_anon_refault * 100 / mm->nr_anon_fault;
		mm_cnt++;

		/* get zram access time */
		if (mm->owner && mm->owner->cred)
			mm_uid = mm->owner->cred->uid.val;

		if (zram_idle_migration_cnt < free_page_cnt) {
			scan_vma(mm, zram_dev); // start migrate
		}
		cond_resched();
		spin_lock(&mmlist_lock);
	}
	spin_unlock(&mmlist_lock);
	mmput(prev_mm);

	zram_idle_migration_flag = false;

	printk("ycc hyswp_migrate: scan_mm(%d), zram_idle_demote_mm(%d), zram_idle_page_demote(%d)",
	       mm_cnt, mm_demote_cnt, zram_idle_migration_cnt);
}

static void scan_mm_swap_page_count() // not do any migration, count zram and flash page in each mm
{
	/* statistic zram, flash page in each mm */
	struct mm_struct *mm;
	struct mm_struct *prev_mm;
	struct list_head *p;
	unsigned long anon_WA_ratio = 200;
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
	reset_app_workingset_activate();
	while ((p = p->next) != &init_mm.mmlist) {
		mm_uid = -1;
		anon_WA_ratio = 200;
		mm = list_entry(p, struct mm_struct, mmlist);
		if (!mmget_not_zero(mm))
			continue;
		spin_unlock(&mmlist_lock);
		mmput(prev_mm);
		prev_mm = mm;

		if (mm->nr_anon_fault)
			anon_WA_ratio = mm->nr_anon_refault * 100 / mm->nr_anon_fault;
		refault_value_global = anon_WA_ratio;

		mm_cnt++;
		avg_anon_fault += mm->nr_anon_fault;

		/* get zram access time */
		if (mm->owner && mm->owner->cred)
			mm_uid = mm->owner->cred->uid.val;
		/* per-app workingset_activate */
		put_app_workingset_activate(mm_uid, anon_WA_ratio);

		scan_vma(mm, 5); // si_type is invalid -> not do any migration
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

/* print log */
void print_swap_ra_log(void)
{
	char msg[1024] = { 0 };
	int i;
	unsigned long local_zram_ra, local_zram_hit, local_flash_ra, local_flash_hit;

	/* global ra statistic */
	/* section 4-d: prefetch and prefetch hit on flash */
	local_zram_hit = atomic_long_read(&zram_ra_hit);
	local_flash_hit = atomic_long_read(&flash_ra_hit);
	local_zram_ra = atomic_long_read(&zram_ra_page);
	local_flash_ra = atomic_long_read(&flash_ra_page);
	sprintf(msg, "global_ra zram_hit > ra > flash_hit > ra, %llu, %llu, %llu, %llu",
		local_zram_hit, local_zram_ra, local_flash_hit, local_flash_ra);
	printk("ycc hyswp_info, scan_round,%d, %s", scan_round, msg);

#ifdef swap_alloc_swap_ra_enable
	/* app-based swap readahead*/
	/* section 4-d: each app prefetch window size */
	sprintf(msg, "app_ra_window");
	for (i = 20; i < total_app_slot; i++) {
		int ra_window = atomic_long_read(&app_ra_window[i]);
		sprintf(msg, "%s, %u", msg, ra_window);
	}
	printk("ycc hyswp_info, scan_round,%d, %s", scan_round, msg);
	// unused
	sprintf(msg, "adaptive_app_ra_hit");
	for (i = 20; i < total_app_slot; i++) {
		int hit_page = atomic_long_read(&app_ra_hit[i]);
		sprintf(msg, "%s, %u", msg, hit_page);
	}
	printk("ycc hyswp_info, scan_round,%d, %s", scan_round, msg);
	// unused
	sprintf(msg, "adaptive_app_ra_page");
	for (i = 20; i < total_app_slot; i++) {
		int ra_page = atomic_long_read(&app_ra_page[i]);
		sprintf(msg, "%s, %u", msg, ra_page);
	}
	printk("ycc hyswp_info, scan_round,%d, %s", scan_round, msg);
#endif
	/* app swap cache hit and ra */
	spin_lock(&distribution_lock);
	sprintf(msg, "app_swap_ra_hit");
	for (i = 20; i < total_app_slot; i++)
		sprintf(msg, "%s, %u", msg, app_swap_cache_hit[i]);
	printk("ycc hyswp_info, scan_round,%d, %s", scan_round, msg);

	sprintf(msg, "app_swap_total_ra");
	for (i = 20; i < total_app_slot; i++)
		sprintf(msg, "%s, %u", msg, app_total_ra[i]);
	printk("ycc hyswp_info, scan_round,%d, %s", scan_round, msg);
	/* section 3-d: number of prefech on flash for each app */
	sprintf(msg, "app_flash_ra_hit");
	for (i = 20; i < total_app_slot; i++)
		sprintf(msg, "%s, %u", msg, app_flash_ra_hit[i]);
	printk("ycc hyswp_info, scan_round,%d, %s", scan_round, msg);
	/* section 3-d: number of prefech hit on flash for each app */
	sprintf(msg, "app_flash_ra");
	for (i = 20; i < total_app_slot; i++)
		sprintf(msg, "%s, %u", msg, app_flash_ra[i]);
	printk("ycc hyswp_info, scan_round,%d, %s", scan_round, msg);

	/* avg swap_ra size */
	/* section 2-b: major of swap-in doesn't prefetch */
	sprintf(msg, "swap_in_prefetch no_prefetch > prefetch");
	printk("ycc hyswp_info scan_round(%d), %s, %llu, %llu", scan_round, msg, no_prefetch_cnt,
	       prefetch_cnt);
	spin_unlock(&distribution_lock);
}

void print_swap_lifetime_log(void)
{
	int i, j;
	char msg[1024] = { 0 };
	long local_all_lifetime_swp_in, local_long_lifetime_swp_in;
	long local_anon_refault_page, local_anon_wa_refault;

	spin_lock(&swpin_ac_lock);

	/* system anon workingset_activate */
	// unused
	local_anon_refault_page = atomic_long_read(&anon_refault_page);
	local_anon_wa_refault = atomic_long_read(&anon_wa_refault);
	printk("ycc hyswp_info scan_round(%d), system_WA, %lld, %lld", scan_round,
	       local_anon_wa_refault, local_anon_refault_page);

	/* get zram access time */
	// unused
	sprintf(msg, "zram_acc");
	for (i = 20; i < total_app_slot; i++) {
		if (app_zram_cnt[i])
			sprintf(msg, "%s, %llu", msg, app_zram_age[i] / app_zram_cnt[i]);
		else
			sprintf(msg, "%s, -1", msg);
	}
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);
	/* section 3-c */
	/* get zram lifetime time */
	/* Dormant page threshold = app refault duration *2 */
	sprintf(msg, "refault_duration");
	for (i = 20; i < total_app_slot; i++) {
		if (app_refault_cnt[i])
			sprintf(msg, "%s, %llu", msg, app_refault_duration[i] / app_refault_cnt[i]);
		else
			sprintf(msg, "%s, -1", msg);
	}
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);

	/* number of dormant/active pages in zRAM for each app (only enable zRAM Page Admission) */
	for (j = 0; j < 20; j++) {
		if (j + 10220 >= 10224 && j + 10220 <= 10238) {
			sprintf(msg, "per_app_swap_distribution app_uid , %d,", j + 10220);
			for (i = 0; i < page_zram_slot; i++)
				sprintf(msg, "%s, %llu", msg, per_app_swap_distribution[j][i]);
			printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);
		}
	}
	// unused
	sprintf(msg, "longlife_rate");
	for (i = 20; i < total_app_slot; i++) {
		if (app_all_lifetime[i])
			sprintf(msg, "%s, %llu", msg,
				app_long_lifetime[i] * 100 / app_all_lifetime[i]);
		else
			sprintf(msg, "%s, -1", msg);
	}
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);
	/* section 3-c */
	local_all_lifetime_swp_in = atomic_long_read(&all_lifetime_swap_in);
	/* Dormant pages threshold: many page in zRAM > 2 * app refault duration (only enable zRAM Page Admission) */
	sprintf(msg, "zram_page_refault_duration");
	for (i = 0; i < page_zram_slot; i++) {
		// number of zram page with 4 entry: <1, 1~2, 2~3 , >3 app refault duration
		if (app_zram_distribution[i])
			sprintf(msg, "%s, %llu", msg, app_zram_distribution[i]);
		else
			sprintf(msg, "%s, -1", msg);
	}
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);
	spin_unlock(&swpin_ac_lock);
	/* Dormant pages threshold: large proportion of swap in < 2 * app refault duration (only enable zRAM Page Admission) */
	sprintf(msg, "swap_in_refault_duration");
	// number of swap-in with 4 entry: <1, 1~2, >2, (* refault duration). The fouth entry is unused
	for (i = 0; i < 4; i++)
		sprintf(msg, "%s, %llu", msg, atomic_long_read(&swap_in_refault_duration[i]));
	printk("ycc hyswp_info scan_round(%d), %s, all, %lld", scan_round, msg,
	       local_all_lifetime_swp_in);
	//unused
	local_long_lifetime_swp_in = atomic_long_read(&long_lifetime_swap_in);
	printk("ycc hyswp_info scan_round(%d), overall-lifetime %lld, %lld", scan_round,
	       local_long_lifetime_swp_in, local_all_lifetime_swp_in);
}

void print_swap_distribution_log(void)
{
	int i, j;
	char msg[1024] = { 0 };
	/* per app swap pattern */
	/* section 2-c : motivative observation */
	/* swap-in cdf */
	printk("ycc hyswp_info *---app distribution start---*");
	for (j = 0; j < 20; j++) {
		if (j + 10220 >= 10224 && j + 10220 <= 10238) {
			sprintf(msg, "app_swap_in_of_time(100s) app_uid , %d,", j + 10220);
			for (i = 0; i < number_time_slot; i++) {
				long swap_in_event = atomic_long_read(&app_swap_in_of_time[j][i]);
				sprintf(msg, "%s, %llu", msg, swap_in_event);
			}
			printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);
		}
	}
	/* number of pages stay in swap in each 100s */
	for (j = 0; j < 20; j++) {
		if (j + 10220 >= 10224 && j + 10220 <= 10238) {
			sprintf(msg, "app_swap_page_of_time(100s) app_uid , %d,", j + 10220);
			for (i = 0; i < number_time_slot; i++) {
				long swap_page = atomic_long_read(&app_swap_pages_of_time[j][i]);
				sprintf(msg, "%s, %llu", msg, swap_page);
			}
			printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);
		}
	}
	printk("ycc hyswp_info *---app distribution end---*");
}

static void show_info()
{
	char msg[1024] = { 0 };
	int i, j;
	i = j = 0;
	// return;
	/* vma anon page fault latency */
	// unused
	if (anon_fault)
		printk("ycc hyswp_info anon_fault_lat fault>avg lat>lat (10^-6 sec), %llu, %llu, %llu",
		       anon_fault, anon_fault_lat / anon_fault, anon_fault_lat);
	
	/* zRAM and flash average swap in latency */
	if (anon_zram_lat_cnt && anon_flash_lat_cnt)
		printk("ycc hyswp_info dev_avg_fault_lat zram > flash (10^-6 sec), %llu, %llu",
		       anon_zram_lat / anon_zram_lat_cnt, anon_flash_lat / anon_flash_lat_cnt);

	print_swap_ra_log();
	/* app swap cache hit and ra */
	spin_lock(&distribution_lock);

	/* section 2-b,3-c,4-c: number of swap in occur on zran/flash */
	/* swap on zram or flash */
	printk("ycc hyswp_info swap_in zram/flash scan_round(%d), %u, %u", scan_round, zram_in,
	       flash_in);

	spin_unlock(&distribution_lock);

	sprintf(msg, "app_mm_struct_cnt");
	for (i = 20; i < total_app_slot; i++) {
		sprintf(msg, "%s, %u", msg, app_mm_struct_count[i]);
	}
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);

	/* section 3-b: anon. WA ratio in each app */
	sprintf(msg, "per_app_WA_ratio");
	for (i = 20; i < total_app_slot; i++) {
		sprintf(msg, "%s, %u", msg, app_workingset_activate_ratio[i]);
	}
	printk("ycc hyswp_info scan_round(%d), %s", scan_round, msg);

	print_swap_lifetime_log();

	print_swap_distribution_log();

	printk("ycc hyswp_info -----------------------------------------------");

	//printk("wyc swapin_info, %d, %d, %d, %d\n", same_app_adj, same_app, diff_app_adj, diff_app);
	printk("wyc migrate_info, %d, %d\n", total_compaction_cnt, (total_valid_slot / total_compaction_cnt));
	printk("wyc migrate_amount, %d, %d, %d\n", res_page, comp_page, swp_out_page);
	printk("wyc minmax_valid_slot, %d, %d\n", min_valid_slot, max_valid_slot);
	printk("wyc swap_out_continuoty, %d, %d\n", swp_out_adj, swp_out_nadj);
}

static int hyswp_migrate(void *p)
{
	// int nid;
	int scan_secs;
	int i, j;
	zram_usage = 200;
	// allow_signal(SIGUSR1);

	atomic_long_set(&all_lifetime_swap_in, 0);
	atomic_long_set(&long_lifetime_swap_in, 0);
	atomic_long_set(&anon_refault_page, 0);
	atomic_long_set(&anon_wa_refault, 0);

	atomic_long_set(&zram_ra_hit, 0);
	atomic_long_set(&flash_ra_hit, 0);
	atomic_long_set(&zram_ra_page, 0);
	atomic_long_set(&flash_ra_page, 0);

	for (i = 0; i < 4; i++)
		atomic_long_set(&swap_in_refault_duration[i], 0);

	for (i = 0; i < total_app_slot; i++) {
		atomic_long_set(&app_ra_page[i], 1);
		atomic_long_set(&app_ra_hit[i], 0);
		atomic_long_set(&app_ra_window[i], 4);
		/* app swap in pattern */
		atomic_long_set(&app_swap_in_zram[i], 0);
		atomic_long_set(&app_swap_in_flash[i], 0);
	}

	for (i = 0; i < total_proc_slot; i++) {
		atomic_long_set(&proc_ra_page[i], 1);
		atomic_long_set(&proc_ra_hit[i], 0);
		atomic_long_set(&proc_ra_window[i], 4);
	}

	for (j = 0; j < 20; j++) {
		for (i = 0; i < number_time_slot; i++) {
			atomic_long_set(&app_swap_in_of_time[j][i], 0);
			atomic_long_set(&app_swap_pages_of_time[j][i], 0);
		}
	}

	scan_round = 0;

	spin_lock(&swpin_ac_lock);
	flash_swap_ac_time = vzalloc(max_flash_swap_slot * sizeof(unsigned));
	spin_unlock(&swpin_ac_lock);
	if (!flash_swap_ac_time)
		printk("ycc fail to alloc flash_swap_ac_time");

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
		// free page info
		si_meminfo(&mem_i);
		free_page_cnt = mem_i.freeram;
		avail_page_cnt = si_mem_available();

		printk("ycc hyswp_migrate: (%d) zram_usage (%d), swapcache(%d) free_pg(%u), avail_pg(%u), hyswp_start(%d), migrate_start(%d)",
		       scan_round, local_zram_usage, total_swapcache_pages(), free_page_cnt,
		       avail_page_cnt, hyswp_enable, hyswp_migrate_enable);
		scan_round++;

		free_page_cnt /= 3;
		free_page_cnt = min((unsigned int)10000, free_page_cnt);
		reset_zram_lifetime();
		reset_app_workingset_activate();
		if (hyswp_migrate_enable && check_hybird_swap()) {
			// for_each_node_state(nid, N_MEMORY)
			//     knewanond_scan_node(NODE_DATA(nid));
			if (local_zram_usage >= zran_usage_th && local_zram_usage <= 100) {
				zram_idle_migration_flag = false;
				start_zram_migrate(); // evicts cold app
				start_zram_idle_migrate(); // evicts dormant page
			}
		}
		scan_mm_swap_page_count(); // only for statistic, not do any migration

		if (show_fault_distribution && scan_round >= 3)
			show_mm_distribution();

#ifdef swap_alloc_swap_ra_enable
		/* app-based swap readahead*/
		set_app_ra_window();
#endif

		show_info(); // print log
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

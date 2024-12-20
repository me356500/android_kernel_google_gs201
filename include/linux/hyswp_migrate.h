#ifndef _LINUX_MM_HYSWP_MIGRATE_H
#define _LINUX_MM_HYSWP_MIGRATE_H

/* swap alloc */
/* enable Flash Swap Alloaction Module and App-Base Prefetch policy */
#define swap_alloc_enable
#define swap_alloc_swap_ra_enable
/* hybrid swap */
extern int hyswp_scan_sec;
extern volatile int zram_usage;

extern bool get_hyswp_enable_flag(void);

extern bool cold_app_identification(unsigned WA_ratio, unsigned long anon_size, unsigned long swap_size);

/* zram idle */
// #define max_zram_idle_index (3 * 1024 * 1024 / 4 + 100) 
// unused 
extern unsigned char *pre_zram_idle, *this_round_page_idle; // unused
extern void register_zram_idle(bool (*zram_idle_check)(unsigned)); // unused
extern bool call_zram_idle_check(unsigned index); // unused
/* get zram access time */
#define fault_zram_acc_time true
#define show_app_zram_acctime true
extern void put_refault_duration(int uid, unsigned acc_time);
extern unsigned call_zram_access_time(unsigned index);
/* flash swap access time*/
#define swap_page_8GB (8 * 1024 * 1024 / 4)
#define max_flash_swap_slot (12 * 1024 * 1024 / 4 + 100)
// 8GB flash swap
extern void update_flash_ac_time(unsigned long slot);
extern unsigned get_flash_ac_time(unsigned long slot);
extern unsigned get_avg_refault_duration(int uid);
extern atomic_long_t all_lifetime_swap_in;
extern atomic_long_t long_lifetime_swap_in;
extern atomic_long_t swap_in_refault_duration[4]; // number of swap-in with 4 entry: <1, 1~2, >2, (* avg refault duration) . The fouth entry is unused
extern void put_app_lifetime_swap_in(int uid, bool long_lifetime);

#ifdef swap_alloc_swap_ra_enable
/* app-based swap readahead*/
extern unsigned get_app_ra_window(int app_uid, int app_pid);
extern unsigned get_app_same_vma_window(int app_uid, int app_pid);
#endif

/* statistic */
#define show_fault_distribution true
extern void put_mm_fault_distribution(unsigned value);
/* anon page fault latency */
extern unsigned long anon_fault, anon_fault_lat;
extern unsigned long anon_flash_lat, anon_flash_lat_cnt;
extern unsigned long anon_zram_lat, anon_zram_lat_cnt;
/* app swap cache hit and ra */
extern void put_swap_ra_count(int app_uid, int app_pid, int ra_hit_flag, int swap_type);
/* swap on zram or flash */
extern unsigned long zram_in, flash_in;
extern void put_app_swap_in_pattern(int page_uid, unsigned si_type);
/* swap slot hole effect */
extern unsigned long virt_prefetch, actual_prefetch;
extern unsigned long swap_ra_io, swap_ra_cnt;
/* avg swap_ra size */
extern unsigned long total_ra_size_cnt[10], total_ra_cnt;
/* swap_ra statistic */
#define max_ra_page 20
extern unsigned long actual_ra_page[max_ra_page];
extern unsigned long ra_io_cnt[max_ra_page];
extern unsigned long no_prefetch_cnt, prefetch_cnt;
/* swap-in continuity */
extern unsigned long same_app_adj, diff_app_adj, same_app, diff_app;
extern unsigned long pre_pid, pre_offset;
/* system anon workingset_activate */
extern atomic_long_t anon_refault_page, anon_wa_refault;

/* swap compaction */
extern unsigned long long total_compaction_cnt;
extern unsigned long long total_valid_slot;
extern unsigned long long min_valid_slot;
extern unsigned long long max_valid_slot;
extern unsigned long long comp_page;
extern unsigned long long res_page;
extern unsigned long long swp_out_page;

extern bool fixed_prefetch;
extern unsigned prefetch_window_size;
extern bool per_app_vma_prefetch;
extern bool per_app_ra_prefetch;
extern bool drop_diff_vma_page;
extern bool extend_large_window;
extern unsigned overflow_fixed_window;
#endif /* _LINUX_MM_HYSWP_MIGRATE_H */
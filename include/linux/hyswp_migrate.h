#ifndef _LINUX_MM_HYSWP_MIGRATE_H
#define _LINUX_MM_HYSWP_MIGRATE_H

/* swap alloc */
// #define swap_alloc_enable
// #define swap_alloc_swap_ra_enable
/* hybrid swap */
#define hyswp_enable true
#define dispatch_enable true
#define anon_refault_active_th 10
extern int hyswp_scan_sec;
extern volatile int zram_usage;

/* zram idle */
#define max_zram_idle_index (3 * 1024 * 1024 / 4 + 100)
extern unsigned char *pre_zram_idle, *this_round_page_idle;
extern void register_zram_idle(bool (*zram_idle_check)(unsigned));
extern bool call_zram_idle_check(unsigned index);
/* get zram access time */
#define fault_zram_acc_time true
#define show_app_zram_acctime true
extern void put_refault_duration(int uid, unsigned acc_time);
extern unsigned call_zram_access_time(unsigned index);
/* flash swap access time*/
#define max_flash_swap_slot (8 * 1024 * 1024 / 4 + 100)
// 8GB flash swap
extern void update_flash_ac_time(unsigned long slot);
extern unsigned get_flash_ac_time(unsigned long slot);
extern unsigned get_avg_refault_duration(int uid);
extern atomic_long_t all_lifetime_swap_in;
extern atomic_long_t long_lifetime_swap_in;
extern atomic_long_t avg_lifetime_distribution[4]; // 1, 1.5, 2 * avg lifetime
extern void put_app_lifetime_swap_in(int uid, bool long_lifetime);


/* statistic */
#define show_fault_distribution false
extern void put_mm_fault_distribution(unsigned value);
/* anon page fault latency */
extern unsigned long anon_fault, anon_fault_lat;
/* app swap cache hit and ra */
extern void put_swap_ra_count(int app_uid, int ra_hit_flag);
/* swap on zram or flash */
extern unsigned long zram_in, flash_in;
/* swap slot hole effect */
extern unsigned long virt_prefetch, actual_prefetch;
extern unsigned long swap_ra_io, swap_ra_cnt;
/* avg swap_ra size */
extern unsigned long total_ra_size_cnt[10], total_ra_cnt;
/* swap_ra statistic */
#define max_ra_page 20
extern unsigned long actual_ra_page[max_ra_page];
extern unsigned long ra_io_cnt[max_ra_page];

#endif /* _LINUX_MM_HYSWP_MIGRATE_H */
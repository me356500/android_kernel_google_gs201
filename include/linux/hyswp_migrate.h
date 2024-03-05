#ifndef _LINUX_MM_HYSWP_MIGRATE_H
#define _LINUX_MM_HYSWP_MIGRATE_H

/* swap alloc */
#define swap_alloc_enable
#define swap_alloc_swap_ra_enable
/* hybrid swap */
#define hyswp_enable true
#define dispatch_enable true
#define anon_refault_active_th 10
extern int hyswp_scan_sec;
extern volatile int zram_usage;
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

#endif /* _LINUX_MM_HYSWP_MIGRATE_H */
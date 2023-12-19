#ifndef _LINUX_MM_HYSWP_MIGRATE_H
#define _LINUX_MM_HYSWP_MIGRATE_H

/* hybrid swap */
#define hyswp_enable true
#define dispatch_enable true
extern int hyswp_scan_sec;
extern volatile int zram_usage;
/* statistic */
#define show_fault_distribution false
extern void put_mm_fault_distribution(unsigned value);

#endif /* _LINUX_MM_HYSWP_MIGRATE_H */
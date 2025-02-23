#ifndef _LINUX_MM_HYSWP_MIGRATE_H
#define _LINUX_MM_HYSWP_MIGRATE_H

extern int hyswp_scan_sec;
extern volatile int zram_usage;

extern void update_flash_ac_time(unsigned long slot);
extern unsigned get_flash_ac_time(unsigned long slot);
extern unsigned get_avg_refault_duration(int uid);
extern void put_refault_duration(int uid, unsigned acc_time);
extern void put_app_swap_in_pattern(int page_uid, unsigned si_type);
#define max_flash_swap_slot (8 * 1024 * 1024 / 4)

#define max_ra_page 20

#endif /* _LINUX_MM_HYSWP_MIGRATE_H */
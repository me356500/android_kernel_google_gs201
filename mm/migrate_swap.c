#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/swap.h>
#include <linux/swapfile.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/plist.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include "internal.h"
#include <linux/pgtable.h>
#include <linux/shmem_fs.h>


static struct task_struct *migrate_thread;

static bool swap_migrate_enable = false;
module_param_named(swap_migrate_enable, swap_migrate_enable, bool, 0644);

static unsigned long type = 0;
module_param_named(type, type, ulong, 0644);

static unsigned int offset = 0;
module_param_named(offset, offset, uint, 0644);

static inline unsigned char swap_count(unsigned char ent)
{
	return ent & ~SWAP_HAS_CACHE;	/* may include COUNT_CONTINUED flag */
}

static bool check_pte_offset(struct vm_area_struct *vma, unsigned long addr, unsigned int offset)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    swp_entry_t entry;
    unsigned int tmp_offset;

    pgd = pgd_offset(vma->vm_mm, addr);
    if (pgd_none_or_clear_bad(pgd)) {
        return false;
    }
    p4d = p4d_offset(pgd, addr);
    if (p4d_none_or_clear_bad(p4d)) {
        return false;
    }
    pud = pud_offset(p4d, addr);
    if (pud_none_or_clear_bad(pud)) {
        return false;
    }
    pmd = pmd_offset(pud, addr);
    if (pmd_none_or_clear_bad(pmd)) {
        return false;
    }
    pte = pte_offset_map(pmd, addr);
    if (!pte)
        return false;

    if (!is_swap_pte(*pte))
        return false;

    entry = pte_to_swp_entry(*pte);
    tmp_offset = swp_offset(entry);

    if (tmp_offset != offset)
        return false;

    return true;
}

bool migrate_swap_page_one(struct page *page, struct vm_area_struct *vma, unsigned long addr, void *arg)
{
	struct swap_info_struct *si;
    swp_entry_t *entry = (swp_entry_t *)arg;
    unsigned int type = swp_type(*entry);
    unsigned int offset = swp_offset(*entry);
    struct page *newpage = NULL;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
	int ret = 0;
    unsigned char swap_map;
    
    if (!check_pte_offset(vma, addr, offset))
        return true;
    
    si = swap_info[type];
    
    pgd = pgd_offset(vma->vm_mm, addr);
    p4d = p4d_offset(pgd, addr);
    pud = pud_offset(p4d, addr);
    pmd = pmd_offset(pud, addr);
    pte = pte_offset_map(pmd, addr);
    
    pte_unmap(pte);
    swap_map = READ_ONCE(si->swap_map[offset]);
    newpage = lookup_swap_cache(*entry, vma, addr, 0);
    if (!newpage) {
        struct vm_fault vmf = {
            .vma = vma,
            .address = addr,
            .pmd = pmd,
        };
        newpage = swapin_readahead(*entry, GFP_HIGHUSER_MOVABLE,
                    &vmf, 1);
    }
    if (!newpage) {
        if (swap_map == 0 || swap_map == SWAP_MAP_BAD) {
            printk(KERN_ERR "[tyc] swap_map %u error\n", swap_map);
            return false;
        }
        printk(KERN_ERR "[tyc] swapin_readahead failed\n");
        return false;
    }

    lock_page(newpage);
    wait_on_page_writeback(newpage);
    ret = unuse_pte(vma, pmd, addr, *entry, newpage);
    if (ret < 0) {
        unlock_page(newpage);
        put_page(newpage);
        printk(KERN_ERR "[tyc] unuse_pte failed\n");
        return false;
    }

    try_to_free_swap(newpage);
    unlock_page(newpage);
    put_page(newpage);
    
    SetPageCompaction(newpage);
    rotate_reclaimable_page(newpage);

    // lru_add_drain();

    //if (PageCompaction(newpage))
        //printk(KERN_INFO "[tyc] offset %u anon_page %p set compaction\n", offset, newpage);

    return false;
}

bool migrate_swap_page_shmem_one(struct page *page, struct vm_area_struct *vma, unsigned long addr, void *arg)
{
	struct inode *inode = page->mapping->host;
    int error = 0;
    //swp_entry_t *entry = (swp_entry_t *)arg;
    //unsigned int offset = swp_offset(*entry);
    struct page *swap = xa_load(&page->mapping->i_pages, page->index);

    if (!xa_is_value(swap)) {
        printk(KERN_ERR "[tyc] shmem error not swap entry\n");
        return false;
    }

    error = shmem_swapin_page(inode, page->index,
					  &swap, SGP_CACHE,
					  mapping_gfp_mask(page->mapping),
					  vma, NULL);

    if (error != 0)
        return false;

    unlock_page(swap);
    put_page(swap);

    SetPageCompaction(swap);
    rotate_reclaimable_page(swap);

    // lru_add_drain();

    //if (PageCompaction(swap))
        //printk(KERN_INFO "[tyc] offset %u shmem %p set compaction\n", offset, swap);

    return false;
}

void migrate_swap_page(unsigned long type, unsigned int offset)
{
    swp_entry_t entry = swp_entry(type, offset);
    struct swap_info_struct *si = NULL;
    struct rmap_walk_control rwc = {
        .arg = &entry
	};
    struct page *page = alloc_page(GFP_KERNEL);
    int tmp_count;

    si = swap_info[type];
    page->mapping = si->rmap[offset].mapping;
    page->index = si->rmap[offset].index;

    if (!page->mapping) {
        printk(KERN_ERR "[tyc] offset %u error\n", offset);
        goto out;
    }

    tmp_count = swap_count(READ_ONCE(si->swap_map[offset]));
    if (tmp_count == SWAP_MAP_BAD) {
        printk(KERN_INFO "[tyc] migrate error SWAP_MAP_BAD\n");
        goto out;
    } else if (tmp_count == SWAP_MAP_SHMEM) {
        rwc.rmap_one = migrate_swap_page_shmem_one;
    } else {
        rwc.rmap_one = migrate_swap_page_one;
    }

    rmap_walk(page, &rwc);
out:
    __free_page(page);
}

static int migrate_thread_func(void *data)
{
    while (!kthread_should_stop()) {
        struct swap_info_struct *si = NULL;
        swp_entry_t entry;
        int tmp_count;

		if (!swap_migrate_enable)
			goto sleep;

        entry = swp_entry(type, offset);
        si = get_swap_device(entry);
        tmp_count = swap_count(READ_ONCE(si->swap_map[offset]));

        if (tmp_count == SWAP_MAP_BAD)
            goto free;
        else
            migrate_swap_page(type, offset);

free:
        if (si)
            put_swap_device(si);
        swap_migrate_enable = false;
sleep:
		msleep(10000); // Sleep for 10 seconds
    }
    return 0;
}

static int __init migrate_swap_init(void)
{
    migrate_thread = kthread_run(migrate_thread_func, NULL, "migrate_thread");
    if (IS_ERR(migrate_thread)) {
        printk(KERN_ERR "[tyc] Failed to create migrate thread\n");
        return PTR_ERR(migrate_thread);
    }
    printk(KERN_INFO "[tyc] migrate module loaded\n");

    return 0;
}

static void __exit migrate_swap_exit(void)
{
    if (migrate_thread) {
        kthread_stop(migrate_thread);
        printk(KERN_INFO "[tyc] migrate thread stopped\n");
    }
    printk(KERN_INFO "[tyc] migrate module unloaded\n");
}

module_init(migrate_swap_init);
module_exit(migrate_swap_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TSOU YI CHIEH");
MODULE_DESCRIPTION("Kernel module that prints log every 10 seconds");
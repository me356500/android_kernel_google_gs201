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
#include <linux/fs.h>

static struct file *filep = NULL;
static size_t ret;
static loff_t pos;
static char buf[256] = {0};

static struct task_struct *rmap_thread;
static int round = 0;

static bool save_log = false;
module_param_named(save_log, save_log, bool, 0644);

static inline unsigned char swap_count(unsigned char ent)
{
	return ent & ~SWAP_HAS_CACHE;	/* may include COUNT_CONTINUED flag */
}

struct rmap_arg {
    unsigned int offset;
    int count;
    int cur;
};

static bool print_vma_pid_address_one(struct page *page, struct vm_area_struct *vma, unsigned long addr, void *arg)
{
    // get vma owner pid
    struct task_struct *task = vma->vm_mm->owner;
    pid_t pid = task->pid;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    swp_entry_t entry;
    unsigned int tmp_offset;
    unsigned int offset = ((struct rmap_arg *)arg)->offset;
    int count = ((struct rmap_arg *)arg)->count;
    int cur = ((struct rmap_arg *)arg)->cur;

    if (cur == count) {
        return false;
    }

	pgd = pgd_offset(vma->vm_mm, addr);
    if (pgd_none_or_clear_bad(pgd)) {
        return true;
    }
    p4d = p4d_offset(pgd, addr);
    if (p4d_none_or_clear_bad(p4d)) {
        return true;
    }
    pud = pud_offset(p4d, addr);
    if (pud_none_or_clear_bad(pud)) {
        return true;
    }
    pmd = pmd_offset(pud, addr);
    if (pmd_none_or_clear_bad(pmd)) {
        return true;
    }
    pte = pte_offset_map(pmd, addr);
    if (!pte)
        return true;

    if (!is_swap_pte(*pte))
        return true;

    entry = pte_to_swp_entry(*pte);
    tmp_offset = swp_offset(entry);

    if (tmp_offset != offset)
        return true;

    sprintf(buf, "pid = %d, vma = %p, addr = %lx\n", pid, vma, addr);

    ret = kernel_write(filep, buf, strlen(buf), &pos);
    if (ret < 0) {
        printk(KERN_ERR "[tyc] Write to file error\n");
        return false;
    }

    ((struct rmap_arg *)arg)->cur++;

    return true;
}

static bool print_vma_pid_address_shmem_one(struct page *page, struct vm_area_struct *vma, unsigned long addr, void *arg)
{
    // get vma owner pid
    struct task_struct *task = vma->vm_mm->owner;
    pid_t pid = task->pid;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;

	pgd = pgd_offset(vma->vm_mm, addr);
    if (pgd_none_or_clear_bad(pgd)) {
        return true;
    }
    p4d = p4d_offset(pgd, addr);
    if (p4d_none_or_clear_bad(p4d)) {
        return true;
    }
    pud = pud_offset(p4d, addr);
    if (pud_none_or_clear_bad(pud)) {
        return true;
    }
    pmd = pmd_offset(pud, addr);
    if (pmd_none_or_clear_bad(pmd)) {
        return true;
    }
    pte = pte_offset_map(pmd, addr);
    if (!pte)
        return true;

    sprintf(buf, "pid = %d, vma = %p, addr = %lx\n", pid, vma, addr);

    ret = kernel_write(filep, buf, strlen(buf), &pos);
    if (ret < 0) {
        printk(KERN_ERR "[tyc] Write to file error\n");
        return false;
    }

    return true;
}

void print_vma_pid_address(struct swap_info_struct *si, struct rmap_arg *arg)
{
    struct rmap_walk_control rwc = {
        .arg = arg
	};

    struct page *page = alloc_page(GFP_KERNEL);
    unsigned int offset = ((struct rmap_arg *)arg)->offset;
    int tmp_count;

    page->mapping = si->rmap[offset].mapping;
    page->index = si->rmap[offset].index;

    if (!page->mapping) {
        printk(KERN_ERR "[tyc] offset %u error\n", offset);
        goto out;
    }

    tmp_count = swap_count(si->swap_map[offset]);

    if (tmp_count == SWAP_MAP_BAD) {
        printk(KERN_INFO "[tyc] print_rmap error SWAP_MAP_BAD\n");
        goto out;
    } else if (tmp_count == SWAP_MAP_SHMEM) {
        rwc.rmap_one = print_vma_pid_address_shmem_one;
    } else {
        rwc.rmap_one = print_vma_pid_address_one;
    }

	rmap_walk(page, &rwc);
out:
    __free_page(page);
}

char *generate_file_name(int round)
{
    char *file_name = kmalloc(256, GFP_KERNEL);
    sprintf(file_name, "/sdcard/log/log_%d.txt", round);
    return file_name;
}

static int rmap_thread_func(void *data)
{
    msleep(10000);  // wait for file system to be ready

    while (!kthread_should_stop()) {
        char *file_name;
        struct swap_info_struct *si = NULL;
        unsigned int offset;
        swp_entry_t entry;
        int count, tmp_count;
        struct rmap_arg arg;

        pos = 0;

        if (save_log == false)
            goto sleep;

        file_name = generate_file_name(round);

        filep = filp_open(file_name, O_RDWR | O_APPEND | O_CREAT, 0644);
        if (IS_ERR(filep)) {
            printk(KERN_ERR "[tyc] Open file %s error\n", file_name);
            printk(KERN_ERR "[tyc] Error code: %ld\n", PTR_ERR(filep));
            goto free;
        }

        plist_for_each_entry(si, &swap_active_head, list) {
            for (offset = 0; offset < si->max; offset++) {
                if (si->swap_map[offset] == 0)
                    continue;

                entry = swp_entry(si->type, offset);
                tmp_count = swap_count(si->swap_map[offset]);

                if (tmp_count == SWAP_MAP_BAD)
                    continue;
                
                if (tmp_count == SWAP_MAP_SHMEM) {
                    sprintf(buf, "offset = %u, SWAP_MAP_SHMEM\n", offset);
                    ret = kernel_write(filep, buf, strlen(buf), &pos);
                    if (ret < 0) {
                        printk(KERN_ERR "[tyc] Write to file error\n");
                        goto close;
                    }

                    arg.offset = offset;
                } else {
                    count = swp_swapcount(entry);
                    if (count == 0)
                        continue;

                    sprintf(buf, "offset = %u, count = %d\n", offset, count);
                    ret = kernel_write(filep, buf, strlen(buf), &pos);
                    if (ret < 0) {
                        printk(KERN_ERR "[tyc] Write to file error\n");
                        goto close;
                    }

                    arg.offset = offset;
                    arg.count = count;
                    arg.cur = 0;
                }
                print_vma_pid_address(si, &arg);
            }
        }

close:
        filp_close(filep, NULL);
free:
        kfree(file_name);
        round++;
sleep:
        save_log = false;
        msleep(10000); // Sleep for 10 seconds
    }
    return 0;
}

static int __init rmap_init(void)
{
    rmap_thread = kthread_run(rmap_thread_func, NULL, "rmap_thread");
    if (IS_ERR(rmap_thread)) {
        printk(KERN_ERR "[tyc] Failed to create rmap thread\n");
        return PTR_ERR(rmap_thread);
    }
    printk(KERN_INFO "[tyc] rmap module loaded\n");

    return 0;
}

static void __exit rmap_exit(void)
{
    if (rmap_thread) {
        kthread_stop(rmap_thread);
        printk(KERN_INFO "[tyc] rmap thread stopped\n");
    }
    printk(KERN_INFO "[tyc] rmap module unloaded\n");
}

module_init(rmap_init);
module_exit(rmap_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TSOU YI CHIEH");
MODULE_DESCRIPTION("Kernel module that prints log every 10 seconds");

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

static bool print_vma_pid_address_one(struct page *page, struct vm_area_struct *vma, unsigned long addr, void *arg)
{
    // get vma owner pid
    struct task_struct *task = vma->vm_mm->owner;
    pid_t pid = task->pid;

    sprintf(buf, "pid = %d, vma = %p, addr = %lx\n", pid, vma, addr);

    ret = kernel_write(filep, buf, strlen(buf), &pos);
    if (ret < 0) {
        printk(KERN_ERR "[tyc] Write to file error\n");
        return false;
    }

    return true;
}

void print_vma_pid_address(struct swap_info_struct *si, int offset)
{
    struct rmap_walk_control rwc = {
		.rmap_one = print_vma_pid_address_one,
	};

    struct page *page = alloc_page(GFP_KERNEL);

    page->mapping = si->rmap[offset].mapping;
    page->index = si->rmap[offset].index;

	rmap_walk(page, &rwc);
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
                entry = swp_entry(si->type, offset);
                tmp_count = swap_count(si->swap_map[offset]);

                if (tmp_count != SWAP_MAP_BAD && tmp_count != SWAP_MAP_SHMEM) {
                    count = swp_swapcount(entry);
                    if (count == 0)
                        continue;

                    sprintf(buf, "offset = %x, count = %d\n", offset, count);
                    ret = kernel_write(filep, buf, strlen(buf), &pos);
                    if (ret < 0) {
                        printk(KERN_ERR "[tyc] Write to file error\n");
                        goto close;
                    }

                    print_vma_pid_address(si, offset);
                }

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


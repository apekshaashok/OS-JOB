#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ======================= STRUCT ======================= */
struct container_entry {
    pid_t pid;
    char container_id[50];
    unsigned long soft_limit;
    unsigned long hard_limit;
    int warned;
    struct list_head list;
};

/* ======================= GLOBAL LIST ======================= */
static LIST_HEAD(container_list);
static DEFINE_MUTEX(container_lock);

/* --- device / timer --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ======================= RSS ======================= */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ======================= HELPERS ======================= */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ======================= TIMER ======================= */
static void timer_callback(struct timer_list *t)
{
    struct container_entry *entry, *tmp;

    mutex_lock(&container_lock);

    list_for_each_entry_safe(entry, tmp, &container_list, list) {

        long rss = get_rss_bytes(entry->pid);

        /* remove dead process */
        if (rss < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* soft limit */
        if (rss > entry->soft_limit && !entry->warned) {
            log_soft_limit_event(entry->container_id,
                                 entry->pid,
                                 entry->soft_limit,
                                 rss);
            entry->warned = 1;
        }

        /* hard limit */
        if (rss > entry->hard_limit) {
            kill_process(entry->container_id,
                         entry->pid,
                         entry->hard_limit,
                         rss);

            list_del(&entry->list);
            kfree(entry);
        }
    }

    mutex_unlock(&container_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ======================= IOCTL ======================= */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct container_entry *entry, *tmp;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        strcpy(entry->container_id, req.container_id);
        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;
        entry->warned = 0;

        mutex_lock(&container_lock);
        list_add(&entry->list, &container_list);
        mutex_unlock(&container_lock);

        printk(KERN_INFO "[monitor] Registered %s pid=%d\n",
               entry->container_id, entry->pid);

        return 0;
    }

    if (cmd == MONITOR_UNREGISTER) {

        mutex_lock(&container_lock);

        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                mutex_unlock(&container_lock);
                return 0;
            }
        }

        mutex_unlock(&container_lock);
        return -ENOENT;
    }

    return -EINVAL;
}

/* ======================= FILE OPS ======================= */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ======================= INIT ======================= */
static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    device_create(cl, NULL, dev_num, NULL, DEVICE_NAME);

    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, dev_num, 1);

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Loaded\n");
    return 0;
}

/* ======================= EXIT ======================= */
static void __exit monitor_exit(void)
{
    struct container_entry *entry, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&container_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&container_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");

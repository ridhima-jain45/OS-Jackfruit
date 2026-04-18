/*
 * monitor.c - Container Memory Monitor Kernel Module
 *
 * Implements a Linux Kernel Module (LKM) that:
 *   - Creates a character device at /dev/container_monitor
 *   - Accepts ioctl() calls from the user-space supervisor to
 *     register / unregister container PIDs with soft and hard
 *     memory limits.
 *   - Runs a kernel thread that checks RSS every second and:
 *       Soft limit exceeded → pr_warn (dmesg warning)
 *       Hard limit exceeded → send SIGKILL
 *   - Uses a mutex-protected linked list for tracked PIDs
 *   - Properly frees all resources on unload
 *
 * Build with:
 *   make module
 * Load with:
 *   sudo insmod monitor.ko
 * Verify device:
 *   ls -l /dev/container_monitor
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/pid.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Runtime Project Team");
MODULE_DESCRIPTION("Container Process Memory Monitor with Soft and Hard Limits");
MODULE_VERSION("1.0");

/* ═══════════════════════════════════════════════════════════
 *  PER-CONTAINER TRACKING ENTRY
 * ═══════════════════════════════════════════════════════════ */

struct pid_entry {
    pid_t          pid;
    unsigned long  soft_limit_bytes;   /* warn when RSS exceeds this */
    unsigned long  hard_limit_bytes;   /* kill when RSS exceeds this */
    char           container_id[32];
    int            soft_warned;        /* 1 after first soft-limit warning */
    struct list_head list;
};

/* Global state */
static LIST_HEAD(pid_list);
static DEFINE_MUTEX(pid_mutex);

/* Character device bookkeeping */
static int            major_number;
static struct class  *monitor_class;
static struct device *monitor_device;

/* Monitor kernel thread */
static struct task_struct *monitor_thread;

/* ═══════════════════════════════════════════════════════════
 *  HELPERS: memory usage and kill
 * ═══════════════════════════════════════════════════════════ */

/*
 * get_rss_kb() – return the Resident Set Size of a process in kilobytes.
 *
 * We hold rcu_read_lock() while dereferencing the task pointer and
 * accessing mm_struct.  get_mm_rss() is safe under this lock because
 * it only reads atomic counters.  The result is shifted from pages to
 * kibibytes using PAGE_SHIFT - 10 (PAGE_SHIFT is typically 12 on x86).
 */
static unsigned long get_rss_kb(pid_t pid)
{
    struct task_struct *task;
    unsigned long       rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm)
        rss = get_mm_rss(task->mm) << (PAGE_SHIFT - 10);
    rcu_read_unlock();

    return rss;
}

/*
 * kill_container() – send SIGKILL to a tracked process.
 *
 * Uses the same rcu_read_lock + find_vpid pattern as get_rss_kb().
 * send_sig() delivers the signal; the process will be reaped by the
 * user-space supervisor via waitpid()/SIGCHLD.
 */
static void kill_container(pid_t pid, const char *container_id)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        send_sig(SIGKILL, task, 0);
        pr_alert("[monitor] HARD LIMIT: killed pid=%d container=%s\n",
                 pid, container_id);
    } else {
        pr_info("[monitor] kill_container: pid=%d (%s) already gone\n",
                pid, container_id);
    }
    rcu_read_unlock();
}

/* ═══════════════════════════════════════════════════════════
 *  MONITOR KERNEL THREAD
 *
 *  Wakes every second and iterates the tracked-PID list.
 *  The mutex ensures we don't race with ioctl add/remove.
 *  We use list_for_each_entry (not _safe) here because we
 *  do not delete entries inside the loop; deletion happens
 *  only in the ioctl handler or on module exit.
 * ═══════════════════════════════════════════════════════════ */

static int monitor_fn(void *data)
{
    while (!kthread_should_stop()) {
        struct pid_entry *entry;

        mutex_lock(&pid_mutex);
        list_for_each_entry(entry, &pid_list, list) {
            unsigned long rss_kb = get_rss_kb(entry->pid);

            if (rss_kb == 0) {
                /* Process may have exited; supervisor will unregister it */
                continue;
            }

            /* Hard limit check (in bytes → convert rss_kb to bytes for comparison) */
            if (rss_kb * 1024UL > entry->hard_limit_bytes) {
                pr_alert("[monitor] HARD LIMIT exceeded: pid=%d container=%s "
                         "rss=%lukB limit=%luMiB\n",
                         entry->pid, entry->container_id,
                         rss_kb, entry->hard_limit_bytes >> 20);
                kill_container(entry->pid, entry->container_id);
            }
            /* Soft limit check – warn only once per registration */
            else if (rss_kb * 1024UL > entry->soft_limit_bytes &&
                     !entry->soft_warned) {
                entry->soft_warned = 1;
                pr_warn("[monitor] SOFT LIMIT exceeded: pid=%d container=%s "
                        "rss=%lukB limit=%luMiB\n",
                        entry->pid, entry->container_id,
                        rss_kb, entry->soft_limit_bytes >> 20);
            }
        }
        mutex_unlock(&pid_mutex);

        msleep(1000);   /* 1-second polling interval */
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  IOCTL HANDLER
 * ═══════════════════════════════════════════════════════════ */

static long monitor_ioctl(struct file *file, unsigned int cmd,
                           unsigned long arg)
{
    struct monitor_request  req;
    struct pid_entry       *entry, *tmp;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    switch (cmd) {

    /* ── REGISTER: add a new PID to the tracked list ── */
    case MONITOR_REGISTER:
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        memset(entry, 0, sizeof(*entry));
        entry->pid              = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned      = 0;
        strncpy(entry->container_id, req.container_id,
                sizeof(entry->container_id) - 1);

        mutex_lock(&pid_mutex);
        list_add_tail(&entry->list, &pid_list);
        mutex_unlock(&pid_mutex);

        pr_info("[monitor] registered pid=%d container=%s soft=%luMiB hard=%luMiB\n",
                req.pid, req.container_id,
                req.soft_limit_bytes >> 20,
                req.hard_limit_bytes >> 20);
        break;

    /* ── UNREGISTER: remove a PID from the tracked list ── */
    case MONITOR_UNREGISTER: {
        int found = 0;

        mutex_lock(&pid_mutex);
        list_for_each_entry_safe(entry, tmp, &pid_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                pr_info("[monitor] unregistered pid=%d container=%s\n",
                        req.pid, req.container_id);
                break;
            }
        }
        mutex_unlock(&pid_mutex);

        if (!found)
            return -ENOENT;
        break;
    }

    default:
        return -EINVAL;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  FILE OPERATIONS for /dev/container_monitor
 * ═══════════════════════════════════════════════════════════ */

static int monitor_open(struct inode *inode, struct file *file)
{
    pr_info("[monitor] device opened\n");
    return 0;
}

static int monitor_release(struct inode *inode, struct file *file)
{
    pr_info("[monitor] device closed\n");
    return 0;
}

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = monitor_open,
    .release        = monitor_release,
    .unlocked_ioctl = monitor_ioctl,
};

/* ═══════════════════════════════════════════════════════════
 *  MODULE INIT / EXIT
 * ═══════════════════════════════════════════════════════════ */

static int __init monitor_init(void)
{
    /* 1. Register character device (dynamic major number) */
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) {
        pr_alert("[monitor] failed to register char device: %d\n", major_number);
        return major_number;
    }
    pr_info("[monitor] registered with major number %d\n", major_number);

    /* 2. Create device class (visible under /sys/class/) */
    monitor_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(monitor_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        pr_alert("[monitor] failed to create device class\n");
        return PTR_ERR(monitor_class);
    }

    /* 3. Create device node (/dev/container_monitor automatically via udev) */
    monitor_device = device_create(monitor_class, NULL,
                                   MKDEV(major_number, 0),
                                   NULL, DEVICE_NAME);
    if (IS_ERR(monitor_device)) {
        class_destroy(monitor_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        pr_alert("[monitor] failed to create device\n");
        return PTR_ERR(monitor_device);
    }

    /* 4. Start monitor kernel thread */
    monitor_thread = kthread_run(monitor_fn, NULL, "container_monitor");
    if (IS_ERR(monitor_thread)) {
        device_destroy(monitor_class, MKDEV(major_number, 0));
        class_destroy(monitor_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        pr_alert("[monitor] failed to create kernel thread\n");
        return PTR_ERR(monitor_thread);
    }

    pr_info("[monitor] module loaded — /dev/%s ready\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct pid_entry *entry, *tmp;

    /* 1. Stop the polling thread */
    if (monitor_thread)
        kthread_stop(monitor_thread);

    /* 2. Free all tracked entries */
    mutex_lock(&pid_mutex);
    list_for_each_entry_safe(entry, tmp, &pid_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&pid_mutex);

    /* 3. Remove device node and class */
    device_destroy(monitor_class, MKDEV(major_number, 0));
    class_destroy(monitor_class);

    /* 4. Unregister char device */
    unregister_chrdev(major_number, DEVICE_NAME);

    pr_info("[monitor] module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

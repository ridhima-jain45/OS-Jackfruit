#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include <linux/kthread.h>
#include <linux/delay.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

#define SOFT_LIMIT_KB (100 * 1024)   // 100 MB
#define HARD_LIMIT_KB (200 * 1024)   // 200 MB

MODULE_LICENSE("GPL");

/* ========================= */
/*  Data Structures        */
/* ========================= */

struct pid_entry {
    pid_t pid;
    struct list_head list;
};

static LIST_HEAD(pid_list);
static DEFINE_MUTEX(pid_mutex);

static int major;
static struct task_struct *monitor_thread;

/* ========================= */
/* Memory Usage Function  */
/* ========================= */

static unsigned long get_mem_usage_kb(pid_t pid)
{
    struct task_struct *task;
    unsigned long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm) {
        rss = get_mm_rss(task->mm) << (PAGE_SHIFT - 10);
    }
    rcu_read_unlock();

    return rss;
}

/* ========================= */
/*  Kill Process           */
/* ========================= */

static void kill_process(pid_t pid)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        send_sig(SIGKILL, task, 0);
        printk(KERN_INFO "[monitor] Killed PID %d\n", pid);
    }
    rcu_read_unlock();
}

/* ========================= */
/*  Monitor Loop           */
/* ========================= */

static int monitor_fn(void *data)
{
    while (!kthread_should_stop()) {

        struct pid_entry *entry;

        mutex_lock(&pid_mutex);

        list_for_each_entry(entry, &pid_list, list) {
            unsigned long mem = get_mem_usage_kb(entry->pid);

            if (mem > HARD_LIMIT_KB) {
                printk(KERN_ALERT "[monitor] PID %d exceeded HARD limit (%lu KB)\n",
                       entry->pid, mem);
                kill_process(entry->pid);
            } 
            else if (mem > SOFT_LIMIT_KB) {
                printk(KERN_WARNING "[monitor] PID %d exceeded SOFT limit (%lu KB)\n",
                       entry->pid, mem);
            }
        }

        mutex_unlock(&pid_mutex);

        msleep(1000);  // check every 1 sec
    }

    return 0;
}

/* ========================= */
/*  IOCTL Handler          */
/* ========================= */

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    pid_t pid;
    struct pid_entry *entry, *tmp;

    if (copy_from_user(&pid, (pid_t __user *)arg, sizeof(pid)))
        return -EFAULT;

    switch (cmd) {

        case REGISTER_PID:
            entry = kmalloc(sizeof(*entry), GFP_KERNEL);
            if (!entry)
                return -ENOMEM;

            entry->pid = pid;

            mutex_lock(&pid_mutex);
            list_add(&entry->list, &pid_list);
            mutex_unlock(&pid_mutex);

            printk(KERN_INFO "[monitor] Registered PID %d\n", pid);
            break;

        case UNREGISTER_PID:
            mutex_lock(&pid_mutex);

            list_for_each_entry_safe(entry, tmp, &pid_list, list) {
                if (entry->pid == pid) {
                    list_del(&entry->list);
                    kfree(entry);
                    printk(KERN_INFO "[monitor] Unregistered PID %d\n", pid);
                    break;
                }
            }

            mutex_unlock(&pid_mutex);
            break;

        default:
            return -EINVAL;
    }

    return 0;
}

/* ========================= */
/*  File Operations        */
/* ========================= */

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ========================= */
/*  Init                   */
/* ========================= */

static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        printk(KERN_ALERT "[monitor] Failed to register device\n");
        return major;
    }

    printk(KERN_INFO "[monitor] Device registered with major %d\n", major);

    /* Start monitoring thread */
    monitor_thread = kthread_run(monitor_fn, NULL, "monitor_thread");
    if (IS_ERR(monitor_thread)) {
        unregister_chrdev(major, DEVICE_NAME);
        printk(KERN_ALERT "[monitor] Failed to create thread\n");
        return PTR_ERR(monitor_thread);
    }

    printk(KERN_INFO "[monitor] Module loaded\n");
    return 0;
}

/* ========================= */
/*  Exit                   */
/* ========================= */

static void __exit monitor_exit(void)
{
    struct pid_entry *entry, *tmp;

    /* Stop thread */
    if (monitor_thread)
        kthread_stop(monitor_thread);

    /* Cleanup list */
    mutex_lock(&pid_mutex);
    list_for_each_entry_safe(entry, tmp, &pid_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&pid_mutex);

    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "[monitor] Module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);


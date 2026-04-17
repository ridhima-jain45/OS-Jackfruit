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

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Container Memory Monitor");

/* ========================= */
/*  Data Structures          */
/* ========================= */

struct pid_entry {
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    char container_id[32];
    struct list_head list;
};

static LIST_HEAD(pid_list);
static DEFINE_MUTEX(pid_mutex);

static int major;
static struct task_struct *monitor_thread;

/* ========================= */
/* Memory Usage Function     */
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
/* Kill Process              */
/* ========================= */

static void kill_process(pid_t pid)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        send_sig(SIGKILL, task, 0);
        printk(KERN_ALERT "[monitor] Killed PID %d\n", pid);
    }
    rcu_read_unlock();
}

/* ========================= */
/* Monitor Loop              */
/* ========================= */

static int monitor_fn(void *data)
{
    while (!kthread_should_stop()) {

        struct pid_entry *entry;

        mutex_lock(&pid_mutex);

        list_for_each_entry(entry, &pid_list, list) {
            unsigned long mem_kb = get_mem_usage_kb(entry->pid);

            if (mem_kb > (entry->hard_limit >> 10)) {
                printk(KERN_ALERT "[monitor] PID %d (%s) exceeded HARD limit (%lu KB)\n",
                       entry->pid, entry->container_id, mem_kb);
                kill_process(entry->pid);
            }
            else if (mem_kb > (entry->soft_limit >> 10)) {
                printk(KERN_WARNING "[monitor] PID %d (%s) exceeded SOFT limit (%lu KB)\n",
                       entry->pid, entry->container_id, mem_kb);
            }
        }

        mutex_unlock(&pid_mutex);

        msleep(1000);  // check every 1 sec
    }

    return 0;
}

/* ========================= */
/* IOCTL Handler             */
/* ========================= */

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct pid_entry *entry, *tmp;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    switch (cmd) {

    case MONITOR_REGISTER:
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;
        strncpy(entry->container_id, req.container_id, sizeof(entry->container_id));

        mutex_lock(&pid_mutex);
        list_add(&entry->list, &pid_list);
        mutex_unlock(&pid_mutex);

        printk(KERN_INFO "[monitor] Registered PID %d (%s)\n",
               req.pid, req.container_id);
        break;

    case MONITOR_UNREGISTER:
    {
        int found = 0;

        mutex_lock(&pid_mutex);

        list_for_each_entry_safe(entry, tmp, &pid_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                printk(KERN_INFO "[monitor] Unregistered PID %d (%s)\n",
                       req.pid, req.container_id);
                found = 1;
                break;
            }
        }

        mutex_unlock(&pid_mutex);

        return found ? 0 : -ENOENT;
    }

    default:
        return -EINVAL;
    }

    return 0;
}

/* ========================= */
/* File Operations           */
/* ========================= */

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ========================= */
/* Init                      */
/* ========================= */

static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        printk(KERN_ALERT "[monitor] Failed to register device\n");
        return major;
    }

    printk(KERN_INFO "[monitor] Device registered with major %d\n", major);

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
/* Exit                      */
/* ========================= */

static void __exit monitor_exit(void)
{
    struct pid_entry *entry, *tmp;

    if (monitor_thread)
        kthread_stop(monitor_thread);

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

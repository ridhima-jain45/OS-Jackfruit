#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DEVICE_NAME "container_monitor"

/* ioctl commands */
#define MONITOR_REGISTER   _IOW('a', 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW('a', 2, struct monitor_request)

/* shared structure */
struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[32];
};

#endif

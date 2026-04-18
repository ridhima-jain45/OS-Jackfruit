/*
 * monitor_ioctl.h - Shared ioctl definitions between user space and kernel space.
 *
 * This header is included by both engine.c (user space) and monitor.c (kernel).
 * Guard the linux/ioctl.h include for user-space builds that use sys/ioctl.h.
 */

#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#  include <linux/ioctl.h>
#  include <linux/types.h>
#else
#  include <sys/ioctl.h>
#  include <sys/types.h>
#endif

#define DEVICE_NAME "container_monitor"

/* ioctl magic number and command codes */
#define MONITOR_IOC_MAGIC  'a'
#define MONITOR_REGISTER   _IOW(MONITOR_IOC_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_IOC_MAGIC, 2, struct monitor_request)

/*
 * Payload sent from user-space supervisor to kernel module for both
 * REGISTER and UNREGISTER operations.
 *
 * Fields:
 *   pid              – host (container) PID as seen from the root PID namespace
 *   soft_limit_bytes – RSS threshold for a warning log (dmesg)
 *   hard_limit_bytes – RSS threshold that triggers SIGKILL
 *   container_id     – human-readable container name (for log messages)
 */
struct monitor_request {
    pid_t         pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char          container_id[32];
};

#endif /* MONITOR_IOCTL_H */

#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define IOCTL_BASE 'M'

#define REGISTER_PID _IOW(IOCTL_BASE, 1, int)
#define UNREGISTER_PID _IOW(IOCTL_BASE, 2, int)

#endif

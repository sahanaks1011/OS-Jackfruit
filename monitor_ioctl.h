#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define IOCTL_REGISTER_PROCESS  _IOW('a','a',int)
#define IOCTL_ALLOCATE_MEMORY   _IOW('a','b',int)
#define IOCTL_TERMINATE_PROCESS _IOW('a','c',int)
#define IOCTL_GET_STATS         _IOR('a','d',int)

#endif
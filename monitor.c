#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>

#include "monitor_ioctl.h"

#define MAX_CONTAINERS 64

MODULE_LICENSE("GPL");

static int major;
static pid_t pids[MAX_CONTAINERS];
static int count = 0;

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    pid_t pid;

    if (copy_from_user(&pid, (pid_t *)arg, sizeof(pid)))
        return -EFAULT;

    switch (cmd)
    {
        case REGISTER_CONTAINER:
            if (count < MAX_CONTAINERS) {
                pids[count++] = pid;
                printk(KERN_INFO "Monitor: Registered PID %d\n", pid);
            }
            break;

        case UNREGISTER_CONTAINER:
            for (int i = 0; i < count; i++) {
                if (pids[i] == pid) {
                    pids[i] = pids[count - 1];
                    count--;
                    printk(KERN_INFO "Monitor: Unregistered PID %d\n", pid);
                    break;
                }
            }
            break;

        default:
            return -EINVAL;
    }

    return 0;
}

static struct file_operations fops = {
    .unlocked_ioctl = device_ioctl,
};

static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    printk(KERN_INFO "Monitor loaded\n");
    return 0;
}

static void __exit monitor_exit(void)
{
    unregister_chrdev(major, DEVICE_NAME);
    printk(KERN_INFO "Monitor unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
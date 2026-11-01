// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include "trace_event.h"

static unsigned long long base;
module_param(base, ullong, 0444);
static void __iomem *port;
static DEFINE_SPINLOCK(trace_lock);

static ssize_t trace_write(struct file *file, const char __user *buffer,
                          size_t bytes, loff_t *position)
{
    struct trace_event event;
    const unsigned long long *words = (const unsigned long long *)&event;
    unsigned long flags;
    unsigned i;

    if (bytes != sizeof(event))
        return -EINVAL;
    if (copy_from_user(&event, buffer, sizeof(event)))
        return -EFAULT;
    if ((event.event != TRACE_BEGIN && event.event != TRACE_END) ||
        event.depth < 1 || event.depth > 4)
        return -EINVAL;
    for (i = 0; i < event.depth; ++i)
        if (event.path[i] > S64_MAX)
            return -EINVAL;

    // One event stays contiguous even when several processes submit traces.
    spin_lock_irqsave(&trace_lock, flags);
    for (i = 0; i < TRACE_WORDS; ++i)
        writeq(words[i], port + i * sizeof(*words));
    spin_unlock_irqrestore(&trace_lock, flags);
    return bytes;
}

static const struct file_operations trace_operations = {
    .owner = THIS_MODULE,
    .write = trace_write,
    .llseek = no_llseek,
};
static struct miscdevice trace_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "trace",
    .fops = &trace_operations,
    .mode = 0666,
};

static int __init trace_init(void)
{
    int status;
    if (!base)
        return -EINVAL;
    port = ioremap(base, sizeof(struct trace_event));
    if (!port)
        return -ENOMEM;
    status = misc_register(&trace_device);
    if (status)
        iounmap(port);
    return status;
}

static void __exit trace_exit(void)
{
    misc_deregister(&trace_device);
    iounmap(port);
}
module_init(trace_init);
module_exit(trace_exit);
MODULE_LICENSE("GPL");

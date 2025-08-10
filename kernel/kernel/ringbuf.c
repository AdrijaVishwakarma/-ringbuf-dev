/*
 * ringbuf.c - dynamic circular queue char device with blocking POP via IOCTL
 *
 * Place this file in ringbuf-dev/kernel/
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/sched.h> /* for TASK_INTERRUPTIBLE */
#include "common.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Dynamic circular queue char device (ringbufdev)");

/* Circular queue structure */
struct ringbuf {
    char *buf;            /* kmalloc'd buffer */
    size_t size;          /* capacity */
    size_t head;          /* read index */
    size_t tail;          /* write index */
    size_t count;         /* bytes stored */
    wait_queue_head_t rq; /* readers wait queue */
    struct mutex lock;    /* protect structure */
};

static struct ringbuf rb;

/* char device bookkeeping */
static dev_t devnum;
static struct cdev rb_cdev;
static struct class *rb_class;

/* Helper: init ring buffer (caller should ensure appropriate locking/state) */
static int ringbuf_init(size_t sz)
{
    if (sz == 0)
        return -EINVAL;

    rb.buf = kmalloc(sz, GFP_KERNEL);
    if (!rb.buf)
        return -ENOMEM;

    rb.size = sz;
    rb.head = rb.tail = rb.count = 0;
    init_waitqueue_head(&rb.rq);
    mutex_init(&rb.lock);
    pr_info("ringbuf: allocated buffer of %zu bytes\n", sz);
    return 0;
}

/* Helper: free ring buffer */
static void ringbuf_free(void)
{
    if (rb.buf) {
        kfree(rb.buf);
        rb.buf = NULL;
    }
    rb.size = rb.head = rb.tail = rb.count = 0;
}

/* push bytes into ring (caller must hold mutex) */
static ssize_t ringbuf_push_locked(const char *kdata, size_t len)
{
    size_t i;

    if (len > rb.size - rb.count)
        return -ENOSPC; /* no enough space */

    for (i = 0; i < len; ++i) {
        rb.buf[rb.tail] = kdata[i];
        rb.tail = (rb.tail + 1) % rb.size;
    }
    rb.count += len;
    return (ssize_t)len;
}

/* pop up to len bytes from ring into out (caller must hold mutex) */
static ssize_t ringbuf_pop_locked(char *out, size_t len)
{
    size_t i;
    size_t tocopy = len;

    if (tocopy > rb.count)
        tocopy = rb.count;

    for (i = 0; i < tocopy; ++i) {
        out[i] = rb.buf[rb.head];
        rb.head = (rb.head + 1) % rb.size;
    }
    rb.count -= tocopy;
    return (ssize_t)tocopy;
}

/* IOCTL handler implementing SET_SIZE_OF_QUEUE, PUSH_DATA, POP_DATA */
static long ringbuf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ks; /* size from user */
    struct queue_data ud; /* user struct copy */
    char *kbuf = NULL;
    ssize_t ret = 0;

    switch (cmd) {
    case SET_SIZE_OF_QUEUE:
        if (copy_from_user(&ks, (int __user *)arg, sizeof(int)))
            return -EFAULT;
        if (ks <= 0)
            return -EINVAL;

        /* reinitialize buffer */
        mutex_lock(&rb.lock);
        ringbuf_free();
        ret = ringbuf_init((size_t)ks);
        mutex_unlock(&rb.lock);
        return ret;

    case PUSH_DATA:
        /* get struct with length + user pointer */
        if (copy_from_user(&ud, (struct queue_data __user *)arg, sizeof(ud)))
            return -EFAULT;
        if (ud.length <= 0)
            return -EINVAL;

        kbuf = kmalloc(ud.length, GFP_KERNEL);
        if (!kbuf)
            return -ENOMEM;

        if (copy_from_user(kbuf, ud.data, ud.length)) {
            kfree(kbuf);
            return -EFAULT;
        }

        mutex_lock(&rb.lock);
        ret = ringbuf_push_locked(kbuf, (size_t)ud.length);
        mutex_unlock(&rb.lock);

        kfree(kbuf);

        if (ret > 0) {
            /* wake any blocked POP callers */
            wake_up_interruptible(&rb.rq);
            return ret;
        }
        return ret; /* may be -ENOSPC */

    case POP_DATA:
        /* copy the user struct to get pointer + requested length */
        if (copy_from_user(&ud, (struct queue_data __user *)arg, sizeof(ud)))
            return -EFAULT;
        if (ud.length <= 0)
            return -EINVAL;

        kbuf = kmalloc(ud.length, GFP_KERNEL);
        if (!kbuf)
            return -ENOMEM;

        /* Block until data available (or signal interrupts) */
        for (;;) {
            mutex_lock(&rb.lock);
            if (rb.count > 0) {
                /* data available, pop now */
                ret = ringbuf_pop_locked(kbuf, (size_t)ud.length);
                mutex_unlock(&rb.lock);
                break;
            }
            mutex_unlock(&rb.lock);

            /* Wait until someone pushes data or signal */
            if (wait_event_interruptible(rb.rq, rb.count > 0)) {
                /* interrupted by signal */
                kfree(kbuf);
                return -ERESTARTSYS;
            }
            /* loop to try again */
        }

        /* copy popped bytes back to user buffer */
        if (ret > 0) {
            if (copy_to_user(ud.data, kbuf, ret)) {
                kfree(kbuf);
                return -EFAULT;
            }
            /* update length field in user struct to actual bytes copied */
            if (put_user((int)ret, &((struct queue_data __user *)arg)->length)) {
                kfree(kbuf);
                return -EFAULT;
            }
        }

        kfree(kbuf);
        return ret;

    default:
        return -EINVAL;
    }
}

/* file ops: open/release minimal */
static int ringbuf_open(struct inode *inode, struct file *file)
{
    return 0;
}
static int ringbuf_release(struct inode *inode, struct file *file)
{
    return 0;
}

/* fops struct */
static const struct file_operations ringbuf_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = ringbuf_ioctl,
    .open = ringbuf_open,
    .release = ringbuf_release,
};

/* module init/exit */
static int __init ringbuf_init_module(void)
{
    int ret;

    ret = alloc_chrdev_region(&devnum, 0, 1, DEVICE_NAME);
    if (ret) {
        pr_err("ringbuf: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&rb_cdev, &ringbuf_fops);
    ret = cdev_add(&rb_cdev, devnum, 1);
    if (ret) {
        pr_err("ringbuf: cdev_add failed: %d\n", ret);
        unregister_chrdev_region(devnum, 1);
        return ret;
    }

    rb_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(rb_class)) {
        pr_err("ringbuf: class_create failed\n");
        cdev_del(&rb_cdev);
        unregister_chrdev_region(devnum, 1);
        return PTR_ERR(rb_class);
    }

    if (IS_ERR(device_create(rb_class, NULL, devnum, NULL, DEVICE_NAME))) {
        pr_err("ringbuf: device_create failed\n");
        class_destroy(rb_class);
        cdev_del(&rb_cdev);
        unregister_chrdev_region(devnum, 1);
        return -ENOMEM;
    }

    pr_info("ringbuf: driver loaded, /dev/%s created\n", DEVICE_NAME);
    return 0;
}

static void __exit ringbuf_cleanup_module(void)
{
    ringbuf_free();
    device_destroy(rb_class, devnum);
    class_destroy(rb_class);
    cdev_del(&rb_cdev);
    unregister_chrdev_region(devnum, 1);
    pr_info("ringbuf: driver unloaded\n");
}

module_init(ringbuf_init_module);
module_exit(ringbuf_cleanup_module);

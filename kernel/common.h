#ifndef RINGBUF_COMMON_H
#define RINGBUF_COMMON_H

#include <linux/ioctl.h>

#define DEVICE_NAME "ringbufdev"

// IOCTL command definitions
#define SET_SIZE_OF_QUEUE _IOW('a', 'a', int *)
#define PUSH_DATA         _IOW('a', 'b', struct queue_data *)
#define POP_DATA          _IOR('a', 'c', struct queue_data *)

// Structure for data exchange between user and kernel
struct queue_data {
    int length;
    char *data; // User-space pointer, will be handled with copy_from_user / copy_to_user
};

#endif // RINGBUF_COMMON_H

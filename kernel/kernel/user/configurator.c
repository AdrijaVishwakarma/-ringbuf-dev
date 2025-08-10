#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "../kernel/common.h"

int main(void) {
    int fd = open("/dev/" DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    int size = 100;
    if (ioctl(fd, SET_SIZE_OF_QUEUE, &size) == -1) {
        perror("ioctl SET_SIZE_OF_QUEUE");
    } else {
        printf("Queue size set to %d\n", size);
    }

    close(fd);
    return 0;
}

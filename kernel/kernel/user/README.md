# ringbuf-dev

A Linux kernel character device implementing a **dynamic circular queue** with **IOCTL-based control** and **blocking reads**.

## Features
- Dynamic queue size allocation via `SET_SIZE_OF_QUEUE` IOCTL
- Push arbitrary data into queue via `PUSH_DATA` IOCTL
- Pop data from queue via `POP_DATA` IOCTL
- Blocking behavior: `POP_DATA` waits if the queue is empty until another process pushes data
- Shared `common.h` header for both kernel & user space

---

## Repository Structure

#ifndef AURORA_INPUT_DEV_H
#define AURORA_INPUT_DEV_H
#include "types.h"

/* /dev/input (0.7): see abi/aurora_abi.h for the record layout. */
struct file;
struct process;

void input_dev_wake(void); /* IRQ context: new bytes queued */
struct file *input_dev_open(int *err);
void input_dev_close(struct file *f);
long input_dev_read(struct process *p, struct file *f, uint64_t ubuf, uint64_t len);
long input_dev_ioctl(struct process *p, struct file *f, uint64_t req, uint64_t arg);
int input_dev_poll(struct file *f, int events);

#endif

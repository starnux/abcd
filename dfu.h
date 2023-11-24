#ifndef __DFU_H__
#define __DFU_H__

#include "device.h"

struct boot_ops;

void *dfu_open(struct device *device, struct boot_ops *ops, char *options);
void dfu_close(struct device *device, void *boot_data);
int dfu_boot(void *boot_data, const void *data, size_t len);

#endif

#ifndef __PYAMLBOOT_H__
#define __PYAMLBOOT_H__

#include "device.h"

struct boot_ops;

void *pyamlboot_open(struct device *device, struct boot_ops *ops, char *options);
void pyamlboot_close(struct device *device, void *boot_data);
int pyamlboot_boot(void *boot_data, const void *data, size_t len);

#endif

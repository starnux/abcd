/*
 * Copyright (c) 2016-2018, Linaro Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define _GNU_SOURCE
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>

#include <sys/ioctl.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "abcd-server.h"
#include "dfu.h"
#include "boot.h"

struct dfu {
	const char *usb_path;

	char *dev_path;

	struct boot_ops *ops;

	struct udev_monitor *mon;
	int fd_mon;

	bool disconnected;
};

static int handle_dfu_add(struct dfu *dfu, const char *path)
{
	dfu->dev_path = strdup(path);

	if (dfu->ops && dfu->ops->opened)
		dfu->ops->opened();

	return 0;
}

static int handle_udev_event(int fd, void *data)
{
	struct dfu *dfu = data;
	struct udev_device* dev;
	const char *dev_path;
	const char *action;
	const char *vendor;
	const char *product;

	/* Monitor has been closed */
	if (!dfu->mon)
		return 0;

	dev = udev_monitor_receive_device(dfu->mon);

	action = udev_device_get_action(dev);
	dev_path = udev_device_get_devpath(dev);

	if (!action || !dev_path)
		goto unref_dev;

	if (!strcmp(action, "add")) {
		if (dfu->dev_path)
			goto unref_dev;

		vendor = udev_device_get_property_value(dev, "ID_VENDOR");
		product = udev_device_get_property_value(dev, "ID_MODEL");
		if (!vendor || strcmp(vendor, "U-Boot"))
			goto unref_dev;
		if (!product || strcmp(product, "USB_download_gadget"))
			goto unref_dev;

		/* Udev path (/devices/xxx/xxx/xxxx/x-x.x.x.x) should contain the usb_path */
		if (!strstr(dev_path, dfu->usb_path))
			goto unref_dev;

		handle_dfu_add(dfu, dev_path);
	} else if (!strcmp(action, "remove")) {
		if (!dfu->dev_path || strcmp(dev_path, dfu->dev_path))
			goto unref_dev;

		free(dfu->dev_path);
		dfu->dev_path = NULL;

		if (dfu->ops && dfu->ops->disconnect)
			dfu->ops->disconnect();

		dfu->disconnected = true;
	}

unref_dev:
	udev_device_unref(dev);

	return 0;
}

void dfu_close(struct device *device, void *boot_data)
{
	struct dfu *dfu = boot_data;

	device->do_boot = NULL;	

	watch_del_readfd(dfu->fd_mon);
	udev_monitor_filter_remove(dfu->mon);
	udev_monitor_unref(dfu->mon);

	dfu->mon = NULL;
	dfu->fd_mon = -1;

	if (dfu->dev_path)
		free(dfu->dev_path);
	if (!dfu->disconnected && dfu->ops && dfu->ops->disconnect)
		dfu->ops->disconnect();

	free(dfu);
}

void *dfu_open(struct device *device, struct boot_ops *ops, char *options)
{
	struct dfu *dfu;
	struct udev* udev;
	struct udev_enumerate* udev_enum;
	struct udev_list_entry* first, *item;

	udev = udev_new();
	if (!udev)
		err(1, "udev_new() failed");

	dfu = calloc(1, sizeof(struct dfu));
	if (!dfu)
		err(1, "failed to allocate dfu structure");

	dfu->usb_path = options;
	dfu->ops = ops;

	dfu->mon = udev_monitor_new_from_netlink(udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(dfu->mon, "usb", NULL);
	udev_monitor_enable_receiving(dfu->mon);

	dfu->fd_mon = udev_monitor_get_fd(dfu->mon);

	watch_add_readfd(dfu->fd_mon, handle_udev_event, dfu);

	udev_enum = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(udev_enum, "usb");
	udev_enumerate_add_match_property(udev_enum, "ID_VENDOR", "U-Boot");
	udev_enumerate_add_match_property(udev_enum, "ID_MODEL", "USB_download_gadget");

	udev_enumerate_scan_devices(udev_enum);

	first = udev_enumerate_get_list_entry(udev_enum);
	udev_list_entry_foreach(item, first) {
		const char *path;

		path = udev_list_entry_get_name(item);
		handle_dfu_add(dfu, path);
	}

	udev_enumerate_unref(udev_enum);

	device->do_boot = dfu_boot;

	return dfu;
}

static int dfu_execute(const char *command)
{
	pid_t pid, pid_ret;
	int status;

	pid = fork();
	switch (pid) {
	case 0:
		/* Do not clobber stdout with program messages or abcd will become confused */
		dup2(2, 1);
		exit(system(command));
	case -1:
		return -1;
	default:
		break;
	}

	pid_ret = waitpid(pid, &status, 0);
	if (pid_ret < 0)
		return pid_ret;

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		errno = -EINTR;
	else
		errno = -EIO;

	return -1;
}

int dfu_boot(void *boot_data, const void *data, size_t len)
{
	struct dfu *dfu = boot_data;
	char *cmd = NULL;
	char *boot_file;
	int ret;
	int fd;

	boot_file = strdup("/tmp/dfu-XXXXXX");
	fd = mkstemp(boot_file);
	if (fd < 0) {
		err(1, "Failed to create tmp file");
		return -1;
	}

	write(fd, data, len);
	close(fd);

	asprintf(&cmd, "dfu-util -p %s -a 0 -D %s",
		 dfu->usb_path, boot_file);

	ret = dfu_execute(cmd);

	unlink(boot_file);

	if (ret)
		return ret;

	asprintf(&cmd, "dfu-util -p %s -e", dfu->usb_path);
	ret = dfu_execute(cmd);

	return ret;
}

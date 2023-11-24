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
#include "pyamlboot.h"
#include "boot.h"

struct pyamlboot {
	const char *cmd;

	char *dev_path;

	struct boot_ops *ops;

	struct udev_monitor *mon;
	int fd_mon;

	bool disconnected;
};

static int handle_pyamlboot_add(struct pyamlboot *pyamlboot, const char *path)
{
	pyamlboot->dev_path = strdup(path);

	if (pyamlboot->ops && pyamlboot->ops->opened)
		pyamlboot->ops->opened();

	return 0;
}

static int handle_udev_event(int fd, void *data)
{
	struct pyamlboot *pyamlboot = data;
	struct udev_device* dev;
	const char *dev_path;
	const char *action;
	const char *vendor_id;
	const char *product_id;

	/* Monitor has been closed */
	if (!pyamlboot->mon)
		return 0;

	dev = udev_monitor_receive_device(pyamlboot->mon);

	action = udev_device_get_action(dev);
	dev_path = udev_device_get_devpath(dev);

	if (!action || !dev_path)
		goto unref_dev;

	if (!strcmp(action, "add")) {
		if (pyamlboot->dev_path)
			goto unref_dev;

		vendor_id = udev_device_get_property_value(dev, "ID_VENDOR_ID");
		product_id = udev_device_get_property_value(dev, "ID_MODEL_ID");
		if (!vendor_id || strcmp(vendor_id, "1b8e"))
			goto unref_dev;
		if (!product_id || strcmp(product_id, "c003"))
			goto unref_dev;

		handle_pyamlboot_add(pyamlboot, dev_path);
	} else if (!strcmp(action, "remove")) {
		if (!pyamlboot->dev_path || strcmp(dev_path, pyamlboot->dev_path))
			goto unref_dev;

		free(pyamlboot->dev_path);
		pyamlboot->dev_path = NULL;

		if (pyamlboot->ops && pyamlboot->ops->disconnect)
			pyamlboot->ops->disconnect();

		pyamlboot->disconnected = true;
	}

unref_dev:
	udev_device_unref(dev);

	return 0;
}

void pyamlboot_close(struct device *device, void *boot_data)
{
	struct pyamlboot *pyamlboot = boot_data;

	device->do_boot = NULL;

	watch_del_readfd(pyamlboot->fd_mon);
	udev_monitor_filter_remove(pyamlboot->mon);
	udev_monitor_unref(pyamlboot->mon);

	pyamlboot->mon = NULL;
	pyamlboot->fd_mon = -1;

	if (pyamlboot->dev_path)
		free(pyamlboot->dev_path);

	if (!pyamlboot->disconnected && pyamlboot->ops && pyamlboot->ops->disconnect)
		pyamlboot->ops->disconnect();

	free(pyamlboot);
}

void *pyamlboot_open(struct device *device, struct boot_ops *ops, char *options)
{
	struct pyamlboot *pyamlboot;
	struct udev* udev;
	struct udev_enumerate* udev_enum;
	struct udev_list_entry* first, *item;

	udev = udev_new();
	if (!udev)
		err(1, "udev_new() failed");

	pyamlboot = calloc(1, sizeof(struct pyamlboot));
	if (!pyamlboot)
		err(1, "failed to allocate pyamlboot structure");

	pyamlboot->cmd = options;
	pyamlboot->ops = ops;
	
	pyamlboot->mon = udev_monitor_new_from_netlink(udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(pyamlboot->mon, "usb", NULL);
	udev_monitor_enable_receiving(pyamlboot->mon);

	pyamlboot->fd_mon = udev_monitor_get_fd(pyamlboot->mon);

	watch_add_readfd(pyamlboot->fd_mon, handle_udev_event, pyamlboot);

	udev_enum = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(udev_enum, "usb");
	udev_enumerate_add_match_property(udev_enum, "ID_VENDOR_ID", "1b8e");
	udev_enumerate_add_match_property(udev_enum, "ID_MODEL_ID", "c003");
	
	udev_enumerate_scan_devices(udev_enum);

	first = udev_enumerate_get_list_entry(udev_enum);
	udev_list_entry_foreach(item, first) {
		const char *path;

		path = udev_list_entry_get_name(item);
		handle_pyamlboot_add(pyamlboot, path);
	}

	udev_enumerate_unref(udev_enum);

	device->do_boot = pyamlboot_boot;

	return pyamlboot;
}

static int pyamlboot_execute(const char *command)
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

int pyamlboot_boot(void *boot_data, const void *data, size_t len)
{
	struct pyamlboot *pyamlboot = boot_data;
	char *cmd = NULL;
	int ret;

	if (strstr(pyamlboot->cmd, "boot-g12")) {
		char *boot_file;
		int fd;
		
		boot_file = strdup("/tmp/pyamlboot-XXXXXX");
		fd = mkstemp(boot_file);
		if (fd < 0) {
			err(1, "Failed to create tmp file");
			return -1;
		}

		write(fd, data, len);
		close(fd);

		asprintf(&cmd, pyamlboot->cmd, boot_file);
		ret = pyamlboot_execute(cmd);

		unlink(boot_file);
	} else {
		char *boot_dir, *boot_file_tpl, *boot_file_bl2;
		int fd;

		if (len < 49152) {
			err(1, "Invalid file length for pre-g12 boot");
			return -1;
		}

		boot_dir = strdup("/tmp/pyamlboot-XXXXXX");
		if (!mkdtemp(boot_dir)) {
			err(1, "Failed to create tmp dir");
			return -1;
		}
			
		asprintf(&boot_file_bl2, "%s/u-boot.bin.usb.bl2", boot_dir);
		asprintf(&boot_file_tpl, "%s/u-boot.bin.usb.tpl", boot_dir);

		fd = open(boot_file_bl2, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd < 0) {
			err(1, "Failed to create bl2 tmp file");
			return -1;
		}
		write(fd, data, 49152);
		close(fd);

		fd = open(boot_file_tpl, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd < 0) {
			err(1, "Failed to create tpl tmp file");
			return -1;
		}
		write(fd, &((const char *)data)[49152], len - 49152);
		close(fd);

		asprintf(&cmd, pyamlboot->cmd, boot_dir);
		ret = pyamlboot_execute(cmd);

		unlink(boot_file_bl2);
		unlink(boot_file_tpl);
		rmdir(boot_dir);
	}

	return ret;
}

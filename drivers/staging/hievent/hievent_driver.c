// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "hievent_driver.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/rtc.h>
#include <linux/uio.h>
#include <linux/list.h>
#include <linux/wait.h>

#ifndef HIEVENTDEV_MAJOR
#define HIEVENTDEV_MAJOR 241
#endif

#ifndef HIEVENT_NR_DEVS
#define HIEVENT_NR_DEVS 2
#endif

static int hievent_major = HIEVENTDEV_MAJOR;

static struct cdev hievent_cdev;

#define HIEVENT_BUFFER ((size_t)1024)
#define HIEVENT_DRIVER "/dev/hwlog_exception"

struct hievent_entry {
	unsigned short len;
	unsigned short header_size;
	int pid;
	int tid;
	int sec;
	int nsec;
	char msg[0];
};

struct hievent_char_device {
	int flag;
	struct mutex mtx; /* lock to protect read/write buffer */
	unsigned char *buffer;
	wait_queue_head_t wq;
	size_t write_offset;
	size_t head_offset;
	size_t size;
	size_t count;
} hievent_dev;

static inline unsigned char *hievent_buffer_head(void)
{
	if (hievent_dev.head_offset > HIEVENT_BUFFER)
		hievent_dev.head_offset =
			hievent_dev.head_offset % HIEVENT_BUFFER;

	return hievent_dev.buffer + hievent_dev.head_offset;
}

static void hievent_buffer_inc(size_t sz)
{
	if (hievent_dev.size + sz <= HIEVENT_BUFFER) {
		hievent_dev.size += sz;
		hievent_dev.write_offset += sz;
		hievent_dev.write_offset %= HIEVENT_BUFFER;
		hievent_dev.count++;
	}
}

static void hievent_buffer_dec(size_t sz)
{
	if (hievent_dev.size >= sz) {
		hievent_dev.size -= sz;
		hievent_dev.head_offset += sz;
		hievent_dev.head_offset %= HIEVENT_BUFFER;
		hievent_dev.count--;
	}
}

static int hievent_read_ring_buffer(unsigned char __user *buffer,
				    size_t buf_len)
{
	size_t retval;
	size_t buf_left = HIEVENT_BUFFER - hievent_dev.head_offset;

	if (buf_left > buf_len) {
		retval = copy_to_user(buffer, hievent_buffer_head(), buf_len);
	} else {
		size_t mem_len = (buf_len > buf_left) ? buf_left : buf_len;

		retval = copy_to_user(buffer, hievent_buffer_head(), mem_len);
		if (retval < 0)
			return retval;

		retval = copy_to_user(buffer + buf_left, hievent_dev.buffer,
				      buf_len - buf_left);
	}
	return retval;
}

static int hievent_read_ring_head_buffer(unsigned char * const buffer,
					 size_t buf_len)
{
	size_t buf_left = HIEVENT_BUFFER - hievent_dev.head_offset;

	if (buf_left > buf_len) {
		memcpy(buffer, hievent_buffer_head(), buf_len);
	} else {
		size_t mem_len = (buf_len > buf_left) ? buf_left : buf_len;

		memcpy(buffer, hievent_buffer_head(), mem_len);
		memcpy(buffer + buf_left, hievent_dev.buffer,
		       buf_len - buf_left);
	}
	return 0;
}

static ssize_t hievent_read(struct file *file, char __user *user_buf,
			    size_t count, loff_t *ppos)
{
	size_t retval;
	struct hievent_entry header;

	(void)file;

	wait_event_interruptible(hievent_dev.wq, (hievent_dev.size > 0));

	(void)mutex_lock(&hievent_dev.mtx);

	retval = hievent_read_ring_head_buffer((unsigned char *)&header,
					       sizeof(header));
	if (retval < 0) {
		retval = -EINVAL;
		goto out;
	}

	if (count < header.len + sizeof(header)) {
		retval = -ENOMEM;
		goto out;
	}

	hievent_buffer_dec(sizeof(header));
	retval = copy_to_user((unsigned char *)user_buf,
			      (unsigned char *)&header,
			      min(count, sizeof(header)));
	if (retval < 0) {
		retval = -EINVAL;
		goto out;
	}

	retval = hievent_read_ring_buffer((unsigned char *)(user_buf +
					  sizeof(header)), header.len);
	if (retval < 0) {
		retval = -EINVAL;
		goto out;
	}

	hievent_buffer_dec(header.len);

	retval = header.len + sizeof(header);
out:
	if (retval == -ENOMEM) {
		// clean ring buffer
		hievent_dev.write_offset = 0;
		hievent_dev.head_offset = 0;
		hievent_dev.size = 0;
		hievent_dev.count = 0;
	}
	(void)mutex_unlock(&hievent_dev.mtx);

	return retval;
}

static int hievent_write_ring_head_buffer(const unsigned char *buffer,
					  size_t buf_len)
{
	size_t buf_left = HIEVENT_BUFFER - hievent_dev.write_offset;

	if (buf_len > buf_left) {
		memcpy(hievent_dev.buffer + hievent_dev.write_offset,
		       buffer, buf_left);
		memcpy(hievent_dev.buffer, buffer + buf_left,
		       min(HIEVENT_BUFFER, buf_len - buf_left));
	} else {
		memcpy(hievent_dev.buffer + hievent_dev.write_offset,
		       buffer, min(buf_left, buf_len));
	}

	return 0;
}

static void hievent_head_init(struct hievent_entry * const header, size_t len)
{
#define NANOSEC_PER_MIRCOSEC 1000
	struct timeval now = { 0 };

	do_gettimeofday(&now);

	header->len = (unsigned short)len;
	header->pid = current->pid;
	header->tid = 0;
	header->sec = now.tv_sec;
	header->nsec = now.tv_usec * NANOSEC_PER_MIRCOSEC;
	header->header_size = sizeof(struct hievent_entry);
}

static void hievent_cover_old_log(size_t buf_len)
{
	int retval;
	struct hievent_entry header;
	size_t total_size = buf_len + sizeof(struct hievent_entry);

	while (total_size + hievent_dev.size >= HIEVENT_BUFFER) {
		retval = hievent_read_ring_head_buffer((unsigned char *)&header,
						       sizeof(header));
		if (retval < 0)
			break;

		/* let count decrease twice */
		hievent_buffer_dec(sizeof(header));
		hievent_buffer_dec(header.len);
	}
}

int hievent_write_internal(const char *buffer, size_t buf_len)
{
	struct hievent_entry header;
	int retval;

	if (buf_len < sizeof(int) ||
	    buf_len > HIEVENT_BUFFER - sizeof(struct hievent_entry))
		return -EINVAL;

	(void)mutex_lock(&hievent_dev.mtx);

	hievent_cover_old_log(buf_len);

	hievent_head_init(&header, buf_len);

	retval = hievent_write_ring_head_buffer((unsigned char *)&header,
						sizeof(header));
	if (retval) {
		retval = -EINVAL;
		goto out;
	}
	hievent_buffer_inc(sizeof(header));

	retval = hievent_write_ring_head_buffer((unsigned char *)(buffer),
						header.len);
	if (retval) {
		retval = -EINVAL;
		goto out;
	}

	hievent_buffer_inc(header.len);

	retval = header.len;

out:
	(void)mutex_unlock(&hievent_dev.mtx);
	if (retval > 0)
		wake_up_interruptible(&hievent_dev.wq);

	return retval;
}

static unsigned int hievent_poll(struct file *filep,
				 struct poll_table_struct *fds)
{
	(void)filep;
	(void)fds;

	wait_event_interruptible(hievent_dev.wq, (hievent_dev.size > 0));

	return (POLLOUT | POLLWRNORM);
}

static ssize_t hievent_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	int check_code = 0;
	unsigned char *temp_buffer = NULL;
	const struct iovec *iov = from->iov;
	int retval;
	int buf_len;

	(void)iocb;
	if (from->nr_segs != 3) { /* must contain 3 segments */
		retval = -EINVAL;
		goto out;
	}

	retval = copy_from_user(&check_code, iov[0].iov_base,
				sizeof(check_code));
	if (retval || check_code != CHECK_CODE) {
		retval = -EINVAL;
		goto out;
	}

	/* seg 1 && 2 is head info */
	buf_len = iov[1].iov_len + iov[2].iov_len;
	if (buf_len > HIEVENT_BUFFER - sizeof(struct hievent_entry)) {
		retval = -ENOMEM;
		goto out;
	}

	temp_buffer = kmalloc(buf_len, GFP_KERNEL);
	if (!temp_buffer) {
		retval = -ENOMEM;
		goto out;
	}

	retval = copy_from_user(temp_buffer, iov[1].iov_base, iov[1].iov_len);
	if (retval) {
		retval = -EIO;
		goto free_mem;
	}

	/* 1 2 head info */
	retval = copy_from_user(temp_buffer + iov[1].iov_len, iov[2].iov_base,
				iov[2].iov_len);
	if (retval) {
		retval = -EIO;
		goto free_mem;
	}

	retval = hievent_write_internal(temp_buffer, buf_len);
	if (retval) {
		retval = -EIO;
		goto free_mem;
	}

	retval = buf_len + iov[0].iov_len;

free_mem:
	kfree(temp_buffer);

out:
	return retval;
}

static const struct file_operations hievent_fops = {
	.read = hievent_read,   /* read */
	.poll = hievent_poll,   /* poll */
	.write_iter = hievent_write_iter, /* write_iter */
};

static void hievent_device_init(void)
{
	hievent_dev.buffer = kmalloc(HIEVENT_BUFFER, GFP_KERNEL);
	if (!hievent_dev.buffer)
		return;

	init_waitqueue_head(&hievent_dev.wq);
	mutex_init(&hievent_dev.mtx);
	hievent_dev.write_offset = 0;
	hievent_dev.head_offset = 0;
	hievent_dev.size = 0;
	hievent_dev.count = 0;
}

static int __init hieventdev_init(void)
{
	int result;
	dev_t devno = MKDEV(hievent_major, 0);

	result = register_chrdev_region(devno, 2, "hwlog_exception");
	if (result < 0)
		return result;

	cdev_init(&hievent_cdev, &hievent_fops);
	hievent_cdev.owner = THIS_MODULE;
	hievent_cdev.ops = &hievent_fops;

	cdev_add(&hievent_cdev, MKDEV(hievent_major, 0), HIEVENT_NR_DEVS);

	hievent_device_init();
	return 0;
}

static void __exit hievent_exit_module(void)
{
	cdev_del(&hievent_cdev);
	unregister_chrdev_region(MKDEV(hievent_major, 0), HIEVENT_NR_DEVS);
}

static int __init hievent_init_module(void)
{
	int state;

	state = hieventdev_init();
	return 0;
}

module_init(hievent_init_module);
module_exit(hievent_exit_module);

MODULE_AUTHOR("OHOS");
MODULE_DESCRIPTION("User mode hievent device interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("hievent");

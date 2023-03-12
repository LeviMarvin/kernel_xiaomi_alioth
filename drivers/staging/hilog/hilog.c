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

#ifndef HILOGDEV_MAJOR
#define HILOGDEV_MAJOR 245
#endif

#ifndef HILOG_NR_DEVS
#define HILOG_NR_DEVS 2
#endif

#ifndef MEMDEV_SIZE
#define MEMDEV_SIZE 4096
#endif

static int hilog_major = HILOGDEV_MAJOR;

module_param(hilog_major, int, 0444);

struct cdev g_hilog_cdev;

#define HILOG_BUFFER CONFIG_HILOG_BUFFER_SIZE
#define HILOG_DRIVER "/dev/hilog"

struct hilog_entry {
	unsigned int len;
	unsigned int header_size;
	unsigned int pid : 16;
	unsigned int task_id : 16;
	unsigned int sec;
	unsigned int nsec;
	unsigned int reserved;
	char msg[0];
};

static ssize_t hilog_write(struct file *file,
			   const char __user *user_buf,
			   size_t count, loff_t *ppos);
static ssize_t hilog_read(struct file *file,
			  char __user *user_buf, size_t count, loff_t *ppos);

static const struct file_operations hilog_fops = {
	.read = hilog_read,
	.write = hilog_write,
};

struct hilog_char_device {
	int flag;
	struct mutex mtx; /* lock to protect read/write buffer */
	unsigned char *buffer;
	wait_queue_head_t wq;
	size_t wr_off;
	size_t hdr_off;
	size_t size;
	size_t count;
} hilog_dev;

static inline unsigned char *hilog_buffer_head(void)
{
	return hilog_dev.buffer + hilog_dev.hdr_off;
}

static void hilog_buffer_inc(size_t sz)
{
	if (hilog_dev.size + sz <= HILOG_BUFFER) {
		hilog_dev.size += sz;
		hilog_dev.wr_off += sz;
		hilog_dev.wr_off %= HILOG_BUFFER;
		hilog_dev.count++;
	}
}

static void hilog_buffer_dec(size_t sz)
{
	if (hilog_dev.size >= sz) {
		hilog_dev.size -= sz;
		hilog_dev.hdr_off += sz;
		hilog_dev.hdr_off %= HILOG_BUFFER;
		hilog_dev.count--;
	}
}

static int hilog_read_ring_buff(unsigned char __user *buffer, size_t buf_len)
{
	size_t retval;
	size_t buf_left = HILOG_BUFFER - hilog_dev.hdr_off;

	if (buf_left > buf_len) {
		retval = copy_to_user(buffer, hilog_buffer_head(), buf_len);
	} else {
		size_t mem_len = (buf_len > buf_left) ? buf_left : buf_len;

		retval = copy_to_user(buffer, hilog_buffer_head(), mem_len);
		if (retval < 0)
			return retval;

		retval = copy_to_user(buffer + buf_left, hilog_dev.buffer,
				      buf_len - buf_left);
	}
	return retval;
}

static int hilog_read_ring_head_buffer(unsigned char *buffer, size_t buf_len)
{
	size_t buf_left = HILOG_BUFFER - hilog_dev.hdr_off;

	if (buf_left > buf_len) {
		memcpy(buffer, hilog_buffer_head(), buf_len);
	} else {
		size_t mem_len = (buf_len > buf_left) ? buf_left : buf_len;

		memcpy(buffer, hilog_buffer_head(), mem_len);
		memcpy(buffer + buf_left, hilog_dev.buffer, buf_len - buf_left);
	}

	return 0;
}

static ssize_t hilog_read(struct file *file,
			  char __user *user_buf, size_t count, loff_t *ppos)
{
	size_t retval;
	struct hilog_entry header;

	(void)file;
	wait_event_interruptible(hilog_dev.wq, (hilog_dev.size > 0));

	(void)mutex_lock(&hilog_dev.mtx);

	retval = hilog_read_ring_head_buffer((unsigned char *)&header,
					     sizeof(header));
	if (retval < 0) {
		retval = -EINVAL;
		goto out;
	}

	if (count < header.len + sizeof(header)) {
		pr_err("buffer too small,buf_len=%d, header.len=%d,%d\n",
		       count, header.len, header.header_size);
		retval = -ENOMEM;
		goto out;
	}

	hilog_buffer_dec(sizeof(header));
	retval = copy_to_user((unsigned char *)user_buf,
			      (unsigned char *)&header,
			      min(count, sizeof(header)));

	if (retval < 0) {
		retval = -EINVAL;
		goto out;
	}

	retval = hilog_read_ring_buff((unsigned char *)
				      (user_buf + sizeof(header)),
				      header.len);
	if (retval < 0) {
		retval = -EINVAL;
		goto out;
	}

	hilog_buffer_dec(header.len);
	retval = header.len + sizeof(header);
out:
	if (retval == -ENOMEM) {
		// clean ring buffer
		hilog_dev.wr_off = 0;
		hilog_dev.hdr_off = 0;
		hilog_dev.size = 0;
		hilog_dev.count = 0;
	}
	(void)mutex_unlock(&hilog_dev.mtx);

	return retval;
}

static int hilog_write_ring_buffer(unsigned char __user *buffer, size_t buf_len)
{
	int retval;
	size_t buf_left = HILOG_BUFFER - hilog_dev.wr_off;

	if (buf_len > buf_left) {
		retval = copy_from_user(hilog_dev.buffer + hilog_dev.wr_off,
					buffer, buf_left);
		if (retval)
			return -1;
		retval = copy_from_user(hilog_dev.buffer, buffer + buf_left,
					min(HILOG_BUFFER, buf_len - buf_left));
	} else {
		retval = copy_from_user(hilog_dev.buffer + hilog_dev.wr_off,
					buffer, min(buf_left, buf_len));
	}

	if (retval < 0)
		return -1;

	return 0;
}

static int hilog_write_ring_head_buffer(unsigned char *buffer, size_t buf_len)
{
	size_t buf_left = HILOG_BUFFER - hilog_dev.wr_off;

	if (buf_len > buf_left) {
		memcpy(hilog_dev.buffer + hilog_dev.wr_off,
		       buffer, buf_left);
		memcpy(hilog_dev.buffer, buffer + buf_left,
		       min(HILOG_BUFFER, buf_len - buf_left));
	} else {
		memcpy(hilog_dev.buffer + hilog_dev.wr_off,
		       buffer, min(buf_left, buf_len));
	}

	return 0;
}

static void hilog_head_init(struct hilog_entry *header, size_t len)
{
#define NANOSEC_PER_MIRCOSEC 1000
	struct timeval now = { 0 };

	do_gettimeofday(&now);
	header->len = len;
	header->pid = current->pid;
	header->task_id = current->pid;
	header->sec = now.tv_sec;
	header->nsec = now.tv_usec * NANOSEC_PER_MIRCOSEC;
	header->header_size = sizeof(struct hilog_entry);
}

static void hilog_cover_old_log(size_t buf_len)
{
	int retval;
	struct hilog_entry header;
	size_t total_size = buf_len + sizeof(struct hilog_entry);
	int drop_log_lines = 0;

	while (total_size + hilog_dev.size >= HILOG_BUFFER) {
		retval = hilog_read_ring_head_buffer((unsigned char *)&header,
						     sizeof(header));
		if (retval < 0)
			break;

		drop_log_lines++;
		hilog_buffer_dec(sizeof(header) + header.len);
	}
	if (drop_log_lines > 0)
		pr_info("hilog ringbuffer full, drop %d line(s) log",
			drop_log_lines);
}

int hilog_write_internal(const char __user *buffer, size_t buf_len)
{
	struct hilog_entry header;
	int retval;

	(void)mutex_lock(&hilog_dev.mtx);
	hilog_cover_old_log(buf_len);
	hilog_head_init(&header, buf_len);

	retval = hilog_write_ring_head_buffer((unsigned char *)&header,
					      sizeof(header));
	if (retval) {
		retval = -ENODATA;
		goto out;
	}
	hilog_buffer_inc(sizeof(header));

	retval = hilog_write_ring_buffer((unsigned char *)(buffer), header.len);
	if (retval) {
		retval = -ENODATA;
		goto out;
	}

	hilog_buffer_inc(header.len);

	retval = header.len;

out:
	(void)mutex_unlock(&hilog_dev.mtx);
	if (retval > 0)
		wake_up_interruptible(&hilog_dev.wq);
	else if (retval < 0)
		pr_err("write fail retval=%d\n", retval);

	return retval;
}

static ssize_t hilog_write(struct file *file,
			   const char __user *user_buf,
			   size_t count, loff_t *ppos)
{
	(void)file;
	if (count + sizeof(struct hilog_entry) > HILOG_BUFFER) {
		pr_err("input too large\n");
		return -ENOMEM;
	}

	return hilog_write_internal(user_buf, count);
}

static void hilog_device_init(void)
{
	hilog_dev.buffer = kmalloc(HILOG_BUFFER, GFP_KERNEL);
	if (!hilog_dev.buffer)
		return;

	init_waitqueue_head(&hilog_dev.wq);
	mutex_init(&hilog_dev.mtx);
	hilog_dev.wr_off = 0;
	hilog_dev.hdr_off = 0;
	hilog_dev.size = 0;
	hilog_dev.count = 0;
}

static int __init hilogdev_init(void)
{
	int result;
	dev_t devno = MKDEV(hilog_major, 0);

	result = register_chrdev_region(devno, 2, "hilog");
	if (result < 0) {
		pr_emerg("\t register hilog error %d\n", result);
		return result;
	}

	cdev_init(&g_hilog_cdev, &hilog_fops);
	g_hilog_cdev.owner = THIS_MODULE;
	g_hilog_cdev.ops = &hilog_fops;

	cdev_add(&g_hilog_cdev, MKDEV(hilog_major, 0), HILOG_NR_DEVS);

	hilog_device_init();
	return 0;
}

static void __exit hilog_exit_module(void)
{
	cdev_del(&g_hilog_cdev);
	unregister_chrdev_region(MKDEV(hilog_major, 0), HILOG_NR_DEVS);
}

static int __init hilog_init_module(void)
{
	int state = hilogdev_init();

	pr_info("\t hilog_init Start%d\n", state);
	return 0;
}

module_init(hilog_init_module);
module_exit(hilog_exit_module);

MODULE_AUTHOR("OHOS");
MODULE_DESCRIPTION("User mode hilog device interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("hilog");

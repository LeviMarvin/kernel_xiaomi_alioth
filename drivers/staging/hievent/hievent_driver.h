/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef HIEVENT_DRIVER_H
#define HIEVENT_DRIVER_H

#include <linux/types.h>

#define CHECK_CODE 0x7BCDABCD

struct idap_header {
	char level;
	char category;
	char log_type;
	char sn;
};

int hievent_write_internal(const char *buffer, size_t buf_len);

#endif /* HIEVENT_DRIVER_H */

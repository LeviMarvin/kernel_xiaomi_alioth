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

#ifndef HIVIEW_HIEVENT_H
#define HIVIEW_HIEVENT_H

#define MAX_PATH_NUMBER     10

/* hievent struct */
struct hiview_hievent {
	unsigned int eventid;

	long long time;

	/* payload linked list */
	struct hievent_payload *head;

	/* file path needs uploaded */
	char *file_path[MAX_PATH_NUMBER];
};

struct hiview_hievent *hievent_create(unsigned int eventid);
int hievent_put_integer(struct hiview_hievent *event,
			const char *key, long value);
int hievent_put_string(struct hiview_hievent *event,
		       const char *key, const char *value);
int hievent_set_time(struct hiview_hievent *event, long long seconds);
int hievent_add_filepath(struct hiview_hievent *event, const char *path);
int hievent_report(struct hiview_hievent *obj);
void hievent_destroy(struct hiview_hievent *event);

#endif /* HIVIEW_HIEVENT_H */

/*
 * This file is a part of plotnetcfg, a tool to visualize network config.
 * Copyright (C) 2014 Red Hat, Inc. -- Jiri Benc <jbenc@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include "handler.h"
#include "if.h"
#include "utils.h"
#include "netns.h"

#define NETNS_RUN_DIR "/var/run/netns"

static int netns_get_list(struct netns_entry **result, int supported)
{
	struct netns_entry *entry, *ptr;
	struct dirent *de;
	DIR *dir;

	*result = ptr = calloc(sizeof(struct netns_entry), 1);
	if (!supported)
		return 0;
	dir = opendir(NETNS_RUN_DIR);
	if (!dir)
		return 0;

	while ((de = readdir(dir)) != NULL) {
		if (!strcmp(de->d_name, ".") ||
		    !strcmp(de->d_name, ".."))
			continue;
		entry = calloc(sizeof(struct netns_entry), 1);
		if (!entry)
			return ENOMEM;
		entry->name = strdup(de->d_name);
		if (!entry->name)
			return ENOMEM;
		ptr->next = entry;
		ptr = entry;
	}
	closedir(dir);

	return 0;
}

int netns_list(struct netns_entry **result, int supported)
{
	struct netns_entry *entry;
	int err;

	if ((err = netns_get_list(result, supported)))
		return err;
	for (entry = *result; entry; entry = entry->next) {
		if (entry->name)
			if ((err = netns_switch(entry)))
				return err;
		if ((err = if_list(&entry->ifaces, entry)))
			return err;
	}
	if ((err = global_handler_post(*result)))
		return err;
	if ((err = handler_post(*result)))
		return err;
	return 0;
}

static int do_netns_switch(const char *path)
{
	int netns;

	netns = open(path, O_RDONLY);
	if (netns < 0)
		return errno;
	if (syscall(__NR_setns, netns, CLONE_NEWNET) < 0)
		return errno;
	close(netns);
	return 0;
}

int netns_switch(struct netns_entry *dest)
{
	char net_path[PATH_MAX];

	snprintf(net_path, sizeof(net_path), "%s/%s", NETNS_RUN_DIR, dest->name);
	return do_netns_switch(net_path);
}

int netns_switch_root(void)
{
	int res;

	res = do_netns_switch("/proc/1/ns/net");
	if (res == ENOENT)
		return -1;
	return res;
}

static void netns_list_destruct(struct netns_entry *entry)
{
	if_list_free(entry->ifaces);
	free(entry->name);
}

void netns_list_free(struct netns_entry *list)
{
	list_free(list, (destruct_f)netns_list_destruct);
}

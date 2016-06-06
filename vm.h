/*
 * This file is a part of plotnetcfg, a tool to visualize network config.
 * Copyright (C) 2016 Red Hat, Inc. -- Ondrej Hlavaty <ohlavaty@redhat.com>
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

#ifndef _VM_H
#define _VM_H

#include "addr.h"
#include "label.h"

#include <stdlib.h>

struct netns_entry;

struct vm {
	struct node n;

	struct netns_entry *ns;		/* Home netns where to render this vm */
	char *name;
	char *driver;
	struct list properties;
	struct list rev_vm;		/* Reverse mapping to interfaces */
};

inline static struct vm *vm_new()
{
	struct vm *vm = calloc(1, sizeof(struct vm));

	list_init(&vm->properties);
	list_init(&vm->rev_vm);

	return vm;
}

inline static void vm_free(struct vm *vm)
{
	if (vm->name)
		free(vm->name);
	if (vm->driver)
		free(vm->driver);
	free(vm);
}

#endif

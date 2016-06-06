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

#include <expat.h>
#include <libvirt/libvirt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../handler.h"
#include "../if.h"
#include "../master.h"
#include "../match.h"
#include "../netns.h"
#include "../vm.h"
#include "libvirt.h"

struct parser_info
{
	int state;
	char *buf;
	char *target, *vm_name;
	int buflen, len;
	struct if_entry *iface;
	struct vm *vm;
	struct list *netns_list;
	struct netns_entry *root;
	int err;
	XML_Parser parser;
};

#define TAG_ERR		-1
#define TAG_ROOT	0
#define TAG_DOMAIN	1
#define TAG_NAME	2
#define TAG_DEVICES	3
#define TAG_INTERFACE	4

static const char *tag_names[] = {
	" (outside document) ",
	"domain",
	"name",
	"devices",
	"interface"
};

#define on_tag(name, tag) if (!strcmp(tag_names[(tag)], (name)))
#define state_transition(name, tag, state) do { on_tag(name, tag) { (state) = (tag); }} while (0)

static void abort_parsing(struct parser_info *data, int err)
{
	data->err = err;
	XML_StopParser(data->parser, XML_FALSE);
}

static void xml_element_start(void *userData, const XML_Char *name, const XML_Char **atts)
{
	struct parser_info *data = userData;

	switch (data->state) {
		case 0:
			on_tag(name, TAG_DOMAIN) {
				data->state = TAG_DOMAIN;
				data->vm = vm_new();
				data->vm->driver = strdup("libvirt");
			}
			break;

		case TAG_DOMAIN:
			state_transition(name, TAG_NAME, data->state);
			state_transition(name, TAG_DEVICES, data->state);
			break;

		case TAG_DEVICES:
			state_transition(name, TAG_INTERFACE, data->state);
			break;

		case TAG_INTERFACE:
			if (!strcmp(name, "target"))
				for (; *atts; atts += 2)
					if (!strcmp(atts[0], "dev"))
						data->target = strdup(atts[1]);
			break;

		default:
			break;
	}
}

static int match_by_name(struct if_entry *iface, void *data)
{
	if (iface->vm
	    || strcmp(iface->if_name, data))
		return 0;

	return 1;
}

static int match_vm_iface(struct vm *vm, struct list *netns_list, char *if_name)
{
	struct netns_entry *root = list_head(*netns_list);
	struct match_desc match;
	int err;

	match_init(&match);
	match.mode = MM_FIRST;
	match.netns_list = netns_list;
	if ((err = match_if(&match, match_by_name, if_name)))
		return err;
	if ((err = vm_set(vm, match_found(match))))
		return err;
	if (match_ambiguous(match))
		label_add(&root->warnings, "failed to match interface %s to virtual machine reliably", if_name);

	return 0;
}

static void xml_element_end(void *userData, const XML_Char *name)
{
	struct parser_info *data = userData;
	int err;

	switch (data->state) {
		case TAG_DOMAIN:
			on_tag(name, TAG_DOMAIN) {
				data->state = TAG_ROOT;
				if (!data->vm->ns) {
					data->vm->ns = data->root;
					list_append(&data->root->vms, node(data->vm));
				}
				data->vm = NULL;
			}
			break;

		case TAG_NAME:
			on_tag(name, TAG_NAME) {
				data->state = TAG_DOMAIN;
				data->vm->name = data->buf;
				data->buf = NULL;
				data->buflen = data->len = 0;
			}
			break;

		case TAG_DEVICES:
			on_tag(name, TAG_DEVICES) {
				data->state = TAG_DOMAIN;
			}
			break;

		case TAG_INTERFACE:
			on_tag(name, TAG_INTERFACE) {
				data->state = TAG_DEVICES;
				if (data->target) {
					if ((err = match_vm_iface(data->vm, data->netns_list, data->target)))
						abort_parsing(data, err);
					free(data->target);
				}
			}
			break;
	}
}

static void xml_characters(void *userData, const XML_Char *s, int len)
{
	struct parser_info *data = userData;
	int nbuflen = data->buflen;

	if (data->state != TAG_NAME)
		return;

	if (data->buflen == 0)
		nbuflen = len + 1;

	while (nbuflen - data->len - len < 1)
		nbuflen *= 2;

	if (nbuflen != data->buflen) {
		data->buf = realloc(data->buf, nbuflen);
		data->buflen = nbuflen;
	}

	strncpy(data->buf + data->len, s, len);
	data->len += len;
	data->buf[data->len] = '\0';
}

int parse_domain(struct list *netns_list, char *xml)
{
	XML_Parser p;
	struct parser_info *data;

	p = XML_ParserCreate("US-ASCII");
	XML_SetElementHandler(p, xml_element_start, xml_element_end);
	XML_SetCharacterDataHandler(p, xml_characters);

	data = alloca(sizeof(struct parser_info));
	memset(data, 0, sizeof(struct parser_info));
	data->netns_list = netns_list;
	data->root = list_head(*netns_list);
	data->parser = p;
	XML_SetUserData(p, (void *) data);

	if (XML_STATUS_OK != XML_Parse(p, xml, strlen(xml), 1))
		label_add(&data->root->warnings, "libvirt handler failed: %s", XML_ErrorString(XML_GetErrorCode(p)));

	XML_ParserFree(p);
	return data->err;
}

static int libvirt_global_post(struct list *netns_list)
{
	char *xml;
	int count, i, err = 0;
	int *activeDomains;
	virConnectPtr conn;
	virDomainPtr dom;

	/* Connect automagically to some driver - should be done better */
	conn = virConnectOpen("remote:///system");
	if (conn == NULL)
		return 0;

	count = virConnectNumOfDomains(conn);
	activeDomains = malloc(sizeof(int) * count);
	count = virConnectListDomains(conn, activeDomains, count);

	for (i = 0 ; i < count ; i++) {
		dom = virDomainLookupByID(conn, activeDomains[i]);
		xml = virDomainGetXMLDesc(dom, 0);
		err = parse_domain(netns_list, xml);
		free(xml);
		if (err) break;
	}
	free(activeDomains);

	virConnectClose(conn);
	return err;
}

static struct global_handler gh_libvirt = {
	.post = libvirt_global_post,
};

void handler_libvirt_register(void)
{
	global_handler_register(&gh_libvirt);
}

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

#include "openvswitch.h"
#include <errno.h>
#include <jansson.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include "../args.h"
#include "../handler.h"
#include "../if.h"
#include "../label.h"
#include "../list.h"
#include "../master.h"
#include "../match.h"
#include "../netlink.h"
#include "../netns.h"
#include "../tunnel.h"
#include "../utils.h"

#include "../compat.h"

#define OVS_DB_DEFAULT	"/var/run/openvswitch/db.sock";
static char *db;
static unsigned int vport_genl_id;

struct ovs_if {
	struct node n;
	struct ovs_port *port;
	struct if_entry *link;
	char *name;
	char *type;
	/* for tunnels: */
	char *local_ip;
	char *remote_ip;
	char *key;
	/* for patch port: */
	char *peer;
};

struct ovs_port {
	struct node n;
	struct ovs_bridge *bridge;
	struct if_entry *link;
	char *name;
	struct list ifaces;
	int iface_count;
	/* vlan tags: */
	unsigned int tag;
	unsigned int trunks_count;
	unsigned int *trunks;
	/* bonding: */
	char *bond_mode;
};

struct ovs_bridge {
	struct node n;
	char *name;
	struct list ports;
	struct ovs_port *system;
};

static DECLARE_LIST(br_list);

static int is_set(json_t *j)
{
	return (!strcmp(json_string_value(json_array_get(j, 0)), "set"));
}

static int is_map(json_t *j)
{
	return (!strcmp(json_string_value(json_array_get(j, 0)), "map"));
}

static int is_uuid(json_t *j)
{
	return (!strcmp(json_string_value(json_array_get(j, 0)), "uuid"));
}

static int is_empty(json_t *j)
{
	return json_is_array(j) && is_set(j);
}

static void destruct_if(struct ovs_if *iface)
{
	free(iface->name);
	free(iface->type);
	free(iface->local_ip);
	free(iface->remote_ip);
	free(iface->key);
	free(iface->peer);
}

static int find_str_option(char **dest, json_t *jarr, const char *search_name)
{
	unsigned int i;

	for (i = 0; i < json_array_size(jarr); i++) {
		json_t *jkv = json_array_get(jarr, i);
		const char *key = json_string_value(json_array_get(jkv, 0));
		if (!strcmp(key, search_name)) {
			*dest = strdup(json_string_value(json_array_get(jkv, 1)));
			return 0;
		}
	}
	return 1;
}

static int iface_is_tunnel(const struct ovs_if *iface)
{
	return !strcmp(iface->type, "vxlan")
		|| !strcmp(iface->type, "geneve")
		|| !strcmp(iface->type, "gre");
}

static struct ovs_if *parse_iface(json_t *jresult, json_t *uuid)
{
	struct ovs_if *iface;
	json_t *jif, *jarr;

	if (!is_uuid(uuid))
		return NULL;
	jif = json_object_get(jresult, "Interface");
	jif = json_object_get(jif, json_string_value(json_array_get(uuid, 1)));
	jif = json_object_get(jif, "new");

	iface = calloc(1, sizeof(*iface));
	if (!iface)
		return NULL;
	iface->name = strdup(json_string_value(json_object_get(jif, "name")));
	iface->type = strdup(json_string_value(json_object_get(jif, "type")));
	jarr = json_object_get(jif, "options");
	if (is_map(jarr)) {
		jarr = json_array_get(jarr, 1);
		if (iface_is_tunnel(iface)) {
			find_str_option(&iface->local_ip, jarr, "local_ip");
			find_str_option(&iface->remote_ip, jarr, "remote_ip");
			find_str_option(&iface->key, jarr, "key");
		} else if (!strcmp(iface->type, "patch")) {
			find_str_option(&iface->peer, jarr, "peer");
		}
	}
	return iface;
}

static int parse_port_info(struct ovs_port *port, json_t *jport)
{
	json_t *jval, *jarr;
	unsigned int i, cnt;

	jval = json_object_get(jport, "tag");
	if (!is_empty(jval))
		port->tag = json_integer_value(jval);
	jarr = json_object_get(jport, "trunks");
	jarr = json_array_get(jarr, 1);
	cnt = json_array_size(jarr);
	if (cnt > 0) {
		port->trunks = malloc(sizeof(*port->trunks) * cnt);
		if (!port->trunks)
			return ENOMEM;
		port->trunks_count = cnt;
		for (i = 0; i < cnt; i++)
			port->trunks[i] = json_integer_value(json_array_get(jarr, i));
	}

	jval = json_object_get(jport, "bond_mode");
	if (!is_empty(jval))
		port->bond_mode = strdup(json_string_value(jval));
	return 0;
}

static void destruct_port(struct ovs_port *port)
{
	free(port->name);
	free(port->trunks);
	free(port->bond_mode);
	free(port->trunks);
	list_free(&port->ifaces, (destruct_f)destruct_if);
}


static struct ovs_port *parse_port(json_t *jresult, json_t *uuid,
				   struct ovs_bridge *br)
{
	struct ovs_port *port;
	struct ovs_if *iface;
	json_t *jport, *jarr;
	unsigned int i;

	if (!is_uuid(uuid))
		return NULL;
	jport = json_object_get(jresult, "Port");
	jport = json_object_get(jport, json_string_value(json_array_get(uuid, 1)));
	jport = json_object_get(jport, "new");

	port = calloc(1, sizeof(*port));
	if (!port)
		return NULL;
	port->name = strdup(json_string_value(json_object_get(jport, "name")));
	port->bridge = br;
	list_init(&port->ifaces);
	if (parse_port_info(port, jport))
		goto err_port;
	jarr = json_object_get(jport, "interfaces");
	if (is_set(jarr)) {
		jarr = json_array_get(jarr, 1);
		for (i = 0; i < json_array_size(jarr); i++) {
			iface = parse_iface(jresult, json_array_get(jarr, i));
			if (!iface)
				goto err_port;
			iface->port = port;
			list_append(&port->ifaces, node(iface));
			port->iface_count++;
		}
	} else {
		iface = parse_iface(jresult, jarr);
		if (!iface)
			return NULL;
		iface->port = port;
		list_append(&port->ifaces, node(iface));
		port->iface_count = 1;
	}

	if (!strcmp(json_string_value(json_object_get(jport, "name")), br->name))
		br->system = port;

	return port;
err_port:
	destruct_port(port);
	return NULL;
}

static struct ovs_bridge *parse_bridge(json_t *jresult, json_t *uuid)
{
	struct ovs_bridge *br;
	struct ovs_port *port;
	json_t *jbridge, *jarr;
	unsigned int i;

	if (!is_uuid(uuid))
		return NULL;
	jbridge = json_object_get(jresult, "Bridge");
	jbridge = json_object_get(jbridge, json_string_value(json_array_get(uuid, 1)));
	jbridge = json_object_get(jbridge, "new");

	br = calloc(1, sizeof(*br));
	if (!br)
		return NULL;
	br->name = strdup(json_string_value(json_object_get(jbridge, "name")));
	list_init(&br->ports);
	jarr = json_object_get(jbridge, "ports");
	if (is_set(jarr)) {
		jarr = json_array_get(jarr, 1);
		for (i = 0; i < json_array_size(jarr); i++) {
			port = parse_port(jresult, json_array_get(jarr, i), br);
			if (!port)
				return NULL;
			list_append(&br->ports, node(port));
		}
	} else
		if ((port = parse_port(jresult, jarr, br)))
			list_append(&br->ports, node(port));

	if (list_empty(br->ports))
		return NULL;
	return br;
}

static void parse(struct list *br_list, char *answer)
{
	struct ovs_bridge *br;
	json_t *jroot, *jresult, *jovs, *jarr;
	unsigned int i;

	jroot = json_loads(answer, 0, NULL);
	if (!jroot)
		return;
	jresult = json_object_get(jroot, "result");
	if (!jresult)
		return;
	/* TODO: add the rest of error handling */
	jovs = json_object_get(jresult, "Open_vSwitch");
	if (json_object_size(jovs) != 1)
		return;
	jovs = json_object_iter_value(json_object_iter(jovs));
	jovs = json_object_get(jovs, "new");

	jarr = json_object_get(jovs, "bridges");
	if (is_set(jarr)) {
		jarr = json_array_get(jarr, 1);
		for (i = 0; i < json_array_size(jarr); i++) {
			br = parse_bridge(jresult, json_array_get(jarr, i));
			if (!br)
				return;
			list_append(br_list, node(br));
		}
	} else
		if ((br = parse_bridge(jresult, jarr)))
			list_append(br_list, node(br));
	json_decref(jroot);
}


static void add_table(json_t *parmobj, char *table, ...)
{
	va_list ap;
	json_t *tableobj, *cols;
	char *s;

	va_start(ap, table);
	tableobj = json_object();
	cols = json_array();
	while ((s = va_arg(ap, char *)))
		json_array_append_new(cols, json_string(s));
	json_object_set_new(tableobj, "columns", cols);
	json_object_set_new(parmobj, table, tableobj);
	va_end(ap);
}

static char *construct_query(void)
{
	json_t *root, *params, *po;
	char *res;

	root = json_object();
	json_object_set_new(root, "method", json_string("monitor"));
	json_object_set_new(root, "id", json_integer(0));

	params = json_array();
	json_array_append_new(params, json_string("Open_vSwitch"));
	json_array_append_new(params, json_null());
	po = json_object();
	add_table(po, "Open_vSwitch", "bridges", "ovs_version", NULL);
	add_table(po, "Bridge", "name", "ports", NULL);
	add_table(po, "Port", "interfaces", "name", "tag", "trunks", "bond_mode", NULL);
	add_table(po, "Interface", "name", "type", "options", "admin_state", "link_state", NULL);
	json_array_append_new(params, po);
	json_object_set_new(root, "params", params);

	res = json_dumps(root, 0);
	json_decref(root);
	return res;
}

static int connect_ovs(void)
{
	int fd;
	struct sockaddr_un sun;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return fd;
	sun.sun_family = AF_UNIX;
	strncpy(sun.sun_path, db, UNIX_PATH_MAX);
	sun.sun_path[UNIX_PATH_MAX - 1] = '\0';
	connect(fd, (struct sockaddr *)&sun, sizeof(sun.sun_family) + strlen(sun.sun_path) + 1);
	return fd;
}

#define CHUNK	65536
static char *read_all(int fd)
{
	char *buf, *newbuf;
	size_t len = 0;
	ssize_t res;

	buf = malloc(CHUNK);
	if (!buf)
		return NULL;
	while (1) {
		res = read(fd, buf + len, CHUNK);
		if (res < 0) {
			free(buf);
			return NULL;
		}
		len += res;
		if (res < CHUNK) {
			buf[len] = '\0';
			return buf;
		}
		newbuf = realloc(buf, len + CHUNK);
		if (!newbuf) {
			free(buf);
			return NULL;
		}
		buf = newbuf;
	}
}

static int check_vport(struct netns_entry *ns, struct if_entry *entry)
{
	struct nl_handle hnd;
	struct ovs_header oh = { .dp_ifindex = 0 };
	struct nlmsg *req, *resp;
	int err = ENOMEM;

	/* Be paranoid. If anything goes wrong, assume the interace is not
	 * a vport. It's better to present an interface as unconnected to
	 * the bridge when it's in fact connected, than vice versa.
	 */
	if (!vport_genl_id)
		return 0;
	if (netns_switch(ns))
		return 0;
	if (genl_open(&hnd))
		return 0;

	req = genlmsg_new(vport_genl_id, OVS_VPORT_CMD_GET, 0);
	if (!req)
		goto out_hnd;
	if (nlmsg_put(req, &oh, sizeof(oh)) ||
	    nla_put_str(req, OVS_VPORT_ATTR_NAME, entry->if_name))
		goto out_req;
	err = nl_exchange(&hnd, req, &resp);
	if (err)
		goto out_req;
	/* Keep err = 0. We're only interested whether the call succeeds or
	 * not, we don't care about the returned data.
	 */
	nlmsg_free(resp);
out_req:
	nlmsg_free(req);
out_hnd:
	nl_close(&hnd);
	return !err;
}

static int link_iface_search(struct if_entry *entry, void *arg)
{
	struct ovs_if *iface = arg;
	struct ovs_if *master = list_head(iface->port->bridge->system->ifaces);
	int search_for_system = !master->link;
	int weight;

	if (!search_for_system &&
	    entry->master && strcmp(entry->master->if_name, "ovs-system"))
		return 0;
	/* Ignore ifindex reported by ovsdb, as it is guessed by the
	 * interface name anyway and does not work correctly accross netns.
	 * The heuristics below is much more reliable, though obviously far
	 * from good, it fails spectacularly when the netdev interface is
	 * renamed.
	 */
	if (strcmp(iface->name, entry->if_name))
		return 0;
	if (!strcmp(iface->type, "internal") &&
	    strcmp(entry->driver, "openvswitch"))
		return 0;

	/* We've got a match. This still may not mean the interface is
	 * actually connected in the kernel datapath. Newer kernels set
	 * master (at least for netdev interface type) to ovs-system, which
	 * we check above. For older kernels, we need to be more clever.
	 */
	if (!search_for_system && !entry->master &&
	    !check_vport(master->link->ns, entry))
		return 0;

	weight = 1;
	if (!search_for_system) {
		if (master->link->ns == entry->ns)
			weight++;
	} else {
		if (!entry->ns->name)
			weight++;
	}
	return weight;
}

static int link_iface(struct ovs_if *iface, struct list *netns_list, int required)
{
	struct netns_entry *root = list_head(*netns_list);
	struct match_desc match;
	int err;

	if (iface->link)
		return 0;

	match_init(&match);
	match.netns_list = netns_list;

	if ((err = match_if(&match, link_iface_search, iface)))
		return err;
	iface->link = match_found(match);
	if (match_ambiguous(match))
		return label_add(&root->warnings,
				 "Failed to map openvswitch interface %s reliably",
				 iface->name);
	if (required && !iface->link)
		return label_add(&root->warnings,
				 "Failed to map openvswitch interface %s",
				 iface->name);
	return 0;
}

static struct if_entry *create_iface(char *name, char *br_name, struct netns_entry *root)
{
	struct if_entry *entry;

	entry = if_create();
	if (!entry)
		return NULL;

	asprintf(&entry->internal_ns, "ovs:%s", br_name);
	if (!entry->internal_ns)
		goto err_entry;

	entry->if_name = strdup(name);
	if (!entry->if_name)
		goto err_name;

	entry->ns = root;
	entry->flags |= IF_INTERNAL;
	list_append(&root->ifaces, node(entry));
	return entry;

err_name:
	free(entry->if_name);
err_entry:
	free(entry);
	return NULL;
}

static void label_iface(struct ovs_if *iface)
{
	if (iface->type && *iface->type)
		if_add_config(iface->link, "type", "%s", iface->type);
	if (iface->local_ip)
		if_add_config(iface->link, "from", "%s", iface->local_ip);
	if (iface->remote_ip)
		if_add_config(iface->link, "to", "%s", iface->remote_ip);
	if (iface->key)
		if_add_config(iface->link, "key", "%s", iface->key);
}

static void label_port_or_iface(struct ovs_port *port, struct if_entry *link)
{
	if (port->tag) {
		 if (asprintf(&link->edge_label, "tag %u", port->tag) < 0)
			link->edge_label = NULL;
	} else if (port->trunks_count) {
		char *buf, *ptr;
		unsigned int i;

		buf = malloc(16 * port->trunks_count + 7 + 1);
		if (!buf)
			return;
		ptr = buf + sprintf(buf, "trunks %u", port->trunks[0]);
		for (i = 1; i < port->trunks_count; i++)
			ptr += sprintf(ptr, ", %u", port->trunks[i]);
		link->edge_label = buf;
	}
	if (port->bond_mode)
		if_add_config(link, "bond mode", "%s", port->bond_mode);
}

static void link_tunnel(struct ovs_if *iface)
{
	if (!iface->local_ip || !*iface->local_ip)
		return;
	link_set(tunnel_find_str(iface->link->ns, iface->local_ip), iface->link);
	iface->link->flags |= IF_LINK_WEAK;
}

static int link_patch_search(struct if_entry *entry, void *arg)
{
	struct ovs_if *iface = arg;

	if (!iface->peer ||
	    strcmp(iface->peer, entry->if_name) ||
	    !(entry->flags & IF_INTERNAL))
		return 0;
	return 1;
}

static int link_patch(struct ovs_if *iface, struct list *netns_list)
{
	int err;
	struct match_desc match;

	match_init(&match);
	match.netns_list = netns_list;

	if ((err = match_if(&match, link_patch_search, iface)))
		return err;
	if (match_ambiguous(match))
		return if_add_warning(iface->link, "failed to find openvswitch patch port peer reliably");

	if (match_found(match))
		peer_set(iface->link, match_found(match));
	/* Ignore case when the peer is not found, it will be found from the
	 * other side. */
	return 0;
}

static int link_ifaces(struct list *netns_list)
{
	struct netns_entry *root = list_head(*netns_list);
	struct ovs_bridge *br;
	struct ovs_port *port;
	struct ovs_if *iface, *ovs_master;
	struct if_entry *master;
	int err;

	list_for_each(br, br_list) {
		if (!br->system || !br->system->iface_count)
			return label_add(&root->warnings,
					 "Failed to find main interface for openvswitch bridge %s",
					 br->name);
		if (br->system->iface_count > 1)
			return label_add(&root->warnings,
					 "Main port for openvswitch bridge %s appears to have several interfaces",
					 br->name);
		if ((err = link_iface(list_head(br->system->ifaces), netns_list, 1)))
			return err;
		list_for_each(port, br->ports) {
			if (port == br->system)
				continue;
			ovs_master = list_head(br->system->ifaces);
			master = ovs_master->link;
			if (port->iface_count > 1) {
				port->link = create_iface(port->name, port->bridge->name, root);
				if (!port->link)
					return ENOMEM;
				master_set(master, port->link);
				master = port->link;
				label_port_or_iface(port, port->link);
			}
			list_for_each(iface, port->ifaces) {
				if ((err = link_iface(iface, netns_list, 0)))
					return err;
				if (!iface->link) {
					iface->link = create_iface(iface->name,
								   iface->port->bridge->name,
								   root);
					if (!iface->link)
						return ENOMEM;
				}

				/* reconnect to the ovs master */
				master_set(master, iface->link);

				label_iface(iface);
				if (port->iface_count == 1)
					label_port_or_iface(port, iface->link);
				if (iface_is_tunnel(iface))
					link_tunnel(iface);
				else if (!strcmp(iface->type, "patch")) {
					if ((err = link_patch(iface, netns_list)))
						return err;
				}
			}
		}
	}
	return 0;
}

static int ovs_global_post(struct list *netns_list)
{
	char *str;
	int fd, len;
	int err;

	str = construct_query();
	len = strlen(str);
	fd = connect_ovs();
	if (fd < 0)
		return 0;
	if (write(fd, str, len) < len) {
		close(fd);
		return 0;
	}
	free(str);
	str = read_all(fd);
	parse(&br_list, str);
	free(str);
	close(fd);
	if (list_empty(br_list))
		return 0;
	if ((err = link_ifaces(netns_list)))
		return err;
	return 0;
}

static void destruct_bridge(struct ovs_bridge *br)
{
	free(br->name);
	list_free(&br->ports, (destruct_f)destruct_port);
}

static int ovs_global_init(void)
{
	struct nl_handle hnd;
	int err;

	if ((err = genl_open(&hnd))) {
		vport_genl_id = 0;
		return 0; /* intentionally ignored */
	}
	vport_genl_id = genl_family_id(&hnd, OVS_VPORT_FAMILY);
	nl_close(&hnd);
	return 0;
}

static void ovs_global_cleanup(_unused struct list *netns_list)
{
	list_free(&br_list, (destruct_f)destruct_bridge);
}

static struct global_handler gh_ovs = {
	.init = ovs_global_init,
	.post = ovs_global_post,
	.cleanup = ovs_global_cleanup,
};

static struct arg_option options[] = {
	{ .long_name = "ovs-db", .short_name = 'D', .has_arg = 1,
	  .type = ARG_CHAR, .action.char_var = &db,
	  .help = "path to openvswitch database" },
};

void handler_openvswitch_register(void)
{
	db = OVS_DB_DEFAULT;
	arg_register_batch(options, ARRAY_SIZE(options));
	global_handler_register(&gh_ovs);
}

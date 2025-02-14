/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../include/netns_ifaddrs.h"
#include "af_unix.h"
#include "conf.h"
#include "config.h"
#include "file_utils.h"
#include "log.h"
#include "macro.h"
#include "memory_utils.h"
#include "network.h"
#include "nl.h"
#include "raw_syscalls.h"
#include "syscall_wrappers.h"
#include "utils.h"

#ifndef HAVE_STRLCPY
#include "include/strlcpy.h"
#endif

lxc_log_define(network, lxc);

typedef int (*instantiate_cb)(struct lxc_handler *, struct lxc_netdev *);
static const char loop_device[] = "lo";

static int lxc_ip_route_dest(__u16 nlmsg_type, int family, int ifindex, void *dest, unsigned int netmask)
{
	int addrlen, err;
	struct nl_handler nlh;
	struct rtmsg *rt;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	addrlen = family == AF_INET ? sizeof(struct in_addr)
				    : sizeof(struct in6_addr);

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags =
	    NLM_F_ACK | NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
	nlmsg->nlmsghdr->nlmsg_type = nlmsg_type;

	rt = nlmsg_reserve(nlmsg, sizeof(struct rtmsg));
	if (!rt)
		goto out;
	rt->rtm_family = family;
	rt->rtm_table = RT_TABLE_MAIN;
	rt->rtm_scope = RT_SCOPE_LINK;
	rt->rtm_protocol = RTPROT_BOOT;
	rt->rtm_type = RTN_UNICAST;
	rt->rtm_dst_len = netmask;

	err = -EINVAL;
	if (nla_put_buffer(nlmsg, RTA_DST, dest, addrlen))
		goto out;
	if (nla_put_u32(nlmsg, RTA_OIF, ifindex))
		goto out;
	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

static int lxc_ipv4_dest_add(int ifindex, struct in_addr *dest, unsigned int netmask)
{
	return lxc_ip_route_dest(RTM_NEWROUTE, AF_INET, ifindex, dest, netmask);
}

static int lxc_ipv6_dest_add(int ifindex, struct in6_addr *dest, unsigned int netmask)
{
	return lxc_ip_route_dest(RTM_NEWROUTE, AF_INET6, ifindex, dest, netmask);
}

static int lxc_ipv4_dest_del(int ifindex, struct in_addr *dest, unsigned int netmask)
{
	return lxc_ip_route_dest(RTM_DELROUTE, AF_INET, ifindex, dest, netmask);
}

static int lxc_ipv6_dest_del(int ifindex, struct in6_addr *dest, unsigned int netmask)
{
	return lxc_ip_route_dest(RTM_DELROUTE, AF_INET6, ifindex, dest, netmask);
}

static int lxc_setup_ipv4_routes(struct lxc_list *ip, int ifindex)
{
	struct lxc_list *iterator;
	int err;

	lxc_list_for_each(iterator, ip) {
		struct lxc_inetdev *inetdev = iterator->elem;

		err = lxc_ipv4_dest_add(ifindex, &inetdev->addr, inetdev->prefix);
		if (err) {
			SYSERROR("Failed to setup ipv4 route for network device "
			         "with ifindex %d", ifindex);
			return minus_one_set_errno(-err);
		}
	}

	return 0;
}

static int lxc_setup_ipv6_routes(struct lxc_list *ip, int ifindex)
{
	struct lxc_list *iterator;
	int err;

	lxc_list_for_each(iterator, ip) {
		struct lxc_inet6dev *inet6dev = iterator->elem;

		err = lxc_ipv6_dest_add(ifindex, &inet6dev->addr, inet6dev->prefix);
		if (err) {
			SYSERROR("Failed to setup ipv6 route for network device "
			         "with ifindex %d", ifindex);
			return minus_one_set_errno(-err);
		}
	}

	return 0;
}

static int setup_ipv4_addr_routes(struct lxc_list *ip, int ifindex)
{
	struct lxc_list *iterator;
	int err;

	lxc_list_for_each(iterator, ip) {
		struct lxc_inetdev *inetdev = iterator->elem;

		err = lxc_ipv4_dest_add(ifindex, &inetdev->addr, 32);

		if (err)
			return error_log_errno(err,
				"Failed to setup ipv4 address route for network device with eifindex %d",
				ifindex);
	}

	return 0;
}

static int setup_ipv6_addr_routes(struct lxc_list *ip, int ifindex)
{
	struct lxc_list *iterator;
	int err;

	lxc_list_for_each(iterator, ip) {
		struct lxc_inet6dev *inet6dev = iterator->elem;

		err = lxc_ipv6_dest_add(ifindex, &inet6dev->addr, 128);
		if (err)
			return error_log_errno(err,
				"Failed to setup ipv6 address route for network device with eifindex %d",
				ifindex);
	}

	return 0;
}

struct ip_proxy_args {
	const char *ip;
	const char *dev;
};

static int lxc_add_ip_neigh_proxy_exec_wrapper(void *data)
{
	struct ip_proxy_args *args = data;

	execlp("ip", "ip", "neigh", "add", "proxy", args->ip, "dev", args->dev, (char *)NULL);
	return -1;
}

static int lxc_del_ip_neigh_proxy_exec_wrapper(void *data)
{
	struct ip_proxy_args *args = data;

	execlp("ip", "ip", "neigh", "flush", "proxy", args->ip, "dev", args->dev, (char *)NULL);
	return -1;
}

static int lxc_add_ip_neigh_proxy(const char *ip, const char *dev)
{
	int ret;
	char cmd_output[PATH_MAX] = {0};
	struct ip_proxy_args args = {
		.ip = ip,
		.dev = dev,
	};

	ret = run_command(cmd_output, sizeof(cmd_output), lxc_add_ip_neigh_proxy_exec_wrapper, &args);
	if (ret < 0) {
		ERROR("Failed to add ip proxy \"%s\" to dev \"%s\": %s", ip, dev, cmd_output);
		return -1;
	}

	return 0;
}

static int lxc_del_ip_neigh_proxy(const char *ip, const char *dev)
{
	int ret;
	char cmd_output[PATH_MAX] = {0};
	struct ip_proxy_args args = {
		.ip = ip,
		.dev = dev,
	};

	ret = run_command(cmd_output, sizeof(cmd_output), lxc_del_ip_neigh_proxy_exec_wrapper, &args);
	if (ret < 0) {
		ERROR("Failed to delete ip proxy \"%s\" to dev \"%s\": %s", ip, dev, cmd_output);
		return -1;
	}

	return 0;
}

static int lxc_is_ip_forwarding_enabled(const char *ifname, int family)
{
	int ret;
	char path[PATH_MAX];
	char buf[1] = "";

	if (family != AF_INET && family != AF_INET6)
		return minus_one_set_errno(EINVAL);

	ret = snprintf(path, PATH_MAX, "/proc/sys/net/%s/conf/%s/%s",
		       family == AF_INET ? "ipv4" : "ipv6", ifname,
		       "forwarding");
	if (ret < 0 || (size_t)ret >= PATH_MAX)
		return minus_one_set_errno(E2BIG);

	return lxc_read_file_expect(path, buf, 1, "1");
}

static int instantiate_veth(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int bridge_index, err;
	char *veth1, *veth2;
	char veth1buf[IFNAMSIZ], veth2buf[IFNAMSIZ];
	unsigned int mtu = 0;

	if (netdev->priv.veth_attr.pair[0] != '\0') {
		veth1 = netdev->priv.veth_attr.pair;
		if (handler->conf->reboot)
			lxc_netdev_delete_by_name(veth1);
	} else {
		err = snprintf(veth1buf, sizeof(veth1buf), "vethXXXXXX");
		if (err < 0 || (size_t)err >= sizeof(veth1buf))
			return -1;

		veth1 = lxc_mkifname(veth1buf);
		if (!veth1)
			return -1;

		/* store away for deconf */
		memcpy(netdev->priv.veth_attr.veth1, veth1, IFNAMSIZ);
	}

	err = snprintf(veth2buf, sizeof(veth2buf), "vethXXXXXX");
	if (err < 0 || (size_t)err >= sizeof(veth2buf))
		return -1;

	veth2 = lxc_mkifname(veth2buf);
	if (!veth2)
		goto out_delete;

	err = lxc_veth_create(veth1, veth2);
	if (err) {
		errno = -err;
		SYSERROR("Failed to create veth pair \"%s\" and \"%s\"", veth1, veth2);
		goto out_delete;
	}

	strlcpy(netdev->created_name, veth2, IFNAMSIZ);

	/* changing the high byte of the mac address to 0xfe, the bridge interface
	 * will always keep the host's mac address and not take the mac address
	 * of a container */
	err = setup_private_host_hw_addr(veth1);
	if (err) {
		errno = -err;
		SYSERROR("Failed to change mac address of host interface \"%s\"", veth1);
		goto out_delete;
	}

	/* Retrieve ifindex of the host's veth device. */
	netdev->priv.veth_attr.ifindex = if_nametoindex(veth1);
	if (!netdev->priv.veth_attr.ifindex) {
		ERROR("Failed to retrieve ifindex for \"%s\"", veth1);
		goto out_delete;
	}

	/* Note that we're retrieving the container's ifindex in the host's
	 * network namespace because we need it to move the device from the
	 * host's network namespace to the container's network namespace later
	 * on.
	 */
	netdev->ifindex = if_nametoindex(veth2);
	if (!netdev->ifindex) {
		ERROR("Failed to retrieve ifindex for \"%s\"", veth2);
		goto out_delete;
	}

	if (netdev->mtu) {
		if (lxc_safe_uint(netdev->mtu, &mtu) < 0)
			WARN("Failed to parse mtu");
		else
			INFO("Retrieved mtu %d", mtu);
	} else if (netdev->link[0] != '\0') {
		bridge_index = if_nametoindex(netdev->link);
		if (bridge_index) {
			mtu = netdev_get_mtu(bridge_index);
			INFO("Retrieved mtu %d from %s", mtu, netdev->link);
		} else {
			mtu = netdev_get_mtu(netdev->ifindex);
			INFO("Retrieved mtu %d from %s", mtu, veth2);
		}
	}

	if (mtu) {
		err = lxc_netdev_set_mtu(veth1, mtu);
		if (!err)
			err = lxc_netdev_set_mtu(veth2, mtu);

		if (err) {
			errno = -err;
			SYSERROR("Failed to set mtu \"%d\" for veth pair \"%s\" "
			         "and \"%s\"", mtu, veth1, veth2);
			goto out_delete;
		}
	}

	if (netdev->link[0] != '\0' && netdev->priv.veth_attr.mode == VETH_MODE_BRIDGE) {
		err = lxc_bridge_attach(netdev->link, veth1);
		if (err) {
			errno = -err;
			SYSERROR("Failed to attach \"%s\" to bridge \"%s\"",
			         veth1, netdev->link);
			goto out_delete;
		}
		INFO("Attached \"%s\" to bridge \"%s\"", veth1, netdev->link);
	}

	err = lxc_netdev_up(veth1);
	if (err) {
		errno = -err;
		SYSERROR("Failed to set \"%s\" up", veth1);
		goto out_delete;
	}

	/* setup ipv4 routes on the host interface */
	if (lxc_setup_ipv4_routes(&netdev->priv.veth_attr.ipv4_routes, netdev->priv.veth_attr.ifindex)) {
		ERROR("Failed to setup ipv4 routes for network device \"%s\"", veth1);
		goto out_delete;
	}

	/* setup ipv6 routes on the host interface */
	if (lxc_setup_ipv6_routes(&netdev->priv.veth_attr.ipv6_routes, netdev->priv.veth_attr.ifindex)) {
		ERROR("Failed to setup ipv6 routes for network device \"%s\"", veth1);
		goto out_delete;
	}

	if (netdev->priv.veth_attr.mode == VETH_MODE_ROUTER) {
		if (netdev->ipv4_gateway) {
			char bufinet4[INET_ADDRSTRLEN];
			if (!inet_ntop(AF_INET, netdev->ipv4_gateway, bufinet4, sizeof(bufinet4))) {
				error_log_errno(-errno, "Failed to convert gateway ipv4 address on \"%s\"", veth1);
				goto out_delete;
			}

			err = lxc_ip_forwarding_on(veth1, AF_INET);
			if (err) {
				error_log_errno(err, "Failed to activate ipv4 forwarding on \"%s\"", veth1);
				goto out_delete;
			}

			err = lxc_add_ip_neigh_proxy(bufinet4, veth1);
			if (err) {
				error_log_errno(err, "Failed to add gateway ipv4 proxy on \"%s\"", veth1);
				goto out_delete;
			}
		}

		if (netdev->ipv6_gateway) {
			char bufinet6[INET6_ADDRSTRLEN];

			if (!inet_ntop(AF_INET6, netdev->ipv6_gateway, bufinet6, sizeof(bufinet6))) {
				error_log_errno(-errno, "Failed to convert gateway ipv6 address on \"%s\"", veth1);
				goto out_delete;
			}

			/* Check for sysctl net.ipv6.conf.all.forwarding=1
			   Kernel requires this to route any packets for IPv6.
			*/
			err = lxc_is_ip_forwarding_enabled("all", AF_INET6);
			if (err) {
				error_log_errno(err, "Requires sysctl net.ipv6.conf.all.forwarding=1");
				goto out_delete;
			}

			err = lxc_ip_forwarding_on(veth1, AF_INET6);
			if (err) {
				error_log_errno(err, "Failed to activate ipv6 forwarding on \"%s\"", veth1);
				goto out_delete;
			}

			err = lxc_neigh_proxy_on(veth1, AF_INET6);
			if (err) {
				error_log_errno(err, "Failed to activate proxy ndp on \"%s\"", veth1);
				goto out_delete;
			}

			err = lxc_add_ip_neigh_proxy(bufinet6, veth1);
			if (err) {
				error_log_errno(err, "Failed to add gateway ipv6 proxy on \"%s\"", veth1);
				goto out_delete;
			}
		}

		/* setup ipv4 address routes on the host interface */
		err = setup_ipv4_addr_routes(&netdev->ipv4, netdev->priv.veth_attr.ifindex);
		if (err) {
			error_log_errno(err, "Failed to setup ip address routes for network device \"%s\"", veth1);
			goto out_delete;
		}

		/* setup ipv6 address routes on the host interface */
		err = setup_ipv6_addr_routes(&netdev->ipv6, netdev->priv.veth_attr.ifindex);
		if (err) {
			error_log_errno(err, "Failed to setup ip address routes for network device \"%s\"", veth1);
			goto out_delete;
		}
	}

	if (netdev->upscript) {
		char *argv[] = {
		    "veth",
		    netdev->link,
		    veth1,
		    NULL,
		};

		err = run_script_argv(handler->name,
				handler->conf->hooks_version, "net",
				netdev->upscript, "up", argv);
		if (err < 0)
			goto out_delete;
	}

	DEBUG("Instantiated veth \"%s/%s\", index is \"%d\"", veth1, veth2,
	      netdev->ifindex);

	return 0;

out_delete:
	if (netdev->ifindex != 0)
		lxc_netdev_delete_by_name(veth1);
	return -1;
}

static int instantiate_macvlan(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	char peer[IFNAMSIZ];
	int err;
	unsigned int mtu = 0;

	if (netdev->link[0] == '\0') {
		ERROR("No link for macvlan network device specified");
		return -1;
	}

	err = snprintf(peer, sizeof(peer), "mcXXXXXX");
	if (err < 0 || (size_t)err >= sizeof(peer))
		return -1;

	if (!lxc_mkifname(peer))
		return -1;

	err = lxc_macvlan_create(netdev->link, peer,
				 netdev->priv.macvlan_attr.mode);
	if (err) {
		errno = -err;
		SYSERROR("Failed to create macvlan interface \"%s\" on \"%s\"",
		         peer, netdev->link);
		goto on_error;
	}

	strlcpy(netdev->created_name, peer, IFNAMSIZ);

	netdev->ifindex = if_nametoindex(peer);
	if (!netdev->ifindex) {
		ERROR("Failed to retrieve ifindex for \"%s\"", peer);
		goto on_error;
	}

	if (netdev->mtu) {
		err = lxc_safe_uint(netdev->mtu, &mtu);
		if (err < 0) {
			errno = -err;
			SYSERROR("Failed to parse mtu \"%s\" for interface \"%s\"", netdev->mtu, peer);
			goto on_error;
		}

		err = lxc_netdev_set_mtu(peer, mtu);
		if (err < 0) {
			errno = -err;
			SYSERROR("Failed to set mtu \"%s\" for interface \"%s\"", netdev->mtu, peer);
			goto on_error;
		}
	}

	if (netdev->upscript) {
		char *argv[] = {
		    "macvlan",
		    netdev->link,
		    NULL,
		};

		err = run_script_argv(handler->name,
				handler->conf->hooks_version, "net",
				netdev->upscript, "up", argv);
		if (err < 0)
			goto on_error;
	}

	DEBUG("Instantiated macvlan \"%s\" with ifindex is %d and mode %d",
	      peer, netdev->ifindex, netdev->priv.macvlan_attr.mode);

	return 0;

on_error:
	lxc_netdev_delete_by_name(peer);
	return -1;
}

static int lxc_ipvlan_create(const char *master, const char *name, int mode, int isolation)
{
	int err, index, len;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct rtattr *nest, *nest2;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	len = strlen(master);
	if (len == 1 || len >= IFNAMSIZ)
		return minus_one_set_errno(EINVAL);

	len = strlen(name);
	if (len == 1 || len >= IFNAMSIZ)
		return minus_one_set_errno(EINVAL);

	index = if_nametoindex(master);
	if (!index)
		return minus_one_set_errno(EINVAL);

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return minus_one_set_errno(-err);

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi) {
		goto out;
	}
	ifi->ifi_family = AF_UNSPEC;

	err = -EPROTO;
	nest = nla_begin_nested(nlmsg, IFLA_LINKINFO);
	if (!nest)
		goto out;

	if (nla_put_string(nlmsg, IFLA_INFO_KIND, "ipvlan"))
		goto out;

	if (mode) {
		nest2 = nla_begin_nested(nlmsg, IFLA_INFO_DATA);
		if (!nest2)
			goto out;

		if (nla_put_u32(nlmsg, IFLA_IPVLAN_MODE, mode))
			goto out;

		/* if_link.h does not define the isolation flag value for bridge mode so we define it as 0
		 * and only send mode if mode >0 as default mode is bridge anyway according to ipvlan docs.
		 */
		if (isolation > 0) {
			if (nla_put_u16(nlmsg, IFLA_IPVLAN_ISOLATION, isolation))
				goto out;
		}

		nla_end_nested(nlmsg, nest2);
	}

	nla_end_nested(nlmsg, nest);

	if (nla_put_u32(nlmsg, IFLA_LINK, index))
		goto out;

	if (nla_put_string(nlmsg, IFLA_IFNAME, name))
		goto out;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	if (err < 0)
		return minus_one_set_errno(-err);
	return 0;
}

static int instantiate_ipvlan(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	char peer[IFNAMSIZ];
	int err;
	unsigned int mtu = 0;

	if (netdev->link[0] == '\0') {
		ERROR("No link for ipvlan network device specified");
		return -1;
	}

	err = snprintf(peer, sizeof(peer), "ipXXXXXX");
	if (err < 0 || (size_t)err >= sizeof(peer))
		return -1;

	if (!lxc_mkifname(peer))
		return -1;

	err = lxc_ipvlan_create(netdev->link, peer, netdev->priv.ipvlan_attr.mode,
				netdev->priv.ipvlan_attr.isolation);
	if (err) {
		SYSERROR("Failed to create ipvlan interface \"%s\" on \"%s\"",
			 peer, netdev->link);
		goto on_error;
	}

	strlcpy(netdev->created_name, peer, IFNAMSIZ);

	netdev->ifindex = if_nametoindex(peer);
	if (!netdev->ifindex) {
		ERROR("Failed to retrieve ifindex for \"%s\"", peer);
		goto on_error;
	}

	if (netdev->mtu) {
		err = lxc_safe_uint(netdev->mtu, &mtu);
		if (err < 0) {
			errno = -err;
			SYSERROR("Failed to parse mtu \"%s\" for interface \"%s\"",
				 netdev->mtu, peer);
			goto on_error;
		}

		err = lxc_netdev_set_mtu(peer, mtu);
		if (err < 0) {
			errno = -err;
			SYSERROR("Failed to set mtu \"%s\" for interface \"%s\"",
				 netdev->mtu, peer);
			goto on_error;
		}
	}

	if (netdev->upscript) {
		char *argv[] = {
		    "ipvlan",
		    netdev->link,
		    NULL,
		};

		err = run_script_argv(handler->name, handler->conf->hooks_version,
				      "net", netdev->upscript, "up", argv);
		if (err < 0)
			goto on_error;
	}

	DEBUG("Instantiated ipvlan \"%s\" with ifindex is %d and mode %d", peer,
	      netdev->ifindex, netdev->priv.macvlan_attr.mode);

	return 0;

on_error:
	lxc_netdev_delete_by_name(peer);
	return -1;
}

static int instantiate_vlan(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	char peer[IFNAMSIZ];
	int err;
	static uint16_t vlan_cntr = 0;
	unsigned int mtu = 0;

	if (netdev->link[0] == '\0') {
		ERROR("No link for vlan network device specified");
		return -1;
	}

	err = snprintf(peer, sizeof(peer), "vlan%d-%d",
		       netdev->priv.vlan_attr.vid, vlan_cntr++);
	if (err < 0 || (size_t)err >= sizeof(peer))
		return -1;

	err = lxc_vlan_create(netdev->link, peer, netdev->priv.vlan_attr.vid);
	if (err) {
		errno = -err;
		SYSERROR("Failed to create vlan interface \"%s\" on \"%s\"",
		         peer, netdev->link);
		return -1;
	}

	strlcpy(netdev->created_name, peer, IFNAMSIZ);

	netdev->ifindex = if_nametoindex(peer);
	if (!netdev->ifindex) {
		ERROR("Failed to retrieve ifindex for \"%s\"", peer);
		goto on_error;
	}

	if (netdev->mtu) {
		err = lxc_safe_uint(netdev->mtu, &mtu);
		if (err < 0) {
			errno = -err;
			SYSERROR("Failed to parse mtu \"%s\" for interface \"%s\"",
				 netdev->mtu, peer);
			goto on_error;
		}

		err = lxc_netdev_set_mtu(peer, mtu);
		if (err) {
			errno = -err;
			SYSERROR("Failed to set mtu \"%s\" for interface \"%s\"",
				 netdev->mtu, peer);
			goto on_error;
		}
	}

	if (netdev->upscript) {
		char *argv[] = {
		    "vlan",
		    netdev->link,
		    NULL,
		};

		err = run_script_argv(handler->name, handler->conf->hooks_version,
				      "net", netdev->upscript, "up", argv);
		if (err < 0) {
			goto on_error;
		}
	}

	DEBUG("Instantiated vlan \"%s\" with ifindex is \"%d\"", peer,
	      netdev->ifindex);

	return 0;

on_error:
	lxc_netdev_delete_by_name(peer);
	return -1;
}

static int instantiate_phys(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int err, mtu_orig = 0;
	unsigned int mtu = 0;

	if (netdev->link[0] == '\0') {
		ERROR("No link for physical interface specified");
		return -1;
	}

	/*
	 * Note that we're retrieving the container's ifindex in the host's
	 * network namespace because we need it to move the device from the
	 * host's network namespace to the container's network namespace later
	 * on.
	 * Note that netdev->link will contain the name of the physical network
	 * device in the host's namespace.
	 */
	netdev->ifindex = if_nametoindex(netdev->link);
	if (!netdev->ifindex) {
		ERROR("Failed to retrieve ifindex for \"%s\"", netdev->link);
		return -1;
	}

	strlcpy(netdev->created_name, netdev->link, IFNAMSIZ);

	/*
	 * Store the ifindex of the host's network device in the host's
	 * namespace.
	 */
	netdev->priv.phys_attr.ifindex = netdev->ifindex;

	/*
	 * Get original device MTU setting and store for restoration after
	 * container shutdown.
	 */
	mtu_orig = netdev_get_mtu(netdev->ifindex);
	if (mtu_orig < 0) {
		SYSERROR("Failed to get original mtu for interface \"%s\"", netdev->link);
		return minus_one_set_errno(-mtu_orig);
	}

	netdev->priv.phys_attr.mtu = mtu_orig;

	if (netdev->mtu) {
		err = lxc_safe_uint(netdev->mtu, &mtu);
		if (err < 0) {
			errno = -err;
			SYSERROR("Failed to parse mtu \"%s\" for interface \"%s\"",
				 netdev->mtu, netdev->link);
			return -1;
		}

		err = lxc_netdev_set_mtu(netdev->link, mtu);
		if (err < 0) {
			errno = -err;
			SYSERROR("Failed to set mtu \"%s\" for interface \"%s\"",
				 netdev->mtu, netdev->link);
			return -1;
		}
	}

	if (netdev->upscript) {
		char *argv[] = {
		    "phys",
		    netdev->link,
		    NULL,
		};

		err = run_script_argv(handler->name, handler->conf->hooks_version,
				      "net", netdev->upscript, "up", argv);
		if (err < 0) {
			return -1;
		}
	}

	DEBUG("Instantiated phys \"%s\" with ifindex is \"%d\"", netdev->link,
	      netdev->ifindex);

	return 0;
}

static int instantiate_empty(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int ret;
	char *argv[] = {
	    "empty",
	    NULL,
	};

	netdev->ifindex = 0;
	if (!netdev->upscript)
		return 0;

	ret = run_script_argv(handler->name, handler->conf->hooks_version,
			      "net", netdev->upscript, "up", argv);
	if (ret < 0)
		return -1;

	return 0;
}

static int instantiate_none(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	netdev->ifindex = 0;
	return 0;
}

static  instantiate_cb netdev_conf[LXC_NET_MAXCONFTYPE + 1] = {
	[LXC_NET_VETH]    = instantiate_veth,
	[LXC_NET_MACVLAN] = instantiate_macvlan,
	[LXC_NET_IPVLAN]  = instantiate_ipvlan,
	[LXC_NET_VLAN]    = instantiate_vlan,
	[LXC_NET_PHYS]    = instantiate_phys,
	[LXC_NET_EMPTY]   = instantiate_empty,
	[LXC_NET_NONE]    = instantiate_none,
};

static int shutdown_veth(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int ret;
	char *argv[] = {
	    "veth",
	    netdev->link,
	    NULL,
	    NULL,
	};

	if (!netdev->downscript)
		return 0;

	if (netdev->priv.veth_attr.pair[0] != '\0')
		argv[2] = netdev->priv.veth_attr.pair;
	else
		argv[2] = netdev->priv.veth_attr.veth1;

	ret = run_script_argv(handler->name,
			handler->conf->hooks_version, "net",
			netdev->downscript, "down", argv);
	if (ret < 0)
		return -1;

	return 0;
}

static int shutdown_macvlan(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int ret;
	char *argv[] = {
		"macvlan",
		netdev->link,
		NULL,
	};

	if (!netdev->downscript)
		return 0;

	ret = run_script_argv(handler->name, handler->conf->hooks_version,
			      "net", netdev->downscript, "down", argv);
	if (ret < 0)
		return -1;

	return 0;
}

static int shutdown_ipvlan(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int ret;
	char *argv[] = {
		"ipvlan",
		netdev->link,
		NULL,
	};

	if (!netdev->downscript)
		return 0;

	ret = run_script_argv(handler->name, handler->conf->hooks_version,
			      "net", netdev->downscript, "down", argv);
	if (ret < 0)
		return -1;

	return 0;
}

static int shutdown_vlan(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int ret;
	char *argv[] = {
	    "vlan",
	    netdev->link,
	    NULL,
	};

	if (!netdev->downscript)
		return 0;

	ret = run_script_argv(handler->name, handler->conf->hooks_version,
			      "net", netdev->downscript, "down", argv);
	if (ret < 0)
		return -1;

	return 0;
}

static int shutdown_phys(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int ret;
	char *argv[] = {
	    "phys",
	    netdev->link,
	    NULL,
	};

	if (!netdev->downscript)
		return 0;

	ret = run_script_argv(handler->name, handler->conf->hooks_version,
			      "net", netdev->downscript, "down", argv);
	if (ret < 0)
		return -1;

	return 0;
}

static int shutdown_empty(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	int ret;
	char *argv[] = {
	    "empty",
	    NULL,
	};

	if (!netdev->downscript)
		return 0;

	ret = run_script_argv(handler->name, handler->conf->hooks_version,
			      "net", netdev->downscript, "down", argv);
	if (ret < 0)
		return -1;

	return 0;
}

static int shutdown_none(struct lxc_handler *handler, struct lxc_netdev *netdev)
{
	return 0;
}

static  instantiate_cb netdev_deconf[LXC_NET_MAXCONFTYPE + 1] = {
	[LXC_NET_VETH]    = shutdown_veth,
	[LXC_NET_MACVLAN] = shutdown_macvlan,
	[LXC_NET_IPVLAN]  = shutdown_ipvlan,
	[LXC_NET_VLAN]    = shutdown_vlan,
	[LXC_NET_PHYS]    = shutdown_phys,
	[LXC_NET_EMPTY]   = shutdown_empty,
	[LXC_NET_NONE]    = shutdown_none,
};

static int lxc_netdev_move_by_index_fd(int ifindex, int fd, const char *ifname)
{
	int err;
	struct nl_handler nlh;
	struct ifinfomsg *ifi;
	struct nlmsg *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi)
		goto out;
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = ifindex;

	if (nla_put_u32(nlmsg, IFLA_NET_NS_FD, fd))
		goto out;

	if (ifname != NULL) {
		if (nla_put_string(nlmsg, IFLA_IFNAME, ifname))
			goto out;
	}

	err = netlink_transaction(&nlh, nlmsg, nlmsg);
out:
	netlink_close(&nlh);
	nlmsg_free(nlmsg);
	return err;
}

int lxc_netdev_move_by_index(int ifindex, pid_t pid, const char *ifname)
{
	int err;
	struct nl_handler nlh;
	struct ifinfomsg *ifi;
	struct nlmsg *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi)
		goto out;
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = ifindex;

	if (nla_put_u32(nlmsg, IFLA_NET_NS_PID, pid))
		goto out;

	if (ifname != NULL) {
		if (nla_put_string(nlmsg, IFLA_IFNAME, ifname))
			goto out;
	}

	err = netlink_transaction(&nlh, nlmsg, nlmsg);
out:
	netlink_close(&nlh);
	nlmsg_free(nlmsg);
	return err;
}

/* If we are asked to move a wireless interface, then we must actually move its
 * phyN device. Detect that condition and return the physname here. The physname
 * will be passed to lxc_netdev_move_wlan() which will free it when done.
 */
#define PHYSNAME "/sys/class/net/%s/phy80211/name"
static char *is_wlan(const char *ifname)
{
	__do_free char *path = NULL;
	int i, ret;
	long physlen;
	size_t len;
	FILE *f;
	char *physname = NULL;

	len = strlen(ifname) + strlen(PHYSNAME) - 1;
	path = must_realloc(NULL, len + 1);
	ret = snprintf(path, len, PHYSNAME, ifname);
	if (ret < 0 || (size_t)ret >= len)
		goto bad;

	f = fopen(path, "r");
	if (!f)
		goto bad;

	/* Feh - sb.st_size is always 4096. */
	fseek(f, 0, SEEK_END);
	physlen = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (physlen < 0) {
		fclose(f);
		goto bad;
	}

	physname = malloc(physlen + 1);
	if (!physname) {
		fclose(f);
		goto bad;
	}

	memset(physname, 0, physlen + 1);
	ret = fread(physname, 1, physlen, f);
	fclose(f);
	if (ret < 0)
		goto bad;

	for (i = 0; i < physlen; i++) {
		if (physname[i] == '\n')
			physname[i] = '\0';

		if (physname[i] == '\0')
			break;
	}

	return physname;

bad:
	free(physname);
	return NULL;
}

static int lxc_netdev_rename_by_name_in_netns(pid_t pid, const char *old,
					      const char *new)
{
	pid_t fpid;

	fpid = fork();
	if (fpid < 0)
		return -1;

	if (fpid != 0)
		return wait_for_pid(fpid);

	if (!switch_to_ns(pid, "net"))
		return -1;

	_exit(lxc_netdev_rename_by_name(old, new));
}

static int lxc_netdev_move_wlan(char *physname, const char *ifname, pid_t pid,
				const char *newname)
{
	char *cmd;
	pid_t fpid;
	int err = -1;

	/* Move phyN into the container.  TODO - do this using netlink.
	 * However, IIUC this involves a bit more complicated work to talk to
	 * the 80211 module, so for now just call out to iw.
	 */
	cmd = on_path("iw", NULL);
	if (!cmd)
		goto out1;
	free(cmd);

	fpid = fork();
	if (fpid < 0)
		goto out1;

	if (fpid == 0) {
		char pidstr[30];
		sprintf(pidstr, "%d", pid);
		execlp("iw", "iw", "phy", physname, "set", "netns", pidstr,
		       (char *)NULL);
		_exit(EXIT_FAILURE);
	}

	if (wait_for_pid(fpid))
		goto out1;

	err = 0;
	if (newname)
		err = lxc_netdev_rename_by_name_in_netns(pid, ifname, newname);

out1:
	free(physname);
	return err;
}

int lxc_netdev_move_by_name(const char *ifname, pid_t pid, const char* newname)
{
	int index;
	char *physname;

	if (!ifname)
		return -EINVAL;

	index = if_nametoindex(ifname);
	if (!index)
		return -EINVAL;

	physname = is_wlan(ifname);
	if (physname)
		return lxc_netdev_move_wlan(physname, ifname, pid, newname);

	return lxc_netdev_move_by_index(index, pid, newname);
}

int lxc_netdev_delete_by_index(int ifindex)
{
	int err;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_ACK | NLM_F_REQUEST;
	nlmsg->nlmsghdr->nlmsg_type = RTM_DELLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi)
		goto out;
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = ifindex;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

int lxc_netdev_delete_by_name(const char *name)
{
	int index;

	index = if_nametoindex(name);
	if (!index)
		return -EINVAL;

	return lxc_netdev_delete_by_index(index);
}

int lxc_netdev_rename_by_index(int ifindex, const char *newname)
{
	int err, len;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	len = strlen(newname);
	if (len == 1 || len >= IFNAMSIZ) {
		err = -EINVAL;
		goto out;
	}

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_ACK | NLM_F_REQUEST;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi)
		goto out;
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = ifindex;

	if (nla_put_string(nlmsg, IFLA_IFNAME, newname))
		goto out;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

int lxc_netdev_rename_by_name(const char *oldname, const char *newname)
{
	int len, index;

	len = strlen(oldname);
	if (len == 1 || len >= IFNAMSIZ)
		return -EINVAL;

	index = if_nametoindex(oldname);
	if (!index)
		return -EINVAL;

	return lxc_netdev_rename_by_index(index, newname);
}

int netdev_set_flag(const char *name, int flag)
{
	int err, index, len;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -EINVAL;
	len = strlen(name);
	if (len == 1 || len >= IFNAMSIZ)
		goto out;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	err = -EINVAL;
	index = if_nametoindex(name);
	if (!index)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi) {
		err = -ENOMEM;
		goto out;
	}
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = index;
	ifi->ifi_change |= IFF_UP;
	ifi->ifi_flags |= flag;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(nlmsg);
	nlmsg_free(answer);
	return err;
}

int netdev_get_flag(const char *name, int *flag)
{
	int err, index, len;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	if (!name)
		return -EINVAL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -EINVAL;
	len = strlen(name);
	if (len == 1 || len >= IFNAMSIZ)
		goto out;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	err = -EINVAL;
	index = if_nametoindex(name);
	if (!index)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_REQUEST;
	nlmsg->nlmsghdr->nlmsg_type = RTM_GETLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi) {
		err = -ENOMEM;
		goto out;
	}
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = index;

	err = netlink_transaction(&nlh, nlmsg, answer);
	if (err)
		goto out;

	ifi = NLMSG_DATA(answer->nlmsghdr);

	*flag = ifi->ifi_flags;
out:
	netlink_close(&nlh);
	nlmsg_free(nlmsg);
	nlmsg_free(answer);
	return err;
}

/*
 * \brief Check a interface is up or not.
 *
 * \param name: name for the interface.
 *
 * \return int.
 * 0 means interface is down.
 * 1 means interface is up.
 * Others means error happened, and ret-value is the error number.
 */
int lxc_netdev_isup(const char *name)
{
	int err, flag;

	err = netdev_get_flag(name, &flag);
	if (err)
		return err;

	if (flag & IFF_UP)
		return 1;

	return 0;
}

int netdev_get_mtu(int ifindex)
{
	int answer_len, err, res;
	struct nl_handler nlh;
	struct ifinfomsg *ifi;
	struct nlmsghdr *msg;
	int readmore = 0, recv_len = 0;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	/* Save the answer buffer length, since it will be overwritten
	 * on the first receive (and we might need to receive more than
	 * once.
	 */
	answer_len = answer->nlmsghdr->nlmsg_len;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlmsg->nlmsghdr->nlmsg_type = RTM_GETLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi)
		goto out;
	ifi->ifi_family = AF_UNSPEC;

	/* Send the request for addresses, which returns all addresses
	 * on all interfaces. */
	err = netlink_send(&nlh, nlmsg);
	if (err < 0)
		goto out;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"

	do {
		/* Restore the answer buffer length, it might have been
		 * overwritten by a previous receive.
		 */
		answer->nlmsghdr->nlmsg_len = answer_len;

		/* Get the (next) batch of reply messages */
		err = netlink_rcv(&nlh, answer);
		if (err < 0)
			goto out;

		recv_len = err;

		/* Satisfy the typing for the netlink macros */
		msg = answer->nlmsghdr;

		while (NLMSG_OK(msg, recv_len)) {

			/* Stop reading if we see an error message */
			if (msg->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *errmsg =
				    (struct nlmsgerr *)NLMSG_DATA(msg);
				err = errmsg->error;
				goto out;
			}

			/* Stop reading if we see a NLMSG_DONE message */
			if (msg->nlmsg_type == NLMSG_DONE) {
				readmore = 0;
				break;
			}

			ifi = NLMSG_DATA(msg);
			if (ifi->ifi_index == ifindex) {
				struct rtattr *rta = IFLA_RTA(ifi);
				int attr_len =
				    msg->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
				res = 0;
				while (RTA_OK(rta, attr_len)) {
					/* Found a local address for the
					 * requested interface, return it.
					 */
					if (rta->rta_type == IFLA_MTU) {
						memcpy(&res, RTA_DATA(rta),
						       sizeof(int));
						err = res;
						goto out;
					}
					rta = RTA_NEXT(rta, attr_len);
				}
			}

			/* Keep reading more data from the socket if the last
			 * message had the NLF_F_MULTI flag set.
			 */
			readmore = (msg->nlmsg_flags & NLM_F_MULTI);

			/* Look at the next message received in this buffer. */
			msg = NLMSG_NEXT(msg, recv_len);
		}
	} while (readmore);

#pragma GCC diagnostic pop

	/* If we end up here, we didn't find any result, so signal an error. */
	err = -1;

out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

int lxc_netdev_set_mtu(const char *name, int mtu)
{
	int err, index, len;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -EINVAL;
	len = strlen(name);
	if (len == 1 || len >= IFNAMSIZ)
		goto out;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	err = -EINVAL;
	index = if_nametoindex(name);
	if (!index)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi) {
		err = -ENOMEM;
		goto out;
	}
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = index;

	if (nla_put_u32(nlmsg, IFLA_MTU, mtu))
		goto out;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(nlmsg);
	nlmsg_free(answer);
	return err;
}

int lxc_netdev_up(const char *name)
{
	return netdev_set_flag(name, IFF_UP);
}

int lxc_netdev_down(const char *name)
{
	return netdev_set_flag(name, 0);
}

int lxc_veth_create(const char *name1, const char *name2)
{
	int err, len;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct rtattr *nest1, *nest2, *nest3;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -EINVAL;
	len = strlen(name1);
	if (len == 1 || len >= IFNAMSIZ)
		goto out;

	len = strlen(name2);
	if (len == 1 || len >= IFNAMSIZ)
		goto out;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags =
	    NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi)
		goto out;
	ifi->ifi_family = AF_UNSPEC;

	err = -EINVAL;
	nest1 = nla_begin_nested(nlmsg, IFLA_LINKINFO);
	if (!nest1)
		goto out;

	if (nla_put_string(nlmsg, IFLA_INFO_KIND, "veth"))
		goto out;

	nest2 = nla_begin_nested(nlmsg, IFLA_INFO_DATA);
	if (!nest2)
		goto out;

	nest3 = nla_begin_nested(nlmsg, VETH_INFO_PEER);
	if (!nest3)
		goto out;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi) {
		err = -ENOMEM;
		goto out;
	}

	if (nla_put_string(nlmsg, IFLA_IFNAME, name2))
		goto out;

	nla_end_nested(nlmsg, nest3);
	nla_end_nested(nlmsg, nest2);
	nla_end_nested(nlmsg, nest1);

	if (nla_put_string(nlmsg, IFLA_IFNAME, name1))
		goto out;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

/* TODO: merge with lxc_macvlan_create */
int lxc_vlan_create(const char *master, const char *name, unsigned short vlanid)
{
	int err, len, lindex;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct rtattr *nest, *nest2;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -EINVAL;
	len = strlen(master);
	if (len == 1 || len >= IFNAMSIZ)
		goto err3;

	len = strlen(name);
	if (len == 1 || len >= IFNAMSIZ)
		goto err3;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto err3;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto err2;

	err = -EINVAL;
	lindex = if_nametoindex(master);
	if (!lindex)
		goto err1;

	nlmsg->nlmsghdr->nlmsg_flags =
	    NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi) {
		err = -ENOMEM;
		goto err1;
	}
	ifi->ifi_family = AF_UNSPEC;

	nest = nla_begin_nested(nlmsg, IFLA_LINKINFO);
	if (!nest)
		goto err1;

	if (nla_put_string(nlmsg, IFLA_INFO_KIND, "vlan"))
		goto err1;

	nest2 = nla_begin_nested(nlmsg, IFLA_INFO_DATA);
	if (!nest2)
		goto err1;

	if (nla_put_u16(nlmsg, IFLA_VLAN_ID, vlanid))
		goto err1;

	nla_end_nested(nlmsg, nest2);
	nla_end_nested(nlmsg, nest);

	if (nla_put_u32(nlmsg, IFLA_LINK, lindex))
		goto err1;

	if (nla_put_string(nlmsg, IFLA_IFNAME, name))
		goto err1;

	err = netlink_transaction(&nlh, nlmsg, answer);
err1:
	nlmsg_free(answer);
err2:
	nlmsg_free(nlmsg);
err3:
	netlink_close(&nlh);
	return err;
}

int lxc_macvlan_create(const char *master, const char *name, int mode)
{
	int err, index, len;
	struct ifinfomsg *ifi;
	struct nl_handler nlh;
	struct rtattr *nest, *nest2;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -EINVAL;
	len = strlen(master);
	if (len == 1 || len >= IFNAMSIZ)
		goto out;

	len = strlen(name);
	if (len == 1 || len >= IFNAMSIZ)
		goto out;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	err = -EINVAL;
	index = if_nametoindex(master);
	if (!index)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags =
	    NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWLINK;

	ifi = nlmsg_reserve(nlmsg, sizeof(struct ifinfomsg));
	if (!ifi) {
		err = -ENOMEM;
		goto out;
	}
	ifi->ifi_family = AF_UNSPEC;

	nest = nla_begin_nested(nlmsg, IFLA_LINKINFO);
	if (!nest)
		goto out;

	if (nla_put_string(nlmsg, IFLA_INFO_KIND, "macvlan"))
		goto out;

	if (mode) {
		nest2 = nla_begin_nested(nlmsg, IFLA_INFO_DATA);
		if (!nest2)
			goto out;

		if (nla_put_u32(nlmsg, IFLA_MACVLAN_MODE, mode))
			goto out;

		nla_end_nested(nlmsg, nest2);
	}

	nla_end_nested(nlmsg, nest);

	if (nla_put_u32(nlmsg, IFLA_LINK, index))
		goto out;

	if (nla_put_string(nlmsg, IFLA_IFNAME, name))
		goto out;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

static int proc_sys_net_write(const char *path, const char *value)
{
	int fd;
	int err = 0;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;

	if (lxc_write_nointr(fd, value, strlen(value)) < 0)
		err = -errno;

	close(fd);
	return err;
}

static int ip_forwarding_set(const char *ifname, int family, int flag)
{
	int ret;
	char path[PATH_MAX];

	if (family != AF_INET && family != AF_INET6)
		return -EINVAL;

	ret = snprintf(path, PATH_MAX, "/proc/sys/net/%s/conf/%s/%s",
		       family == AF_INET ? "ipv4" : "ipv6", ifname, "forwarding");
	if (ret < 0 || (size_t)ret >= PATH_MAX)
		return -E2BIG;

	return proc_sys_net_write(path, flag ? "1" : "0");
}

int lxc_ip_forwarding_on(const char *name, int family)
{
	return ip_forwarding_set(name, family, 1);
}

int lxc_ip_forwarding_off(const char *name, int family)
{
	return ip_forwarding_set(name, family, 0);
}

static int neigh_proxy_set(const char *ifname, int family, int flag)
{
	int ret;
	char path[PATH_MAX];

	if (family != AF_INET && family != AF_INET6)
		return -EINVAL;

	ret = snprintf(path, PATH_MAX, "/proc/sys/net/%s/conf/%s/%s",
		       family == AF_INET ? "ipv4" : "ipv6", ifname,
		       family == AF_INET ? "proxy_arp" : "proxy_ndp");
	if (ret < 0 || (size_t)ret >= PATH_MAX)
		return -E2BIG;

	return proc_sys_net_write(path, flag ? "1" : "0");
}

static int lxc_is_ip_neigh_proxy_enabled(const char *ifname, int family)
{
	int ret;
	char path[PATH_MAX];
	char buf[1] = "";

	if (family != AF_INET && family != AF_INET6)
		return minus_one_set_errno(EINVAL);

	ret = snprintf(path, PATH_MAX, "/proc/sys/net/%s/conf/%s/%s",
		       family == AF_INET ? "ipv4" : "ipv6", ifname,
		       family == AF_INET ? "proxy_arp" : "proxy_ndp");
	if (ret < 0 || (size_t)ret >= PATH_MAX)
		return minus_one_set_errno(E2BIG);

	return lxc_read_file_expect(path, buf, 1, "1");
}

int lxc_neigh_proxy_on(const char *name, int family)
{
	return neigh_proxy_set(name, family, 1);
}

int lxc_neigh_proxy_off(const char *name, int family)
{
	return neigh_proxy_set(name, family, 0);
}

int lxc_convert_mac(char *macaddr, struct sockaddr *sockaddr)
{
	int i = 0;
	unsigned val;
	char c;
	unsigned char *data;

	sockaddr->sa_family = ARPHRD_ETHER;
	data = (unsigned char *)sockaddr->sa_data;

	while ((*macaddr != '\0') && (i < ETH_ALEN)) {
		c = *macaddr++;
		if (isdigit(c))
			val = c - '0';
		else if (c >= 'a' && c <= 'f')
			val = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			val = c - 'A' + 10;
		else
			return -EINVAL;

		val <<= 4;
		c = *macaddr;
		if (isdigit(c))
			val |= c - '0';
		else if (c >= 'a' && c <= 'f')
			val |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			val |= c - 'A' + 10;
		else if (c == ':' || c == 0)
			val >>= 4;
		else
			return -EINVAL;
		if (c != 0)
			macaddr++;
		*data++ = (unsigned char)(val & 0377);
		i++;

		if (*macaddr == ':')
			macaddr++;
	}

	return 0;
}

static int ip_addr_add(int family, int ifindex, void *addr, void *bcast,
		       void *acast, int prefix)
{
	int addrlen, err;
	struct ifaddrmsg *ifa;
	struct nl_handler nlh;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	addrlen = family == AF_INET ? sizeof(struct in_addr)
				    : sizeof(struct in6_addr);

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags =
	    NLM_F_ACK | NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWADDR;

	ifa = nlmsg_reserve(nlmsg, sizeof(struct ifaddrmsg));
	if (!ifa)
		goto out;
	ifa->ifa_prefixlen = prefix;
	ifa->ifa_index = ifindex;
	ifa->ifa_family = family;
	ifa->ifa_scope = 0;

	err = -EINVAL;
	if (nla_put_buffer(nlmsg, IFA_LOCAL, addr, addrlen))
		goto out;

	if (nla_put_buffer(nlmsg, IFA_ADDRESS, addr, addrlen))
		goto out;

	if (nla_put_buffer(nlmsg, IFA_BROADCAST, bcast, addrlen))
		goto out;

	/* TODO: multicast, anycast with ipv6 */
	err = -EPROTONOSUPPORT;
	if (family == AF_INET6 &&
	    (memcmp(bcast, &in6addr_any, sizeof(in6addr_any)) ||
	     memcmp(acast, &in6addr_any, sizeof(in6addr_any))))
		goto out;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

int lxc_ipv6_addr_add(int ifindex, struct in6_addr *addr,
		      struct in6_addr *mcast, struct in6_addr *acast,
		      int prefix)
{
	return ip_addr_add(AF_INET6, ifindex, addr, mcast, acast, prefix);
}

int lxc_ipv4_addr_add(int ifindex, struct in_addr *addr, struct in_addr *bcast,
		      int prefix)
{
	return ip_addr_add(AF_INET, ifindex, addr, bcast, NULL, prefix);
}

/* Find an IFA_LOCAL (or IFA_ADDRESS if not IFA_LOCAL is present) address from
 * the given RTM_NEWADDR message. Allocates memory for the address and stores
 * that pointer in *res (so res should be an in_addr** or in6_addr**).
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"

static int ifa_get_local_ip(int family, struct nlmsghdr *msg, void **res)
{
	int addrlen;
	struct ifaddrmsg *ifa = NLMSG_DATA(msg);
	struct rtattr *rta = IFA_RTA(ifa);
	int attr_len = NLMSG_PAYLOAD(msg, sizeof(struct ifaddrmsg));

	if (ifa->ifa_family != family)
		return 0;

	addrlen = family == AF_INET ? sizeof(struct in_addr)
				    : sizeof(struct in6_addr);

	/* Loop over the rtattr's in this message */
	while (RTA_OK(rta, attr_len)) {
		/* Found a local address for the requested interface,
		 * return it.
		 */
		if (rta->rta_type == IFA_LOCAL ||
		    rta->rta_type == IFA_ADDRESS) {
			/* Sanity check. The family check above should make sure
			 * the address length is correct, but check here just in
			 * case.
			 */
			if (RTA_PAYLOAD(rta) != addrlen)
				return -1;

			/* We might have found an IFA_ADDRESS before, which we
			 * now overwrite with an IFA_LOCAL.
			 */
			if (!*res) {
				*res = malloc(addrlen);
				if (!*res)
					return -1;
			}

			memcpy(*res, RTA_DATA(rta), addrlen);
			if (rta->rta_type == IFA_LOCAL)
				break;
		}
		rta = RTA_NEXT(rta, attr_len);
	}
	return 0;
}

#pragma GCC diagnostic pop

static int ip_addr_get(int family, int ifindex, void **res)
{
	int answer_len, err;
	struct ifaddrmsg *ifa;
	struct nl_handler nlh;
	struct nlmsghdr *msg;
	int readmore = 0, recv_len = 0;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	/* Save the answer buffer length, since it will be overwritten on the
	 * first receive (and we might need to receive more than once).
	 */
	answer_len = answer->nlmsghdr->nlmsg_len;

	nlmsg->nlmsghdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
	nlmsg->nlmsghdr->nlmsg_type = RTM_GETADDR;

	ifa = nlmsg_reserve(nlmsg, sizeof(struct ifaddrmsg));
	if (!ifa)
		goto out;
	ifa->ifa_family = family;

	/* Send the request for addresses, which returns all addresses on all
	 * interfaces.
	 */
	err = netlink_send(&nlh, nlmsg);
	if (err < 0)
		goto out;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"

	do {
		/* Restore the answer buffer length, it might have been
		 * overwritten by a previous receive.
		 */
		answer->nlmsghdr->nlmsg_len = answer_len;

		/* Get the (next) batch of reply messages. */
		err = netlink_rcv(&nlh, answer);
		if (err < 0)
			goto out;

		recv_len = err;
		err = 0;

		/* Satisfy the typing for the netlink macros. */
		msg = answer->nlmsghdr;

		while (NLMSG_OK(msg, recv_len)) {
			/* Stop reading if we see an error message. */
			if (msg->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *errmsg =
				    (struct nlmsgerr *)NLMSG_DATA(msg);
				err = errmsg->error;
				goto out;
			}

			/* Stop reading if we see a NLMSG_DONE message. */
			if (msg->nlmsg_type == NLMSG_DONE) {
				readmore = 0;
				break;
			}

			if (msg->nlmsg_type != RTM_NEWADDR) {
				err = -1;
				goto out;
			}

			ifa = (struct ifaddrmsg *)NLMSG_DATA(msg);
			if (ifa->ifa_index == ifindex) {
				if (ifa_get_local_ip(family, msg, res) < 0) {
					err = -1;
					goto out;
				}

				/* Found a result, stop searching. */
				if (*res)
					goto out;
			}

			/* Keep reading more data from the socket if the last
			 * message had the NLF_F_MULTI flag set.
			 */
			readmore = (msg->nlmsg_flags & NLM_F_MULTI);

			/* Look at the next message received in this buffer. */
			msg = NLMSG_NEXT(msg, recv_len);
		}
	} while (readmore);

#pragma GCC diagnostic pop

	/* If we end up here, we didn't find any result, so signal an
	 * error.
	 */
	err = -1;

out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

int lxc_ipv6_addr_get(int ifindex, struct in6_addr **res)
{
	return ip_addr_get(AF_INET6, ifindex, (void **)res);
}

int lxc_ipv4_addr_get(int ifindex, struct in_addr **res)
{
	return ip_addr_get(AF_INET, ifindex, (void **)res);
}

static int ip_gateway_add(int family, int ifindex, void *gw)
{
	int addrlen, err;
	struct nl_handler nlh;
	struct rtmsg *rt;
	struct nlmsg *answer = NULL, *nlmsg = NULL;

	addrlen = family == AF_INET ? sizeof(struct in_addr)
				    : sizeof(struct in6_addr);

	err = netlink_open(&nlh, NETLINK_ROUTE);
	if (err)
		return err;

	err = -ENOMEM;
	nlmsg = nlmsg_alloc(NLMSG_GOOD_SIZE);
	if (!nlmsg)
		goto out;

	answer = nlmsg_alloc_reserve(NLMSG_GOOD_SIZE);
	if (!answer)
		goto out;

	nlmsg->nlmsghdr->nlmsg_flags =
	    NLM_F_ACK | NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
	nlmsg->nlmsghdr->nlmsg_type = RTM_NEWROUTE;

	rt = nlmsg_reserve(nlmsg, sizeof(struct rtmsg));
	if (!rt)
		goto out;
	rt->rtm_family = family;
	rt->rtm_table = RT_TABLE_MAIN;
	rt->rtm_scope = RT_SCOPE_UNIVERSE;
	rt->rtm_protocol = RTPROT_BOOT;
	rt->rtm_type = RTN_UNICAST;
	/* "default" destination */
	rt->rtm_dst_len = 0;

	err = -EINVAL;

	/* If gateway address not supplied, then a device route will be created instead */
	if (gw != NULL) {
		if (nla_put_buffer(nlmsg, RTA_GATEWAY, gw, addrlen))
			goto out;
	}

	/* Adding the interface index enables the use of link-local
	 * addresses for the gateway.
	 */
	if (nla_put_u32(nlmsg, RTA_OIF, ifindex))
		goto out;

	err = netlink_transaction(&nlh, nlmsg, answer);
out:
	netlink_close(&nlh);
	nlmsg_free(answer);
	nlmsg_free(nlmsg);
	return err;
}

int lxc_ipv4_gateway_add(int ifindex, struct in_addr *gw)
{
	return ip_gateway_add(AF_INET, ifindex, gw);
}

int lxc_ipv6_gateway_add(int ifindex, struct in6_addr *gw)
{
	return ip_gateway_add(AF_INET6, ifindex, gw);
}
bool is_ovs_bridge(const char *bridge)
{
	int ret;
	struct stat sb;
	char brdirname[22 + IFNAMSIZ + 1] = {0};

	ret = snprintf(brdirname, 22 + IFNAMSIZ + 1, "/sys/class/net/%s/bridge",
		       bridge);
	if (ret < 0 || (size_t)ret >= 22 + IFNAMSIZ + 1)
		return false;

	ret = stat(brdirname, &sb);
	if (ret < 0 && errno == ENOENT)
		return true;

	return false;
}

struct ovs_veth_args {
	const char *bridge;
	const char *nic;
};

/* Called from a background thread - when nic goes away, remove it from the
 * bridge.
 */
static int lxc_ovs_delete_port_exec(void *data)
{
	struct ovs_veth_args *args = data;

	execlp("ovs-vsctl", "ovs-vsctl", "del-port", args->bridge, args->nic,
	       (char *)NULL);
	return -1;
}

int lxc_ovs_delete_port(const char *bridge, const char *nic)
{
	int ret;
	char cmd_output[PATH_MAX];
	struct ovs_veth_args args;

	args.bridge = bridge;
	args.nic = nic;
	ret = run_command(cmd_output, sizeof(cmd_output),
			  lxc_ovs_delete_port_exec, (void *)&args);
	if (ret < 0) {
		ERROR("Failed to delete \"%s\" from openvswitch bridge \"%s\": "
		      "%s", bridge, nic, cmd_output);
		return -1;
	}

	return 0;
}

static int lxc_ovs_attach_bridge_exec(void *data)
{
	struct ovs_veth_args *args = data;

	execlp("ovs-vsctl", "ovs-vsctl", "add-port", args->bridge, args->nic,
	       (char *)NULL);
	return -1;
}

static int lxc_ovs_attach_bridge(const char *bridge, const char *nic)
{
	int ret;
	char cmd_output[PATH_MAX];
	struct ovs_veth_args args;

	args.bridge = bridge;
	args.nic = nic;
	ret = run_command(cmd_output, sizeof(cmd_output),
			  lxc_ovs_attach_bridge_exec, (void *)&args);
	if (ret < 0) {
		ERROR("Failed to attach \"%s\" to openvswitch bridge \"%s\": %s",
		      bridge, nic, cmd_output);
		return -1;
	}

	return 0;
}

int lxc_bridge_attach(const char *bridge, const char *ifname)
{
	int err, fd, index;
	size_t retlen;
	struct ifreq ifr;

	if (strlen(ifname) >= IFNAMSIZ)
		return -EINVAL;

	index = if_nametoindex(ifname);
	if (!index)
		return -EINVAL;

	if (is_ovs_bridge(bridge))
		return lxc_ovs_attach_bridge(bridge, ifname);

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	retlen = strlcpy(ifr.ifr_name, bridge, IFNAMSIZ);
	if (retlen >= IFNAMSIZ) {
		close(fd);
		return -E2BIG;
	}

	ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	ifr.ifr_ifindex = index;
	err = ioctl(fd, SIOCBRADDIF, &ifr);
	close(fd);
	if (err)
		err = -errno;

	return err;
}

static const char *const lxc_network_types[LXC_NET_MAXCONFTYPE + 1] = {
	[LXC_NET_EMPTY]   = "empty",
	[LXC_NET_VETH]    = "veth",
	[LXC_NET_MACVLAN] = "macvlan",
	[LXC_NET_IPVLAN]  = "ipvlan",
	[LXC_NET_PHYS]    = "phys",
	[LXC_NET_VLAN]    = "vlan",
	[LXC_NET_NONE]    = "none",
};

const char *lxc_net_type_to_str(int type)
{
	if (type < 0 || type > LXC_NET_MAXCONFTYPE)
		return NULL;

	return lxc_network_types[type];
}

static const char padchar[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

char *lxc_mkifname(char *template)
{
	int ret;
	struct netns_ifaddrs *ifa, *ifaddr;
	char name[IFNAMSIZ];
	bool exists = false;
	size_t i = 0;
#ifdef HAVE_RAND_R
	unsigned int seed;

	seed = randseed(false);
#else

	(void)randseed(true);
#endif

	if (strlen(template) >= IFNAMSIZ)
		return NULL;

	/* Get all the network interfaces. */
	ret = netns_getifaddrs(&ifaddr, -1, &(bool){false});
	if (ret < 0) {
		SYSERROR("Failed to get network interfaces");
		return NULL;
	}

	/* Generate random names until we find one that doesn't exist. */
	for (;;) {
		name[0] = '\0';
		(void)strlcpy(name, template, IFNAMSIZ);

		exists = false;

		for (i = 0; i < strlen(name); i++) {
			if (name[i] == 'X') {
#ifdef HAVE_RAND_R
				name[i] = padchar[rand_r(&seed) % strlen(padchar)];
#else
				name[i] = padchar[rand() % strlen(padchar)];
#endif
			}
		}

		for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
			if (!strcmp(ifa->ifa_name, name)) {
				exists = true;
				break;
			}
		}

		if (!exists)
			break;
	}

	netns_freeifaddrs(ifaddr);
	(void)strlcpy(template, name, strlen(template) + 1);

	return template;
}

int setup_private_host_hw_addr(char *veth1)
{
	int err, sockfd;
	struct ifreq ifr;

	sockfd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sockfd < 0)
		return -errno;

	err = snprintf((char *)ifr.ifr_name, IFNAMSIZ, "%s", veth1);
	if (err < 0 || (size_t)err >= IFNAMSIZ) {
		close(sockfd);
		return -E2BIG;
	}

	err = ioctl(sockfd, SIOCGIFHWADDR, &ifr);
	if (err < 0) {
		close(sockfd);
		return -errno;
	}

	ifr.ifr_hwaddr.sa_data[0] = 0xfe;
	err = ioctl(sockfd, SIOCSIFHWADDR, &ifr);
	close(sockfd);
	if (err < 0)
		return -errno;

	return 0;
}

int lxc_find_gateway_addresses(struct lxc_handler *handler)
{
	struct lxc_list *network = &handler->conf->network;
	struct lxc_list *iterator;
	struct lxc_netdev *netdev;
	int link_index;

	lxc_list_for_each(iterator, network) {
		netdev = iterator->elem;

		if (!netdev->ipv4_gateway_auto && !netdev->ipv6_gateway_auto)
			continue;

		if (netdev->type != LXC_NET_VETH && netdev->type != LXC_NET_MACVLAN) {
			ERROR("Automatic gateway detection is only supported for veth and macvlan");
			return -1;
		}

		if (netdev->link[0] == '\0') {
			ERROR("Automatic gateway detection needs a link interface");
			return -1;
		}

		link_index = if_nametoindex(netdev->link);
		if (!link_index)
			return -EINVAL;

		if (netdev->ipv4_gateway_auto) {
			if (lxc_ipv4_addr_get(link_index, &netdev->ipv4_gateway)) {
				ERROR("Failed to automatically find ipv4 gateway address from link interface \"%s\"",
				      netdev->link);
				return -1;
			}
		}

		if (netdev->ipv6_gateway_auto) {
			if (lxc_ipv6_addr_get(link_index, &netdev->ipv6_gateway)) {
				ERROR("Failed to automatically find ipv6 gateway address from link interface \"%s\"",
				      netdev->link);
				return -1;
			}
		}
	}

	return 0;
}

#define LXC_USERNIC_PATH LIBEXECDIR "/lxc/lxc-user-nic"
static int lxc_create_network_unpriv_exec(const char *lxcpath, const char *lxcname,
					  struct lxc_netdev *netdev, pid_t pid, unsigned int hooks_version)
{
	int ret;
	pid_t child;
	int bytes, pipefd[2];
	char *token, *saveptr = NULL;
	char netdev_link[IFNAMSIZ];
	char buffer[PATH_MAX] = {0};
	size_t retlen;

	if (netdev->type != LXC_NET_VETH) {
		ERROR("Network type %d not support for unprivileged use", netdev->type);
		return -1;
	}

	ret = pipe(pipefd);
	if (ret < 0) {
		SYSERROR("Failed to create pipe");
		return -1;
	}

	child = fork();
	if (child < 0) {
		SYSERROR("Failed to create new process");
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	if (child == 0) {
		char pidstr[INTTYPE_TO_STRLEN(pid_t)];

		close(pipefd[0]);

		ret = dup2(pipefd[1], STDOUT_FILENO);
		if (ret >= 0)
			ret = dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		if (ret < 0) {
			SYSERROR("Failed to duplicate std{err,out} file descriptor");
			_exit(EXIT_FAILURE);
		}

		if (netdev->link[0] != '\0')
			retlen = strlcpy(netdev_link, netdev->link, IFNAMSIZ);
		else
			retlen = strlcpy(netdev_link, "none", IFNAMSIZ);
		if (retlen >= IFNAMSIZ) {
			SYSERROR("Invalid network device name");
			_exit(EXIT_FAILURE);
		}

		ret = snprintf(pidstr, sizeof(pidstr), "%d", pid);
		if (ret < 0 || ret >= sizeof(pidstr))
			_exit(EXIT_FAILURE);
		pidstr[sizeof(pidstr) - 1] = '\0';

		INFO("Execing lxc-user-nic create %s %s %s veth %s %s", lxcpath,
		     lxcname, pidstr, netdev_link,
		     netdev->name[0] != '\0' ? netdev->name : "(null)");
		if (netdev->name[0] != '\0')
			execlp(LXC_USERNIC_PATH, LXC_USERNIC_PATH, "create",
			       lxcpath, lxcname, pidstr, "veth", netdev_link,
			       netdev->name, (char *)NULL);
		else
			execlp(LXC_USERNIC_PATH, LXC_USERNIC_PATH, "create",
			       lxcpath, lxcname, pidstr, "veth", netdev_link,
			       (char *)NULL);
		SYSERROR("Failed to execute lxc-user-nic");
		_exit(EXIT_FAILURE);
	}

	/* close the write-end of the pipe */
	close(pipefd[1]);

	bytes = lxc_read_nointr(pipefd[0], &buffer, PATH_MAX);
	if (bytes < 0) {
		SYSERROR("Failed to read from pipe file descriptor");
		close(pipefd[0]);
	} else {
		buffer[bytes - 1] = '\0';
	}

	ret = wait_for_pid(child);
	close(pipefd[0]);
	if (ret != 0 || bytes < 0) {
		ERROR("lxc-user-nic failed to configure requested network: %s",
		      buffer[0] != '\0' ? buffer : "(null)");
		return -1;
	}
	TRACE("Received output \"%s\" from lxc-user-nic", buffer);

	/* netdev->name */
	token = strtok_r(buffer, ":", &saveptr);
	if (!token) {
		ERROR("Failed to parse lxc-user-nic output");
		return -1;
	}

	/*
	 * lxc-user-nic will take care of proper network device naming. So
	 * netdev->name and netdev->created_name need to be identical to not
	 * trigger another rename later on.
	 */
	retlen = strlcpy(netdev->name, token, IFNAMSIZ);
	if (retlen < IFNAMSIZ)
		retlen = strlcpy(netdev->created_name, token, IFNAMSIZ);
	if (retlen >= IFNAMSIZ) {
		ERROR("Container side veth device name returned by lxc-user-nic is too long");
		return -E2BIG;
	}

	/* netdev->ifindex */
	token = strtok_r(NULL, ":", &saveptr);
	if (!token) {
		ERROR("Failed to parse lxc-user-nic output");
		return -1;
	}

	ret = lxc_safe_int(token, &netdev->ifindex);
	if (ret < 0) {
		errno = -ret;
		SYSERROR("Failed to convert string \"%s\" to integer", token);
		return -1;
	}

	/* netdev->priv.veth_attr.veth1 */
	token = strtok_r(NULL, ":", &saveptr);
	if (!token) {
		ERROR("Failed to parse lxc-user-nic output");
		return -1;
	}

	retlen = strlcpy(netdev->priv.veth_attr.veth1, token, IFNAMSIZ);
	if (retlen >= IFNAMSIZ) {
		ERROR("Host side veth device name returned by lxc-user-nic is "
		      "too long");
		return -E2BIG;
	}

	/* netdev->priv.veth_attr.ifindex */
	token = strtok_r(NULL, ":", &saveptr);
	if (!token) {
		ERROR("Failed to parse lxc-user-nic output");
		return -1;
	}

	ret = lxc_safe_int(token, &netdev->priv.veth_attr.ifindex);
	if (ret < 0) {
		errno = -ret;
		SYSERROR("Failed to convert string \"%s\" to integer", token);
		return -1;
	}

	if (netdev->upscript) {
		char *argv[] = {
			"veth",
			netdev->link,
			netdev->priv.veth_attr.veth1,
			NULL,
		};

		ret = run_script_argv(lxcname, hooks_version, "net",
				      netdev->upscript, "up", argv);
		if (ret < 0)
			return -1;
    }

	return 0;
}

static int lxc_delete_network_unpriv_exec(const char *lxcpath, const char *lxcname,
					  struct lxc_netdev *netdev,
					  const char *netns_path)
{
	int bytes, ret;
	pid_t child;
	int pipefd[2];
	char buffer[PATH_MAX] = {0};

	if (netdev->type != LXC_NET_VETH) {
		ERROR("Network type %d not support for unprivileged use", netdev->type);
		return -1;
	}

	ret = pipe(pipefd);
	if (ret < 0) {
		SYSERROR("Failed to create pipe");
		return -1;
	}

	child = fork();
	if (child < 0) {
		SYSERROR("Failed to create new process");
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	if (child == 0) {
		char *hostveth;

		close(pipefd[0]);

		ret = dup2(pipefd[1], STDOUT_FILENO);
		if (ret >= 0)
			ret = dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		if (ret < 0) {
			SYSERROR("Failed to duplicate std{err,out} file descriptor");
			_exit(EXIT_FAILURE);
		}

		if (netdev->priv.veth_attr.pair[0] != '\0')
			hostveth = netdev->priv.veth_attr.pair;
		else
			hostveth = netdev->priv.veth_attr.veth1;
		if (hostveth[0] == '\0') {
			SYSERROR("Host side veth device name is missing");
			_exit(EXIT_FAILURE);
		}

		if (netdev->link[0] == '\0') {
			SYSERROR("Network link for network device \"%s\" is "
				 "missing", netdev->priv.veth_attr.veth1);
			_exit(EXIT_FAILURE);
		}

		INFO("Execing lxc-user-nic delete %s %s %s veth %s %s", lxcpath,
		     lxcname, netns_path, netdev->link, hostveth);
		execlp(LXC_USERNIC_PATH, LXC_USERNIC_PATH, "delete", lxcpath,
		       lxcname, netns_path, "veth", netdev->link, hostveth,
		       (char *)NULL);
		SYSERROR("Failed to exec lxc-user-nic.");
		_exit(EXIT_FAILURE);
	}

	close(pipefd[1]);

	bytes = lxc_read_nointr(pipefd[0], &buffer, PATH_MAX);
	if (bytes < 0) {
		SYSERROR("Failed to read from pipe file descriptor.");
		close(pipefd[0]);
	} else {
		buffer[bytes - 1] = '\0';
	}

	ret = wait_for_pid(child);
	close(pipefd[0]);
	if (ret != 0 || bytes < 0) {
		ERROR("lxc-user-nic failed to delete requested network: %s",
		      buffer[0] != '\0' ? buffer : "(null)");
		return -1;
	}

	return 0;
}

bool lxc_delete_network_unpriv(struct lxc_handler *handler)
{
	int ret;
	struct lxc_list *iterator;
	struct lxc_list *network = &handler->conf->network;
	/* strlen("/proc/") = 6
	 * +
	 * INTTYPE_TO_STRLEN(pid_t)
	 * +
	 * strlen("/fd/") = 4
	 * +
	 * INTTYPE_TO_STRLEN(int)
	 * +
	 * \0
	 */
	char netns_path[6 + INTTYPE_TO_STRLEN(pid_t) + 4 + INTTYPE_TO_STRLEN(int) + 1];

	*netns_path = '\0';

	if (handler->nsfd[LXC_NS_NET] < 0) {
		DEBUG("Cannot not guarantee safe deletion of network devices. "
		      "Manual cleanup maybe needed");
		return false;
	}

	ret = snprintf(netns_path, sizeof(netns_path), "/proc/%d/fd/%d",
		       lxc_raw_getpid(), handler->nsfd[LXC_NS_NET]);
	if (ret < 0 || ret >= sizeof(netns_path))
		return false;

	lxc_list_for_each(iterator, network) {
		char *hostveth = NULL;
		struct lxc_netdev *netdev = iterator->elem;

		/* We can only delete devices whose ifindex we have. If we don't
		 * have the index it means that we didn't create it.
		 */
		if (!netdev->ifindex)
			continue;

		if (netdev->type == LXC_NET_PHYS) {
			ret = lxc_netdev_rename_by_index(netdev->ifindex,
							 netdev->link);
			if (ret < 0)
				WARN("Failed to rename interface with index %d "
				     "to its initial name \"%s\"",
				     netdev->ifindex, netdev->link);
			else
				TRACE("Renamed interface with index %d to its "
				      "initial name \"%s\"",
				      netdev->ifindex, netdev->link);

			ret = netdev_deconf[netdev->type](handler, netdev);
			goto clear_ifindices;
		}

		ret = netdev_deconf[netdev->type](handler, netdev);
		if (ret < 0)
			WARN("Failed to deconfigure network device");

		if (netdev->type != LXC_NET_VETH)
			goto clear_ifindices;

		if (netdev->link[0] == '\0' || !is_ovs_bridge(netdev->link))
			goto clear_ifindices;

		if (netdev->priv.veth_attr.pair[0] != '\0')
			hostveth = netdev->priv.veth_attr.pair;
		else
			hostveth = netdev->priv.veth_attr.veth1;
		if (hostveth[0] == '\0')
			goto clear_ifindices;

		ret = lxc_delete_network_unpriv_exec(handler->lxcpath,
						     handler->name, netdev,
						     netns_path);
		if (ret < 0) {
			WARN("Failed to remove port \"%s\" from openvswitch "
			     "bridge \"%s\"", hostveth, netdev->link);
			goto clear_ifindices;
		}
		INFO("Removed interface \"%s\" from \"%s\"", hostveth,
		     netdev->link);

clear_ifindices:
		/* We need to clear any ifindices we recorded so liblxc won't
		 * have cached stale data which would cause it to fail on reboot
		 * we're we don't re-read the on-disk config file.
		 */
		netdev->ifindex = 0;
		if (netdev->type == LXC_NET_PHYS) {
			netdev->priv.phys_attr.ifindex = 0;
		} else if (netdev->type == LXC_NET_VETH) {
			netdev->priv.veth_attr.veth1[0] = '\0';
			netdev->priv.veth_attr.ifindex = 0;
		}
	}

	return true;
}

static int lxc_setup_l2proxy(struct lxc_netdev *netdev) {
	struct lxc_list *cur, *next;
	struct lxc_inetdev *inet4dev;
	struct lxc_inet6dev *inet6dev;
	char bufinet4[INET_ADDRSTRLEN], bufinet6[INET6_ADDRSTRLEN];
	int err = 0;
	unsigned int lo_ifindex = 0;

	/* If IPv4 addresses are specified, then check that sysctl is configured correctly. */
	if (!lxc_list_empty(&netdev->ipv4)) {
		/* Check for net.ipv4.conf.[link].forwarding=1 */
		if (lxc_is_ip_forwarding_enabled(netdev->link, AF_INET) < 0) {
			ERROR("Requires sysctl net.ipv4.conf.%s.forwarding=1", netdev->link);
			return minus_one_set_errno(EINVAL);
		}
	}

	/* If IPv6 addresses are specified, then check that sysctl is configured correctly. */
	if (!lxc_list_empty(&netdev->ipv6)) {
		/* Check for net.ipv6.conf.[link].proxy_ndp=1 */
		if (lxc_is_ip_neigh_proxy_enabled(netdev->link, AF_INET6) < 0) {
			ERROR("Requires sysctl net.ipv6.conf.%s.proxy_ndp=1", netdev->link);
			return minus_one_set_errno(EINVAL);
		}

		/* Check for net.ipv6.conf.[link].forwarding=1 */
		if (lxc_is_ip_forwarding_enabled(netdev->link, AF_INET6) < 0) {
			ERROR("Requires sysctl net.ipv6.conf.%s.forwarding=1", netdev->link);
			return minus_one_set_errno(EINVAL);
		}
	}

	/* Perform IPVLAN specific checks. */
	if (netdev->type == LXC_NET_IPVLAN) {
		/* Check mode is l3s as other modes do not work with l2proxy. */
		if (netdev->priv.ipvlan_attr.mode != IPVLAN_MODE_L3S) {
			ERROR("Requires ipvlan mode on dev \"%s\" be l3s when used with l2proxy", netdev->link);
			return minus_one_set_errno(EINVAL);
		}

		/* Retrieve local-loopback interface index for use with IPVLAN static routes. */
		lo_ifindex = if_nametoindex(loop_device);
		if (lo_ifindex == 0) {
			ERROR("Failed to retrieve ifindex for \"%s\" routing cleanup", loop_device);
			return minus_one_set_errno(EINVAL);
		}
	}

	lxc_list_for_each_safe(cur, &netdev->ipv4, next) {
		inet4dev = cur->elem;
		if (!inet_ntop(AF_INET, &inet4dev->addr, bufinet4, sizeof(bufinet4)))
			return minus_one_set_errno(-errno);

		if (lxc_add_ip_neigh_proxy(bufinet4, netdev->link) < 0)
			return minus_one_set_errno(EINVAL);

		/* IPVLAN requires a route to local-loopback to trigger l2proxy. */
		if (netdev->type == LXC_NET_IPVLAN) {
			err = lxc_ipv4_dest_add(lo_ifindex, &inet4dev->addr, 32);
			if (err < 0) {
				ERROR("Failed to add ipv4 dest \"%s\" for network device \"%s\"", bufinet4, loop_device);
				return minus_one_set_errno(-err);
			}
		}
	}

	lxc_list_for_each_safe(cur, &netdev->ipv6, next) {
		inet6dev = cur->elem;
		if (!inet_ntop(AF_INET6, &inet6dev->addr, bufinet6, sizeof(bufinet6)))
			return minus_one_set_errno(-errno);

		if (lxc_add_ip_neigh_proxy(bufinet6, netdev->link) < 0)
			return minus_one_set_errno(EINVAL);

		/* IPVLAN requires a route to local-loopback to trigger l2proxy. */
		if (netdev->type == LXC_NET_IPVLAN) {
			err = lxc_ipv6_dest_add(lo_ifindex, &inet6dev->addr, 128);
			if (err < 0) {
				ERROR("Failed to add ipv6 dest \"%s\" for network device \"%s\"", bufinet6, loop_device);
				return minus_one_set_errno(-err);
			}
		}
	}

	return 0;
}

static int lxc_delete_ipv4_l2proxy(struct in_addr *ip, char *link, unsigned int lo_ifindex) {
	char bufinet4[INET_ADDRSTRLEN];
	unsigned int errCount = 0;

	if (!inet_ntop(AF_INET, ip, bufinet4, sizeof(bufinet4))) {
		SYSERROR("Failed to convert IP for l2proxy ipv4 removal on dev \"%s\"", link);
		return minus_one_set_errno(EINVAL);
	}

	/* If a local-loopback ifindex supplied remove the static route to the lo device. */
	if (lo_ifindex > 0) {
		if (lxc_ipv4_dest_del(lo_ifindex, ip, 32) < 0) {
			errCount++;
			ERROR("Failed to delete ipv4 dest \"%s\" for network ifindex \"%u\"", bufinet4, lo_ifindex);
		}
	}

	/* If link is supplied remove the IP neigh proxy entry for this IP on the device. */
	if (link[0] != '\0') {
		if (lxc_del_ip_neigh_proxy(bufinet4, link) < 0)
			errCount++;
	}

	if (errCount > 0)
		return minus_one_set_errno(EINVAL);

	return 0;
}

static int lxc_delete_ipv6_l2proxy(struct in6_addr *ip, char *link, unsigned int lo_ifindex) {
	char bufinet6[INET6_ADDRSTRLEN];
	unsigned int errCount = 0;

	if (!inet_ntop(AF_INET6, ip, bufinet6, sizeof(bufinet6))) {
		SYSERROR("Failed to convert IP for l2proxy ipv6 removal on dev \"%s\"", link);
		return minus_one_set_errno(EINVAL);
	}

	/* If a local-loopback ifindex supplied remove the static route to the lo device. */
	if (lo_ifindex > 0) {
		if (lxc_ipv6_dest_del(lo_ifindex, ip, 128) < 0) {
			errCount++;
			ERROR("Failed to delete ipv6 dest \"%s\" for network ifindex \"%u\"", bufinet6, lo_ifindex);
		}
	}

	/* If link is supplied remove the IP neigh proxy entry for this IP on the device. */
	if (link[0] != '\0') {
		if (lxc_del_ip_neigh_proxy(bufinet6, link) < 0)
			errCount++;
	}

	if (errCount > 0)
		return minus_one_set_errno(EINVAL);

	return 0;
}

static int lxc_delete_l2proxy(struct lxc_netdev *netdev) {
	unsigned int lo_ifindex = 0;
	unsigned int errCount = 0;
	struct lxc_list *cur, *next;
	struct lxc_inetdev *inet4dev;
	struct lxc_inet6dev *inet6dev;

	/* Perform IPVLAN specific checks. */
	if (netdev->type == LXC_NET_IPVLAN) {
		/* Retrieve local-loopback interface index for use with IPVLAN static routes. */
		lo_ifindex = if_nametoindex(loop_device);
		if (lo_ifindex == 0) {
			errCount++;
			ERROR("Failed to retrieve ifindex for \"%s\" routing cleanup", loop_device);
		}
	}

	lxc_list_for_each_safe(cur, &netdev->ipv4, next) {
		inet4dev = cur->elem;
		if (lxc_delete_ipv4_l2proxy(&inet4dev->addr, netdev->link, lo_ifindex) < 0)
			errCount++;
	}

	lxc_list_for_each_safe(cur, &netdev->ipv6, next) {
		inet6dev = cur->elem;
		if (lxc_delete_ipv6_l2proxy(&inet6dev->addr, netdev->link, lo_ifindex) < 0)
			errCount++;
	}

	if (errCount > 0)
		return minus_one_set_errno(EINVAL);

	return 0;
}

static int lxc_create_network_priv(struct lxc_handler *handler)
{
	struct lxc_list *iterator;
	struct lxc_list *network = &handler->conf->network;

	lxc_list_for_each(iterator, network) {
		struct lxc_netdev *netdev = iterator->elem;

		if (netdev->type < 0 || netdev->type > LXC_NET_MAXCONFTYPE) {
			ERROR("Invalid network configuration type %d", netdev->type);
			return -1;
		}

		/* Setup l2proxy entries if enabled and used with a link property */
		if (netdev->l2proxy && netdev->link[0] != '\0') {
			if (lxc_setup_l2proxy(netdev)) {
				ERROR("Failed to setup l2proxy");
				return -1;
			}
		}

		if (netdev_conf[netdev->type](handler, netdev)) {
			ERROR("Failed to create network device");
			return -1;
		}
	}

	return 0;
}

int lxc_network_move_created_netdev_priv(struct lxc_handler *handler)
{
	pid_t pid = handler->pid;
	struct lxc_list *network = &handler->conf->network;
	struct lxc_list *iterator;

	if (am_guest_unpriv())
		return 0;

	lxc_list_for_each(iterator, network) {
		int ret;
		struct lxc_netdev *netdev = iterator->elem;

		if (!netdev->ifindex)
			continue;

		ret = lxc_netdev_move_by_index(netdev->ifindex, pid, NULL);
		if (ret) {
			errno = -ret;
			SYSERROR("Failed to move network device \"%s\" with ifindex %d to network namespace %d",
				 netdev->created_name, netdev->ifindex, pid);
			return -1;
		}

		DEBUG("Moved network device \"%s\" with ifindex %d to network namespace of %d",
		      netdev->created_name, netdev->ifindex, pid);
	}

	return 0;
}

static int network_requires_advanced_setup(int type)
{
	if (type == LXC_NET_EMPTY)
		return false;

	if (type == LXC_NET_NONE)
		return false;

	return true;
}

static int lxc_create_network_unpriv(struct lxc_handler *handler)
{
	int hooks_version = handler->conf->hooks_version;
	const char *lxcname = handler->name;
	const char *lxcpath = handler->lxcpath;
	struct lxc_list *network = &handler->conf->network;
	pid_t pid = handler->pid;
	struct lxc_list *iterator;

	lxc_list_for_each(iterator, network) {
		struct lxc_netdev *netdev = iterator->elem;

		if (!network_requires_advanced_setup(netdev->type))
			continue;

		if (netdev->type != LXC_NET_VETH) {
			ERROR("Networks of type %s are not supported by unprivileged containers",
			      lxc_net_type_to_str(netdev->type));
			return -1;
		}

		if (netdev->mtu)
			INFO("mtu ignored due to insufficient privilege");

		if (lxc_create_network_unpriv_exec(lxcpath, lxcname, netdev,
						   pid, hooks_version))
			return -1;
	}

	return 0;
}

bool lxc_delete_network_priv(struct lxc_handler *handler)
{
	int ret;
	struct lxc_list *iterator;
	struct lxc_list *network = &handler->conf->network;

	lxc_list_for_each(iterator, network) {
		char *hostveth = NULL;
		struct lxc_netdev *netdev = iterator->elem;

		/* We can only delete devices whose ifindex we have. If we don't
		 * have the index it means that we didn't create it.
		 */
		if (!netdev->ifindex)
			continue;

		/* Delete l2proxy entries if enabled and used with a link property */
		if (netdev->l2proxy && netdev->link[0] != '\0') {
			if (lxc_delete_l2proxy(netdev))
				WARN("Failed to delete all l2proxy config");
				/* Don't return, let the network be cleaned up as normal. */
		}

		if (netdev->type == LXC_NET_PHYS) {
			ret = lxc_netdev_rename_by_index(netdev->ifindex, netdev->link);
			if (ret < 0)
				WARN("Failed to rename interface with index %d "
				     "from \"%s\" to its initial name \"%s\"",
				     netdev->ifindex, netdev->name, netdev->link);
			else {
				TRACE("Renamed interface with index %d from "
				      "\"%s\" to its initial name \"%s\"",
				      netdev->ifindex, netdev->name,
				      netdev->link);

				/* Restore original MTU */
				ret = lxc_netdev_set_mtu(netdev->link, netdev->priv.phys_attr.mtu);
				if (ret < 0) {
					WARN("Failed to set interface \"%s\" to its initial mtu \"%d\"",
						netdev->link, netdev->priv.phys_attr.mtu);
				} else {
					TRACE("Restored interface \"%s\" to its initial mtu \"%d\"",
						netdev->link, netdev->priv.phys_attr.mtu);
				}
			}

			ret = netdev_deconf[netdev->type](handler, netdev);
			goto clear_ifindices;
		}

		ret = netdev_deconf[netdev->type](handler, netdev);
		if (ret < 0)
			WARN("Failed to deconfigure network device");

		/* Recent kernels remove the virtual interfaces when the network
		 * namespace is destroyed but in case we did not move the
		 * interface to the network namespace, we have to destroy it.
		 */
		ret = lxc_netdev_delete_by_index(netdev->ifindex);
		if (ret < 0) {
			if (errno != ENODEV) {
				WARN("Failed to remove interface \"%s\" with index %d",
				     netdev->name[0] != '\0' ? netdev->name : "(null)",
				     netdev->ifindex);
				goto clear_ifindices;
			}
			INFO("Interface \"%s\" with index %d already deleted or existing in different network namespace",
			     netdev->name[0] != '\0' ? netdev->name : "(null)",
			     netdev->ifindex);
		}
		INFO("Removed interface \"%s\" with index %d",
		     netdev->name[0] != '\0' ? netdev->name : "(null)",
		     netdev->ifindex);

		if (netdev->type != LXC_NET_VETH)
			goto clear_ifindices;

		/* Explicitly delete host veth device to prevent lingering
		 * devices. We had issues in LXD around this.
		 */
		if (netdev->priv.veth_attr.pair[0] != '\0')
			hostveth = netdev->priv.veth_attr.pair;
		else
			hostveth = netdev->priv.veth_attr.veth1;
		if (hostveth[0] == '\0')
			goto clear_ifindices;

		ret = lxc_netdev_delete_by_name(hostveth);
		if (ret < 0) {
			WARN("Failed to remove interface \"%s\" from \"%s\"",
			     hostveth, netdev->link);
			goto clear_ifindices;
		}
		INFO("Removed interface \"%s\" from \"%s\"", hostveth, netdev->link);

		if (netdev->link[0] == '\0' || !is_ovs_bridge(netdev->link)) {
			netdev->priv.veth_attr.veth1[0] = '\0';
			netdev->ifindex = 0;
			netdev->priv.veth_attr.ifindex = 0;
			goto clear_ifindices;
		}

		/* Delete the openvswitch port. */
		ret = lxc_ovs_delete_port(netdev->link, hostveth);
		if (ret < 0)
			WARN("Failed to remove port \"%s\" from openvswitch "
			     "bridge \"%s\"", hostveth, netdev->link);
		else
			INFO("Removed port \"%s\" from openvswitch bridge \"%s\"",
			     hostveth, netdev->link);

clear_ifindices:
		/* We need to clear any ifindices we recorded so liblxc won't
		 * have cached stale data which would cause it to fail on reboot
		 * we're we don't re-read the on-disk config file.
		 */
		netdev->ifindex = 0;
		if (netdev->type == LXC_NET_PHYS) {
			netdev->priv.phys_attr.ifindex = 0;
		} else if (netdev->type == LXC_NET_VETH) {
			netdev->priv.veth_attr.veth1[0] = '\0';
			netdev->priv.veth_attr.ifindex = 0;
		}
	}

	return true;
}

int lxc_requests_empty_network(struct lxc_handler *handler)
{
	struct lxc_list *network = &handler->conf->network;
	struct lxc_list *iterator;
	bool found_none = false, found_nic = false;

	if (lxc_list_empty(network))
		return 0;

	lxc_list_for_each(iterator, network) {
		struct lxc_netdev *netdev = iterator->elem;

		if (netdev->type == LXC_NET_NONE)
			found_none = true;
		else
			found_nic = true;
	}
	if (found_none && !found_nic)
		return 1;
	return 0;
}

/* try to move physical nics to the init netns */
int lxc_restore_phys_nics_to_netns(struct lxc_handler *handler)
{
	int ret;
	int oldfd;
	char ifname[IFNAMSIZ];
	struct lxc_list *iterator;
	int netnsfd = handler->nsfd[LXC_NS_NET];
	struct lxc_conf *conf = handler->conf;

	/* We need CAP_NET_ADMIN in the parent namespace in order to setns() to
	 * the parent network namespace. We won't have this capability if we are
	 * unprivileged.
	 */
	if (!handler->am_root)
		return 0;

	TRACE("Moving physical network devices back to parent network namespace");

	oldfd = lxc_preserve_ns(handler->monitor_pid, "net");
	if (oldfd < 0) {
		SYSERROR("Failed to preserve network namespace");
		return -1;
	}

	ret = setns(netnsfd, CLONE_NEWNET);
	if (ret < 0) {
		SYSERROR("Failed to enter network namespace");
		close(oldfd);
		return -1;
	}

	lxc_list_for_each(iterator, &conf->network) {
		struct lxc_netdev *netdev = iterator->elem;

		if (netdev->type != LXC_NET_PHYS)
			continue;

		/* Retrieve the name of the interface in the container's network
		 * namespace.
		 */
		if (!if_indextoname(netdev->ifindex, ifname)) {
			WARN("No interface corresponding to ifindex %d",
			     netdev->ifindex);
			continue;
		}

		ret = lxc_netdev_move_by_index_fd(netdev->ifindex, oldfd, netdev->link);
		if (ret < 0)
			WARN("Error moving network device \"%s\" back to "
			     "network namespace", ifname);
		else
			TRACE("Moved network device \"%s\" back to network "
			      "namespace", ifname);
	}

	ret = setns(oldfd, CLONE_NEWNET);
	close(oldfd);
	if (ret < 0) {
		SYSERROR("Failed to enter network namespace");
		return -1;
	}

	return 0;
}

static int setup_hw_addr(char *hwaddr, const char *ifname)
{
	struct sockaddr sockaddr;
	struct ifreq ifr;
	int ret, fd;

	ret = lxc_convert_mac(hwaddr, &sockaddr);
	if (ret) {
		errno = -ret;
		SYSERROR("Mac address \"%s\" conversion failed", hwaddr);
		return -1;
	}

	memcpy(ifr.ifr_name, ifname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ-1] = '\0';
	memcpy((char *) &ifr.ifr_hwaddr, (char *) &sockaddr, sizeof(sockaddr));

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	ret = ioctl(fd, SIOCSIFHWADDR, &ifr);
	if (ret)
		SYSERROR("Failed to perform ioctl");

	close(fd);

	DEBUG("Mac address \"%s\" on \"%s\" has been setup", hwaddr,
	      ifr.ifr_name);

	return ret;
}

static int setup_ipv4_addr(struct lxc_list *ip, int ifindex)
{
	struct lxc_list *iterator;
	int err;

	lxc_list_for_each(iterator, ip) {
		struct lxc_inetdev *inetdev = iterator->elem;

		err = lxc_ipv4_addr_add(ifindex, &inetdev->addr,
					&inetdev->bcast, inetdev->prefix);
		if (err) {
			errno = -err;
			SYSERROR("Failed to setup ipv4 address for network device "
			         "with ifindex %d", ifindex);
			return -1;
		}
	}

	return 0;
}

static int setup_ipv6_addr(struct lxc_list *ip, int ifindex)
{
	struct lxc_list *iterator;
	int err;

	lxc_list_for_each(iterator, ip) {
		struct lxc_inet6dev *inet6dev = iterator->elem;

		err = lxc_ipv6_addr_add(ifindex, &inet6dev->addr,
					&inet6dev->mcast, &inet6dev->acast,
					inet6dev->prefix);
		if (err) {
			errno = -err;
			SYSERROR("Failed to setup ipv6 address for network device "
			         "with ifindex %d", ifindex);
			return -1;
		}
	}

	return 0;
}

static int lxc_setup_netdev_in_child_namespaces(struct lxc_netdev *netdev)
{
	char ifname[IFNAMSIZ];
	int err;
	char *current_ifname = ifname;
	char bufinet4[INET_ADDRSTRLEN], bufinet6[INET6_ADDRSTRLEN];

	/* empty network namespace */
	if (!netdev->ifindex) {
		if (netdev->flags & IFF_UP) {
			err = lxc_netdev_up("lo");
			if (err) {
				errno = -err;
				SYSERROR("Failed to set the loopback network device up");
				return -1;
			}
		}

		if (netdev->type == LXC_NET_EMPTY)
			return 0;

		if (netdev->type == LXC_NET_NONE)
			return 0;

		netdev->ifindex = if_nametoindex(netdev->created_name);
		if (!netdev->ifindex)
			SYSERROR("Failed to retrieve ifindex for network device with name %s",
				 netdev->created_name ?: "(null)");
	}

	/* get the new ifindex in case of physical netdev */
	if (netdev->type == LXC_NET_PHYS) {
		netdev->ifindex = if_nametoindex(netdev->link);
		if (!netdev->ifindex) {
			ERROR("Failed to get ifindex for network device \"%s\"",
			      netdev->link);
			return -1;
		}
	}

	/* retrieve the name of the interface */
	if (!if_indextoname(netdev->ifindex, current_ifname)) {
		SYSERROR("Failed to retrieve name for network device with ifindex %d",
			 netdev->ifindex);
		return -1;
	}

	/* Default: let the system choose an interface name.
	 * When the IFLA_IFNAME attribute is passed something like "<prefix>%d"
	 * netlink will replace the format specifier with an appropriate index.
	 */
	if (netdev->name[0] == '\0') {
		if (netdev->type == LXC_NET_PHYS)
			(void)strlcpy(netdev->name, netdev->link, IFNAMSIZ);
		else
			(void)strlcpy(netdev->name, "eth%d", IFNAMSIZ);
	}

	/* rename the interface name */
	if (strcmp(current_ifname, netdev->name) != 0) {
		err = lxc_netdev_rename_by_name(current_ifname, netdev->name);
		if (err) {
			errno = -err;
			SYSERROR("Failed to rename network device \"%s\" to \"%s\"",
			         current_ifname, netdev->name);
			return -1;
		}

		TRACE("Renamed network device from \"%s\" to \"%s\"",
		      current_ifname, netdev->name);
	}

	/* Re-read the name of the interface because its name has changed
	 * and would be automatically allocated by the system
	 */
	if (!if_indextoname(netdev->ifindex, current_ifname)) {
		ERROR("Failed get name for network device with ifindex %d",
		      netdev->ifindex);
		return -1;
	}

	/* Now update the recorded name of the network device to reflect the
	 * name of the network device in the child's network namespace. We will
	 * later on send this information back to the parent.
	 */
	(void)strlcpy(netdev->name, current_ifname, IFNAMSIZ);

	/* set a mac address */
	if (netdev->hwaddr) {
		if (setup_hw_addr(netdev->hwaddr, current_ifname)) {
			ERROR("Failed to setup hw address for network device \"%s\"",
			      current_ifname);
			return -1;
		}
	}

	/* setup ipv4 addresses on the interface */
	if (setup_ipv4_addr(&netdev->ipv4, netdev->ifindex)) {
		ERROR("Failed to setup ip addresses for network device \"%s\"",
		      current_ifname);
		return -1;
	}

	/* setup ipv6 addresses on the interface */
	if (setup_ipv6_addr(&netdev->ipv6, netdev->ifindex)) {
		ERROR("Failed to setup ipv6 addresses for network device \"%s\"",
		      current_ifname);
		return -1;
	}

	/* set the network device up */
	if (netdev->flags & IFF_UP) {
		err = lxc_netdev_up(current_ifname);
		if (err) {
			errno = -err;
			SYSERROR("Failed to set network device \"%s\" up",
			         current_ifname);
			return -1;
		}

		/* the network is up, make the loopback up too */
		err = lxc_netdev_up("lo");
		if (err) {
			errno = -err;
			SYSERROR("Failed to set the loopback network device up");
			return -1;
		}
	}

	/* setup ipv4 gateway on the interface */
	if (netdev->ipv4_gateway || netdev->ipv4_gateway_dev) {
		if (!(netdev->flags & IFF_UP)) {
			ERROR("Cannot add ipv4 gateway for network device "
			      "\"%s\" when not bringing up the interface", current_ifname);
			return -1;
		}

		if (lxc_list_empty(&netdev->ipv4)) {
			ERROR("Cannot add ipv4 gateway for network device "
			      "\"%s\" when not assigning an address", current_ifname);
			return -1;
		}

		/* Setup device route if ipv4_gateway_dev is enabled */
		if (netdev->ipv4_gateway_dev) {
			err = lxc_ipv4_gateway_add(netdev->ifindex, NULL);
			if (err < 0) {
				SYSERROR("Failed to setup ipv4 gateway to network device \"%s\"",
				         current_ifname);
				return minus_one_set_errno(-err);
			}
		} else {
			/* Check the gateway address is valid */
			if (!inet_ntop(AF_INET, netdev->ipv4_gateway, bufinet4, sizeof(bufinet4)))
				return minus_one_set_errno(errno);

			/* Try adding a default route to the gateway address */
			err = lxc_ipv4_gateway_add(netdev->ifindex, netdev->ipv4_gateway);
			if (err < 0) {
				/* If adding the default route fails, this could be because the
				 * gateway address is in a different subnet to the container's address.
				 * To work around this, we try adding a static device route to the
				 * gateway address first, and then try again.
				 */
				err = lxc_ipv4_dest_add(netdev->ifindex, netdev->ipv4_gateway, 32);
				if (err < 0) {
					errno = -err;
					SYSERROR("Failed to add ipv4 dest \"%s\" for network device \"%s\"",
						bufinet4, current_ifname);
					return -1;
				}

				err = lxc_ipv4_gateway_add(netdev->ifindex, netdev->ipv4_gateway);
				if (err < 0) {
					errno = -err;
					SYSERROR("Failed to setup ipv4 gateway \"%s\" for network device \"%s\"",
						bufinet4, current_ifname);
					return -1;
				}
			}
		}
	}

	/* setup ipv6 gateway on the interface */
	if (netdev->ipv6_gateway || netdev->ipv6_gateway_dev) {
		if (!(netdev->flags & IFF_UP)) {
			ERROR("Cannot add ipv6 gateway for network device \"%s\" when not bringing up the interface",
			      current_ifname);
			return -1;
		}

		if (lxc_list_empty(&netdev->ipv6) && !IN6_IS_ADDR_LINKLOCAL(netdev->ipv6_gateway)) {
			ERROR("Cannot add ipv6 gateway for network device \"%s\" when not assigning an address",
			      current_ifname);
			return -1;
		}

		/* Setup device route if ipv6_gateway_dev is enabled */
		if (netdev->ipv6_gateway_dev) {
			err = lxc_ipv6_gateway_add(netdev->ifindex, NULL);
			if (err < 0) {
				SYSERROR("Failed to setup ipv6 gateway to network device \"%s\"",
				         current_ifname);
				return minus_one_set_errno(-err);
			}
		} else {
			/* Check the gateway address is valid */
			if (!inet_ntop(AF_INET6, netdev->ipv6_gateway, bufinet6, sizeof(bufinet6)))
				return minus_one_set_errno(errno);

			/* Try adding a default route to the gateway address */
			err = lxc_ipv6_gateway_add(netdev->ifindex, netdev->ipv6_gateway);
			if (err < 0) {
				/* If adding the default route fails, this could be because the
				 * gateway address is in a different subnet to the container's address.
				 * To work around this, we try adding a static device route to the
				 * gateway address first, and then try again.
				 */
				err = lxc_ipv6_dest_add(netdev->ifindex, netdev->ipv6_gateway, 128);
				if (err < 0) {
					errno = -err;
					SYSERROR("Failed to add ipv6 dest \"%s\" for network device \"%s\"",
						bufinet6, current_ifname);
					return -1;
				}

				err = lxc_ipv6_gateway_add(netdev->ifindex, netdev->ipv6_gateway);
				if (err < 0) {
					errno = -err;
					SYSERROR("Failed to setup ipv6 gateway \"%s\" for network device \"%s\"",
						bufinet6, current_ifname);
					return -1;
				}
			}
		}
	}

	DEBUG("Network device \"%s\" has been setup", current_ifname);

	return 0;
}

int lxc_setup_network_in_child_namespaces(const struct lxc_conf *conf,
					  struct lxc_list *network)
{
	struct lxc_list *iterator;

	lxc_list_for_each(iterator, network) {
		struct lxc_netdev *netdev = iterator->elem;

		if (lxc_setup_netdev_in_child_namespaces(netdev)) {
			ERROR("Failed to setup netdev");
			return -1;
		}
	}

	if (!lxc_list_empty(network))
		INFO("Network has been setup");

	return 0;
}

int lxc_network_send_to_child(struct lxc_handler *handler)
{
	struct lxc_list *iterator;
	struct lxc_list *network = &handler->conf->network;
	int data_sock = handler->data_sock[0];

	lxc_list_for_each(iterator, network) {
		int ret;
		struct lxc_netdev *netdev = iterator->elem;

		if (!network_requires_advanced_setup(netdev->type))
			continue;

		ret = lxc_send_nointr(data_sock, netdev->name, IFNAMSIZ, MSG_NOSIGNAL);
		if (ret < 0)
			return -1;

		ret = lxc_send_nointr(data_sock, netdev->created_name, IFNAMSIZ, MSG_NOSIGNAL);
		if (ret < 0)
			return -1;

		TRACE("Sent network device name \"%s\" to child", netdev->created_name);
	}

	return 0;
}

int lxc_network_recv_from_parent(struct lxc_handler *handler)
{
	struct lxc_list *iterator;
	struct lxc_list *network = &handler->conf->network;
	int data_sock = handler->data_sock[1];

	lxc_list_for_each(iterator, network) {
		int ret;
		struct lxc_netdev *netdev = iterator->elem;

		if (!network_requires_advanced_setup(netdev->type))
			continue;

		ret = lxc_recv_nointr(data_sock, netdev->name, IFNAMSIZ, 0);
		if (ret < 0)
			return -1;

		ret = lxc_recv_nointr(data_sock, netdev->created_name, IFNAMSIZ, 0);
		if (ret < 0)
			return -1;
		TRACE("Received network device name \"%s\" from parent", netdev->created_name);
	}

	return 0;
}

int lxc_network_send_name_and_ifindex_to_parent(struct lxc_handler *handler)
{
	struct lxc_list *iterator, *network;
	int data_sock = handler->data_sock[0];

	if (!handler->am_root)
		return 0;

	network = &handler->conf->network;
	lxc_list_for_each(iterator, network) {
		int ret;
		struct lxc_netdev *netdev = iterator->elem;

		/* Send network device name in the child's namespace to parent. */
		ret = lxc_send_nointr(data_sock, netdev->name, IFNAMSIZ, MSG_NOSIGNAL);
		if (ret < 0)
			return -1;

		/* Send network device ifindex in the child's namespace to
		 * parent.
		 */
		ret = lxc_send_nointr(data_sock, &netdev->ifindex, sizeof(netdev->ifindex), MSG_NOSIGNAL);
		if (ret < 0)
			return -1;
	}

	if (!lxc_list_empty(network))
		TRACE("Sent network device names and ifindices to parent");

	return 0;
}

int lxc_network_recv_name_and_ifindex_from_child(struct lxc_handler *handler)
{
	struct lxc_list *iterator, *network;
	int data_sock = handler->data_sock[1];

	if (!handler->am_root)
		return 0;

	network = &handler->conf->network;
	lxc_list_for_each(iterator, network) {
		int ret;
		struct lxc_netdev *netdev = iterator->elem;

		/* Receive network device name in the child's namespace to
		 * parent.
		 */
		ret = lxc_recv_nointr(data_sock, netdev->name, IFNAMSIZ, 0);
		if (ret < 0)
			return -1;

		/* Receive network device ifindex in the child's namespace to
		 * parent.
		 */
		ret = lxc_recv_nointr(data_sock, &netdev->ifindex, sizeof(netdev->ifindex), 0);
		if (ret < 0)
			return -1;
	}

	return 0;
}

void lxc_delete_network(struct lxc_handler *handler)
{
	bool bret;

	if (handler->am_root)
		bret = lxc_delete_network_priv(handler);
	else
		bret = lxc_delete_network_unpriv(handler);
	if (!bret)
		DEBUG("Failed to delete network devices");
	else
		DEBUG("Deleted network devices");
}

int lxc_netns_set_nsid(int fd)
{
	int ret;
	char buf[NLMSG_ALIGN(sizeof(struct nlmsghdr)) +
		 NLMSG_ALIGN(sizeof(struct rtgenmsg)) +
		 NLMSG_ALIGN(1024)];
	struct nl_handler nlh;
	struct nlmsghdr *hdr;
	struct rtgenmsg *msg;
	int saved_errno;
	const __s32 ns_id = -1;
	const __u32 netns_fd = fd;

	ret = netlink_open(&nlh, NETLINK_ROUTE);
	if (ret < 0)
		return -1;

	memset(buf, 0, sizeof(buf));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
	hdr = (struct nlmsghdr *)buf;
	msg = (struct rtgenmsg *)NLMSG_DATA(hdr);
#pragma GCC diagnostic pop

	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(*msg));
	hdr->nlmsg_type = RTM_NEWNSID;
	hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	hdr->nlmsg_pid = 0;
	hdr->nlmsg_seq = RTM_NEWNSID;
	msg->rtgen_family = AF_UNSPEC;

	ret = addattr(hdr, 1024, __LXC_NETNSA_FD, &netns_fd, sizeof(netns_fd));
	if (ret < 0)
		goto on_error;

	ret = addattr(hdr, 1024, __LXC_NETNSA_NSID, &ns_id, sizeof(ns_id));
	if (ret < 0)
		goto on_error;

	ret = __netlink_transaction(&nlh, hdr, hdr);

on_error:
	saved_errno = errno;
	netlink_close(&nlh);
	errno = saved_errno;

	return ret;
}

static int parse_rtattr(struct rtattr *tb[], int max, struct rtattr *rta, int len)
{

	memset(tb, 0, sizeof(struct rtattr *) * (max + 1));

	while (RTA_OK(rta, len)) {
		unsigned short type = rta->rta_type;

		if ((type <= max) && (!tb[type]))
			tb[type] = rta;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
		rta = RTA_NEXT(rta, len);
#pragma GCC diagnostic pop
	}

	return 0;
}

static inline __s32 rta_getattr_s32(const struct rtattr *rta)
{
	return *(__s32 *)RTA_DATA(rta);
}

#ifndef NETNS_RTA
#define NETNS_RTA(r) \
	((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct rtgenmsg))))
#endif

int lxc_netns_get_nsid(int fd)
{
	int ret;
	ssize_t len;
	char buf[NLMSG_ALIGN(sizeof(struct nlmsghdr)) +
		 NLMSG_ALIGN(sizeof(struct rtgenmsg)) +
		 NLMSG_ALIGN(1024)];
	struct rtattr *tb[__LXC_NETNSA_MAX + 1];
	struct nl_handler nlh;
	struct nlmsghdr *hdr;
	struct rtgenmsg *msg;
	int saved_errno;
	__u32 netns_fd = fd;

	ret = netlink_open(&nlh, NETLINK_ROUTE);
	if (ret < 0)
		return -1;

	memset(buf, 0, sizeof(buf));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
	hdr = (struct nlmsghdr *)buf;
	msg = (struct rtgenmsg *)NLMSG_DATA(hdr);
#pragma GCC diagnostic pop

	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(*msg));
	hdr->nlmsg_type = RTM_GETNSID;
	hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	hdr->nlmsg_pid = 0;
	hdr->nlmsg_seq = RTM_GETNSID;
	msg->rtgen_family = AF_UNSPEC;

	ret = addattr(hdr, 1024, __LXC_NETNSA_FD, &netns_fd, sizeof(netns_fd));
	if (ret == 0)
		ret = __netlink_transaction(&nlh, hdr, hdr);

	saved_errno = errno;
	netlink_close(&nlh);
	errno = saved_errno;
	if (ret < 0)
		return -1;

	errno = EINVAL;
	msg = NLMSG_DATA(hdr);
	len = hdr->nlmsg_len - NLMSG_SPACE(sizeof(*msg));
	if (len < 0)
		return -1;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
	parse_rtattr(tb, __LXC_NETNSA_MAX, NETNS_RTA(msg), len);
	if (tb[__LXC_NETNSA_NSID])
		return rta_getattr_s32(tb[__LXC_NETNSA_NSID]);
#pragma GCC diagnostic pop

	return -1;
}

int lxc_create_network(struct lxc_handler *handler)
{
	int ret;

	if (handler->am_root) {
		ret = lxc_create_network_priv(handler);
		if (ret)
			return -1;

		return lxc_network_move_created_netdev_priv(handler);
	}

	return lxc_create_network_unpriv(handler);
}

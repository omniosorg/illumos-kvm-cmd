/*
 * QEMU System Emulator
 * Solaris VNIC support
 *
 * Copyright 2016 Joyent, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <libdlpi.h>
#include <string.h>
#include <stdio.h>
#include <stropts.h>
#include <stdlib.h>
#include <unistd.h>

#include <netpacket/packet.h>
#include <assert.h>
#include <net/if_dl.h>
#include <sys/ethernet.h>
#include <sys/dlpi.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "net/vnic.h"
#include "net/vnic-dhcp.h"

#include "qemu-common.h"
#include "qemu-error.h"
#include "qemu-option.h"
#include "qemu-char.h"

/*
 * XXX We should determine a good way to get this buffer size. 64k feels like
 * such an arbitrary number...
 */
#define	VNIC_BUFSIZE	65536

typedef struct VNICState {
	VLANClientState	vns_nc;
	int		vns_fd;
	unsigned int	vns_rpoll;
	unsigned int	vns_wpoll;
	uint8_t		vns_buf[VNIC_BUFSIZE];
	uint8_t		vns_txbuf[VNIC_BUFSIZE];
	uint_t		vns_sap;
	dlpi_handle_t	vns_hdl;
	VNICDHCPState	vns_ds;
} VNICState;

static void vnic_update_fd_handler(VNICState *);

static void
vnic_read_poll(VNICState *vsp, int enable)
{
	vsp->vns_rpoll = enable;
	vnic_update_fd_handler(vsp);
}

static void
vnic_write_poll(VNICState *vsp, int enable)
{
	vsp->vns_wpoll = enable;
	vnic_update_fd_handler(vsp);
}

static void
vnic_poll(VLANClientState *ncp, bool enable)
{
	VNICState *vsp = DO_UPCAST(VNICState, vns_nc, ncp);
	vnic_read_poll(vsp, 1);
	vnic_write_poll(vsp, 1);
}

static int
vnic_read_packet(VNICState *vsp, uint8_t *buf, int len)
{
	struct strbuf sbuf;
	int flags, ret;

	flags = 0;
	sbuf.maxlen = len;
	sbuf.buf = (char *)buf;

	do {
		ret = getmsg(vsp->vns_fd, NULL, &sbuf, &flags);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1 && errno == EAGAIN) {
		vnic_write_poll(vsp, 1);
		return (0);
	}

	if (ret == -1) {
		return (-1);
	}

	return (sbuf.len);
}

static int
vnic_write_packet(VNICState *vsp, const uint8_t *buf, int len)
{
	struct strbuf sbuf;
	int flags, ret;

	flags = 0;
	sbuf.len = len;
	sbuf.buf = (char *)buf;

	do {
		ret = putmsg(vsp->vns_fd, NULL, &sbuf, flags);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1 && errno == EAGAIN) {
		vnic_write_poll(vsp, 1);
		return (0);
	}

	if (ret == -1)
		return (-1);

	return (len);
}

static int
vnic_can_send(void *opaque)
{
	VNICState *vsp = opaque;
	return (qemu_can_send_packet(&vsp->vns_nc));
}

static void
vnic_send_completed(VLANClientState *nc, ssize_t len)
{
	VNICState *vsp = DO_UPCAST(VNICState, vns_nc, nc);
	vnic_read_poll(vsp, 1);
}

/* outside world -> VM */
static void
vnic_send(void *opaque)
{
	VNICState *vsp = opaque;
	int ret;

	do {
		ret = vnic_read_packet(vsp, vsp->vns_buf,
		    sizeof (vsp->vns_buf));
		if (ret <= 0)
			break;

		ret = qemu_send_packet_async(&vsp->vns_nc, vsp->vns_buf, ret,
		    vnic_send_completed);

		if (ret == 0)
			vnic_read_poll(vsp, 0);

	} while (ret > 0 && qemu_can_send_packet(&vsp->vns_nc));
}

static void
vnic_writable(void *opaque)
{
	VNICState *vsp = opaque;
	vnic_write_poll(vsp, 0);
	qemu_flush_queued_packets(&vsp->vns_nc);
}

/* VM -> outside world */
static ssize_t
vnic_receive(VLANClientState *ncp, const uint8_t *buf, size_t size)
{
	uint16_t ethtype;
	VNICState *vsp = DO_UPCAST(VNICState, vns_nc, ncp);

	if (vsp->vns_ds.vnds_enabled && get_ethertype(buf, size, &ethtype)) {
		VNICDHCPState *vdsp = &vsp->vns_ds;
		int ret;
		switch (ethtype) {
		case ETH_P_ARP:
			if (!is_arp_request(buf, size, vdsp))
				goto send;
			ret = create_arp_response(buf, size, vdsp);
			break;
		case ETH_P_IP:
			if (!is_dhcp_request(buf, size))
				goto send;
			ret = create_dhcp_response(buf, size, vdsp);
			break;
		default:
			goto send;
		}

		if (!ret)
			return (size);

		ret = qemu_send_packet_async(&vsp->vns_nc,
		    vdsp->vnds_buf, ret, vnic_send_completed);
		if (ret == 0)
			vnic_read_poll(vsp, 0);

		return (size);
	}

send:
	return (vnic_write_packet(vsp, buf, size));
}

static void
vnic_cleanup(VLANClientState *ncp)
{
	VNICState *vsp;

	vsp = DO_UPCAST(VNICState, vns_nc, ncp);

	qemu_purge_queued_packets(ncp);

	dlpi_close(vsp->vns_hdl);
}

static void
vnic_update_fd_handler(VNICState *vsp)
{
	qemu_set_fd_handler2(vsp->vns_fd,
	    vsp->vns_rpoll ? vnic_can_send : NULL,
	    vsp->vns_rpoll ? vnic_send : NULL,
	    vsp->vns_wpoll ? vnic_writable : NULL,
	    vsp);
}

static NetClientInfo net_vnic_info = {
	.type = NET_CLIENT_TYPE_VNIC,
	.size = sizeof (VNICState),
	.receive = vnic_receive,
	.poll = vnic_poll,
	.cleanup = vnic_cleanup
};

#ifdef CONFIG_SUNOS_VNIC_KVM
static int
net_init_kvm(int vfd)
{
	int kfd;

	if ((kfd = open("/dev/kvm", O_RDWR)) < 0) {
		error_report("can't open /dev/kvm for vnic: %s\n",
		    strerror(errno));
		return (-1);
	}

	/* XXX We shouldn't be embedding the KVM_NET_QUEUE fd */
	if (ioctl(kfd, 0x2000ae21, vfd) < 0) {
		error_report("can't ioctl: %s\n", strerror(errno));
		return (-1);
	}

	(void) close(kfd);

	return (0);
}
#endif

int
net_init_vnic(QemuOpts *opts, Monitor *mon, const char *name, VLANState *vlan)
{
	int fd, len;
	const char *ifname, *mac;
	uchar_t *macaddr;
	VLANClientState *ncp;
	VNICState *vsp;

	if ((ifname = qemu_opt_get(opts, "ifname")) == NULL) {
		error_report("missing ifname required for vnic\n");
		return (-1);
	}

	mac = qemu_opt_get(opts, "macaddr");

	if (mac != NULL) {
		macaddr = _link_aton(mac, &len);
		if (macaddr == NULL || len != ETHERADDRL) {
			error_report("invalid macaddr for vnic: %s\n", mac);
			return (-1);
		}
	}

	ncp = qemu_new_net_client(&net_vnic_info, vlan, NULL, "vnic", name);
	vsp = DO_UPCAST(VNICState, vns_nc, ncp);

	if (dlpi_open(ifname, &vsp->vns_hdl, DLPI_RAW) != DLPI_SUCCESS) {
		error_report("vnic: failed to open interface %s", ifname);
		return (-1);
	}

	if (dlpi_bind(vsp->vns_hdl, DLPI_ANY_SAP, &vsp->vns_sap) != DLPI_SUCCESS) {
		error_report("vnic: failed to bind interface %s", ifname);
		return (-1);
	}

	/*
	 * We only set the mac address of the vnic if the user passed in the
	 * option on the command line.
	 */
	if (mac != NULL) {
		if (dlpi_set_physaddr(vsp->vns_hdl, DL_CURR_PHYS_ADDR, macaddr,
		    ETHERADDRL) != DLPI_SUCCESS) {
			error_report("vnic: failed to set mac address\n");
			return (-1);
		}
	}

	/*
	 * We are enabling support for two different kinds of promiscuous modes.
	 * The first is getting us the basics of the unicast traffic that we
	 * care about. The latter is going to ensure that we also get other
	 * types of physical traffic such as multicast and broadcast.
	 */
	if (dlpi_promiscon(vsp->vns_hdl, DL_PROMISC_SAP) != DLPI_SUCCESS) {
		error_report("vnic: failed to be promiscous with interface %s",
		    ifname);
		return (-1);
	}

	if (dlpi_promiscon(vsp->vns_hdl, DL_PROMISC_PHYS) != DLPI_SUCCESS) {
		error_report("vnic: failed to be promiscous with interface %s",
		    ifname);
		return (-1);
	}

	fd = dlpi_fd(vsp->vns_hdl);

	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
		error_report("vnic: failed to set fd on interface %s to "
		    "non-blocking: %s\n", ifname, strerror(errno));
		return (-1);
	}

	vsp->vns_fd = fd;

	snprintf(vsp->vns_nc.info_str, sizeof (vsp->vns_nc.info_str),
	    "ifname=%s", qemu_opt_get(opts, "ifname"));

	if (vnic_dhcp_init(&vsp->vns_ds, opts) == 0)
		return (-1);

#ifdef CONFIG_SUNOS_VNIC_KVM
	net_init_kvm(fd);
#endif

	/* We have to manually intialize the polling for read */
	vnic_read_poll(vsp, 1);

	return (0);
}

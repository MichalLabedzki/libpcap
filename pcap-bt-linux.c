/*
 * Copyright (c) 2006 Paolo Abeni (Italy)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 * products derived from this software without specific prior written
 * permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Bluetooth sniffing API implementation for Linux platform
 * By Paolo Abeni <paolo.abeni@email.it>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pcap-int.h"
#include "pcap-bt-linux.h"
#include "pcap/bluetooth.h"

#ifdef NEED_STRERROR_H
#include "strerror.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <arpa/inet.h>

#define HCI_MAX_DEV 15

/* Start of copy of unexported Linux Kernel headers */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH    31
#endif

#define BTPROTO_HCI  1

#define SOL_HCI     0

#define HCI_CHANNEL_RAW     0

#define HCI_DATA_DIR    1
#define HCI_FILTER      2
#define HCI_TIME_STAMP  3

#define HCI_CMSG_DIR      0x0001
#define HCI_CMSG_TSTAMP   0x0002

#define HCIGETDEVLIST   _IOR('H', 210, int)
#define HCIGETDEVINFO   _IOR('H', 211, int)

struct hci_dev_req {
	uint16_t  dev_id;
	uint32_t  dev_opt;
};

struct hci_dev_list_req {
	uint16_t  dev_num;
	struct hci_dev_req dev_req[HCI_MAX_DEV];
};

typedef struct {
	uint8_t b[6];
} bdaddr_t;

struct sockaddr_hci_v1 {
	sa_family_t    hci_family;
	unsigned short hci_dev;
};

struct sockaddr_hci_v2 {
	sa_family_t    hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

struct hci_filter {
	uint32_t type_mask;
	uint32_t event_mask[2];
	uint16_t opcode;
};

struct hci_dev_stats {
	uint32_t err_rx;
	uint32_t err_tx;
	uint32_t cmd_tx;
	uint32_t evt_rx;
	uint32_t acl_tx;
	uint32_t acl_rx;
	uint32_t sco_tx;
	uint32_t sco_rx;
	uint32_t byte_rx;
	uint32_t byte_tx;
};

struct hci_dev_info {
	uint16_t dev_id;
	char  name[8];

	bdaddr_t bdaddr;

	uint32_t flags;
	uint8_t  type;

	uint8_t  features[8];

	uint32_t pkt_type;
	uint32_t link_policy;
	uint32_t link_mode;

	uint16_t acl_mtu;
	uint16_t acl_pkts;
	uint16_t sco_mtu;
	uint16_t sco_pkts;

	struct hci_dev_stats stat;
};
/* End of copy of unexported Linux Kernel headers */

#define BT_IFACE "bluetooth"
#define BT_CTRL_SIZE 128

/* forward declaration */
static int bt_activate(pcap_t *);
static int bt_read_linux(pcap_t *, int , pcap_handler , u_char *);
static int bt_inject_linux(pcap_t *, const void *, size_t);
static int bt_setdirection_linux(pcap_t *, pcap_direction_t);
static int bt_stats_linux(pcap_t *, struct pcap_stat *);

/*
 * Private data for capturing on Linux Bluetooth devices.
 */
struct pcap_bt {
	int dev_id;		/* device ID of device we're bound to */
};

int
bt_findalldevs(pcap_if_t **alldevsp, char *err_str)
{
	struct hci_dev_list_req dev_list;
	struct hci_dev_req *dev_req;
	int i, sock;
	int ret = 0;

	sock  = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (sock < 0)
	{
		/* if bluetooth is not supported this is not fatal*/
		if (errno == EAFNOSUPPORT)
			return 0;
		snprintf(err_str, PCAP_ERRBUF_SIZE,
		    "Can't open raw Bluetooth socket: %s", strerror(errno));
		return -1;
	}

	dev_list.dev_num = HCI_MAX_DEV;

	if (ioctl(sock, HCIGETDEVLIST, (void *) &dev_list) < 0)
	{
		snprintf(err_str, PCAP_ERRBUF_SIZE,
		    "Can't get Bluetooth device list via ioctl: %s",
		    strerror(errno));
		ret = -1;
		goto done;
	}

	dev_req = dev_list.dev_req;
	for (i = 0; i < dev_list.dev_num; i++) {
		char dev_name[20], dev_descr[30];

		snprintf(dev_name, 20, BT_IFACE"%d", dev_req[i].dev_id);
		snprintf(dev_descr, 30, "Bluetooth adapter number %d", i);

		if (pcap_add_if(alldevsp, dev_name, 0,
		       dev_descr, err_str) < 0)
		{
			ret = -1;
			break;
		}

	}

done:
	close(sock);
	return ret;
}

pcap_t *
bt_create(const char *device, char *ebuf, int *is_ours)
{
	const char *cp;
	char *cpend;
	long devnum;
	pcap_t *p;

	/* Does this look like a Bluetooth device? */
	cp = strrchr(device, '/');
	if (cp == NULL)
		cp = device;
	/* Does it begin with BT_IFACE? */
	if (strncmp(cp, BT_IFACE, sizeof BT_IFACE - 1) != 0) {
		/* Nope, doesn't begin with BT_IFACE */
		*is_ours = 0;
		return NULL;
	}
	/* Yes - is BT_IFACE followed by a number? */
	cp += sizeof BT_IFACE - 1;
	devnum = strtol(cp, &cpend, 10);
	if (cpend == cp || *cpend != '\0') {
		/* Not followed by a number. */
		*is_ours = 0;
		return NULL;
	}
	if (devnum < 0) {
		/* Followed by a non-valid number. */
		*is_ours = 0;
		return NULL;
	}

	/* OK, it's probably ours. */
	*is_ours = 1;

	p = pcap_create_common(device, ebuf, sizeof (struct pcap_bt));
	if (p == NULL)
		return (NULL);

	p->activate_op = bt_activate;
	return (p);
}

static int
bt_activate(pcap_t* handle)
{
	struct pcap_bt *handlep = handle->priv;
	struct sockaddr_hci_v1 addr_v1;
	struct sockaddr_hci_v2 addr_v2;
	struct sockaddr       *addr;
	int                    addr_size;
	int opt;
	int		dev_id;
	struct hci_filter	flt;
	int err = PCAP_ERROR;
	struct utsname uname_data;
	unsigned int   version_major;
	unsigned int   version_minor;
	unsigned int   version_release;

	/* get bt interface id */
	if (sscanf(handle->opt.source, BT_IFACE"%d", &dev_id) != 1)
	{
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
			"Can't get Bluetooth device index from %s",
			 handle->opt.source);
		return PCAP_ERROR;
	}

	/* Initialize some components of the pcap structure. */
	handle->bufsize = handle->snapshot+BT_CTRL_SIZE+sizeof(pcap_bluetooth_h4_header);
	handle->offset = BT_CTRL_SIZE;
	handle->linktype = DLT_BLUETOOTH_HCI_H4_WITH_PHDR;

	handle->read_op = bt_read_linux;
	handle->inject_op = bt_inject_linux;
	handle->setfilter_op = install_bpf_program; /* no kernel filtering */
	handle->setdirection_op = bt_setdirection_linux;
	handle->set_datalink_op = NULL;	/* can't change data link type */
	handle->getnonblock_op = pcap_getnonblock_fd;
	handle->setnonblock_op = pcap_setnonblock_fd;
	handle->stats_op = bt_stats_linux;
	handlep->dev_id = dev_id;

	/* Create HCI socket */
	handle->fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (handle->fd < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		    "Can't create raw socket: %s", strerror(errno));
		return PCAP_ERROR;
	}

	handle->buffer = malloc(handle->bufsize);
	if (!handle->buffer) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "Can't allocate dump buffer: %s",
			pcap_strerror(errno));
		goto close_fail;
	}

	opt = 1;
	if (setsockopt(handle->fd, SOL_HCI, HCI_DATA_DIR, &opt, sizeof(opt)) < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		    "Can't enable data direction info: %s", strerror(errno));
		goto close_fail;
	}

	opt = 1;
	if (setsockopt(handle->fd, SOL_HCI, HCI_TIME_STAMP, &opt, sizeof(opt)) < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		    "Can't enable time stamp: %s", strerror(errno));
		goto close_fail;
	}

	/* Setup filter, do not call hci function to avoid dependence on
	 * external libs	*/
	memset(&flt, 0, sizeof(flt));
	memset((void *) &flt.type_mask, 0xff, sizeof(flt.type_mask));
	memset((void *) &flt.event_mask, 0xff, sizeof(flt.event_mask));
	if (setsockopt(handle->fd, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		    "Can't set filter: %s", strerror(errno));
		goto close_fail;
	}

	if (!(uname(&uname_data) == 0 && sscanf(uname_data.release, "%u.%u.%u", &version_major, &version_minor, &version_release) == 3)) {
		/* Fallback to older kernel version */
		version_major = 2;
		version_minor = 6;
		version_release = 37;
	}

	if (!(version_major >= 2 && version_minor >= 6 && version_release >= 38)) {
		addr_v2.hci_family = AF_BLUETOOTH;
		addr_v2.hci_dev = handlep->dev_id;
		addr_v2.hci_channel = HCI_CHANNEL_RAW;

		addr_size = sizeof(struct sockaddr_hci_v2);
		addr = (struct sockaddr *) &addr_v2;
	} else {
		addr_v1.hci_family = AF_BLUETOOTH;
		addr_v1.hci_dev = handlep->dev_id;

		addr_size = sizeof(struct sockaddr_hci_v1);
		addr = (struct sockaddr *) &addr_v1;
	}

	/* Bind socket to the HCI device */
	if (bind(handle->fd,  addr, addr_size) < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		    "Can't attach to device %d: %s", handlep->dev_id,
		    strerror(errno));
		goto close_fail;
	}

	if (handle->opt.rfmon) {
		/*
		 * Monitor mode doesn't apply to Bluetooth devices.
		 */
		err = PCAP_ERROR_RFMON_NOTSUP;
		goto close_fail;
	}

	if (handle->opt.buffer_size != 0) {
		/*
		 * Set the socket buffer size to the specified value.
		 */
		if (setsockopt(handle->fd, SOL_SOCKET, SO_RCVBUF,
		    &handle->opt.buffer_size,
		    sizeof(handle->opt.buffer_size)) == -1) {
			snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
				 "SO_RCVBUF: %s", pcap_strerror(errno));
			goto close_fail;
		}
	}

	handle->selectable_fd = handle->fd;
	return 0;

close_fail:
	pcap_cleanup_live_common(handle);
	return err;
}

static int
bt_read_linux(pcap_t *handle, int max_packets _U_, pcap_handler callback, u_char *user)
{
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec  iv;
	ssize_t ret;
	struct pcap_pkthdr pkth;
	pcap_bluetooth_h4_header* bthdr;
	int in=0;

	bthdr = (pcap_bluetooth_h4_header*) &handle->buffer[handle->offset];
	iv.iov_base = &handle->buffer[handle->offset+sizeof(pcap_bluetooth_h4_header)];
	iv.iov_len  = handle->snapshot;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iv;
	msg.msg_iovlen = 1;
	msg.msg_control = handle->buffer;
	msg.msg_controllen = handle->offset;

	/* ignore interrupt system call error */
	do {
		ret = recvmsg(handle->fd, &msg, 0);
		if (handle->break_loop)
		{
			handle->break_loop = 0;
			return -2;
		}
	} while ((ret == -1) && (errno == EINTR));

	if (ret < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		    "Can't receive packet: %s", strerror(errno));
		return -1;
	}

	pkth.caplen = ret;

	/* get direction and timestamp*/
	cmsg = CMSG_FIRSTHDR(&msg);

	while (cmsg) {
		switch (cmsg->cmsg_type) {
			case HCI_CMSG_DIR:
				memcpy(&in, CMSG_DATA(cmsg), sizeof in);
				break;
			case HCI_CMSG_TSTAMP:
				memcpy(&pkth.ts, CMSG_DATA(cmsg),
				sizeof pkth.ts);
				break;
		}
		cmsg = CMSG_NXTHDR(&msg, cmsg);
	}
	if ((in && (handle->direction == PCAP_D_OUT)) ||
				((!in) && (handle->direction == PCAP_D_IN)))
		return 0;

	bthdr->direction = htonl(in != 0);
	pkth.caplen+=sizeof(pcap_bluetooth_h4_header);
	pkth.len = pkth.caplen;
	if (handle->fcode.bf_insns == NULL ||
	    bpf_filter(handle->fcode.bf_insns, &handle->buffer[handle->offset],
	      pkth.len, pkth.caplen)) {
		callback(user, &pkth, &handle->buffer[handle->offset]);
		return 1;
	}
	return 0;	/* didn't pass filter */
}

static int
bt_inject_linux(pcap_t *handle, const void *buf _U_, size_t size _U_)
{
	snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "inject not supported on "
    		"bluetooth devices");
	return (-1);
}


static int
bt_stats_linux(pcap_t *handle, struct pcap_stat *stats)
{
	struct pcap_bt *handlep = handle->priv;
	int ret;
	struct hci_dev_info dev_info;
	struct hci_dev_stats * s = &dev_info.stat;
	dev_info.dev_id = handlep->dev_id;

	/* ignore eintr */
	do {
		ret = ioctl(handle->fd, HCIGETDEVINFO, (void *)&dev_info);
	} while ((ret == -1) && (errno == EINTR));

	if (ret < 0) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		    "Can't get stats via ioctl: %s", strerror(errno));
		return (-1);
	}

	/* we receive both rx and tx frames, so comulate all stats */
	stats->ps_recv = s->evt_rx + s->acl_rx + s->sco_rx + s->cmd_tx +
		s->acl_tx +s->sco_tx;
	stats->ps_drop = s->err_rx + s->err_tx;
	stats->ps_ifdrop = 0;
	return 0;
}

static int
bt_setdirection_linux(pcap_t *p, pcap_direction_t d)
{
	p->direction = d;
	return 0;
}

#include "can_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

struct can_bus {
	SOCKET sock;
	struct sockaddr_in addr;
	int virtual_bus;
};

static char last_error[256];
static int wsa_started = 0;
static int last_would_block = 0;

static unsigned short vcan_port(const char *name)
{
	unsigned int hash = 5381;
	const unsigned char *p = (const unsigned char *)name;

	while (*p)
		hash = ((hash << 5) + hash) ^ *p++;

	return (unsigned short)(30000 + (hash % 20000));
}

static void vcan_group(const char *name, struct in_addr *group)
{
	unsigned int hash = 2166136261U;
	const unsigned char *p = (const unsigned char *)name;
	unsigned char octet3;
	unsigned char octet4;

	while (*p) {
		hash ^= *p++;
		hash *= 16777619U;
	}

	octet3 = (unsigned char)(hash & 0xffU);
	octet4 = (unsigned char)((hash >> 8) & 0xffU);
	group->s_addr = htonl(0xEFFF0000U | ((unsigned int)octet3 << 8) | octet4);
}

static void set_last_wsa_error(const char *context)
{
	int err = WSAGetLastError();
	last_would_block = err == WSAEWOULDBLOCK;
	snprintf(last_error, sizeof(last_error), "%s: WSA error %d", context, err);
}

int can_bus_open(can_bus_t **bus, const char *name)
{
	WSADATA wsa;
	struct can_bus *opened;
	struct sockaddr_in bind_addr;
	struct ip_mreq mreq;
	struct in_addr loopback_addr;
	int reuse = 1;
	int loopback = 1;
	int ttl = 1;

	if (!wsa_started) {
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			snprintf(last_error, sizeof(last_error), "WSAStartup failed");
			return -1;
		}
		wsa_started = 1;
	}

	opened = calloc(1, sizeof(*opened));
	if (!opened) {
		snprintf(last_error, sizeof(last_error), "out of memory");
		return -1;
	}

	opened->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (opened->sock == INVALID_SOCKET) {
		set_last_wsa_error("socket");
		free(opened);
		return -1;
	}

	loopback_addr.s_addr = htonl(INADDR_LOOPBACK);
	setsockopt(opened->sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
	{
		int buffer_size = 1024 * 1024;
		setsockopt(opened->sock, SOL_SOCKET, SO_RCVBUF,
		           (const char *)&buffer_size, sizeof(buffer_size));
	}

	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(vcan_port(name));

	if (bind(opened->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
		set_last_wsa_error("bind");
		closesocket(opened->sock);
		free(opened);
		return -1;
	}

	memset(&opened->addr, 0, sizeof(opened->addr));
	opened->addr.sin_family = AF_INET;
	vcan_group(name, &opened->addr.sin_addr);
	opened->addr.sin_port = htons(vcan_port(name));

	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_multiaddr = opened->addr.sin_addr;
	mreq.imr_interface = loopback_addr;
	if (setsockopt(opened->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
	               (const char *)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
		set_last_wsa_error("IP_ADD_MEMBERSHIP");
		closesocket(opened->sock);
		free(opened);
		return -1;
	}
	setsockopt(opened->sock, IPPROTO_IP, IP_MULTICAST_IF,
	           (const char *)&loopback_addr, sizeof(loopback_addr));
	setsockopt(opened->sock, IPPROTO_IP, IP_MULTICAST_LOOP,
	           (const char *)&loopback, sizeof(loopback));
	setsockopt(opened->sock, IPPROTO_IP, IP_MULTICAST_TTL,
	           (const char *)&ttl, sizeof(ttl));

	opened->virtual_bus = 1;
	*bus = opened;
	return 0;
}

int can_bus_send(can_bus_t *bus, const struct canfd_frame *frame, size_t mtu)
{
	if (sendto(bus->sock, (const char *)frame, (int)mtu, 0,
	           (const struct sockaddr *)&bus->addr, sizeof(bus->addr)) == SOCKET_ERROR) {
		set_last_wsa_error("sendto");
		return -1;
	}
	return 0;
}

int can_bus_recv(can_bus_t *bus, struct canfd_frame *frame, size_t *mtu)
{
	int nbytes = recvfrom(bus->sock, (char *)frame, sizeof(*frame), 0, NULL, NULL);

	if (nbytes == SOCKET_ERROR) {
		set_last_wsa_error("recvfrom");
		return -1;
	}

	*mtu = (size_t)nbytes;
	return 0;
}

int can_bus_set_nonblocking(can_bus_t *bus, int nonblocking)
{
	u_long mode = nonblocking ? 1 : 0;

	if (ioctlsocket(bus->sock, FIONBIO, &mode) == SOCKET_ERROR) {
		set_last_wsa_error("ioctlsocket FIONBIO");
		return -1;
	}
	return 0;
}

int can_bus_error_is_would_block(void)
{
	return last_would_block;
}

void can_bus_close(can_bus_t *bus)
{
	if (!bus)
		return;

	closesocket(bus->sock);
	free(bus);
}

const char *can_bus_error(void)
{
	return last_error[0] ? last_error : "unknown CAN bus error";
}

int can_bus_is_virtual(const can_bus_t *bus)
{
	return bus ? bus->virtual_bus : 0;
}

#else
#include <net/if.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

struct can_bus {
	int fd;
	int virtual_bus;
};

static char last_error[256];
static const int canfd_on = 1;
static int last_would_block = 0;

static void set_last_errno(const char *context)
{
	last_would_block = errno == EAGAIN || errno == EWOULDBLOCK;
	snprintf(last_error, sizeof(last_error), "%s: %s", context, strerror(errno));
}

int can_bus_open(can_bus_t **bus, const char *name)
{
	struct sockaddr_can addr;
	struct ifreq ifr;
	struct can_bus *opened = calloc(1, sizeof(*opened));

	if (!opened) {
		snprintf(last_error, sizeof(last_error), "out of memory");
		return -1;
	}

	opened->fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (opened->fd < 0) {
		set_last_errno("socket");
		free(opened);
		return -1;
	}

	addr.can_family = AF_CAN;
	memset(&ifr.ifr_name, 0, sizeof(ifr.ifr_name));
	strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name) - 1);
	if (ioctl(opened->fd, SIOCGIFINDEX, &ifr) < 0) {
		set_last_errno("SIOCGIFINDEX");
		close(opened->fd);
		free(opened);
		return -1;
	}

	addr.can_ifindex = ifr.ifr_ifindex;
	setsockopt(opened->fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on));

	if (bind(opened->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		set_last_errno("bind");
		close(opened->fd);
		free(opened);
		return -1;
	}

	opened->virtual_bus = 0;
	*bus = opened;
	return 0;
}

int can_bus_send(can_bus_t *bus, const struct canfd_frame *frame, size_t mtu)
{
	if (write(bus->fd, frame, mtu) != (ssize_t)mtu) {
		set_last_errno("write");
		return -1;
	}
	return 0;
}

int can_bus_recv(can_bus_t *bus, struct canfd_frame *frame, size_t *mtu)
{
	ssize_t nbytes = read(bus->fd, frame, sizeof(*frame));

	if (nbytes < 0) {
		set_last_errno("read");
		return -1;
	}

	*mtu = (size_t)nbytes;
	return 0;
}

int can_bus_set_nonblocking(can_bus_t *bus, int nonblocking)
{
	int flags = fcntl(bus->fd, F_GETFL, 0);

	if (flags < 0) {
		set_last_errno("fcntl F_GETFL");
		return -1;
	}
	if (nonblocking)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;
	if (fcntl(bus->fd, F_SETFL, flags) < 0) {
		set_last_errno("fcntl F_SETFL");
		return -1;
	}
	return 0;
}

int can_bus_error_is_would_block(void)
{
	return last_would_block;
}

void can_bus_close(can_bus_t *bus)
{
	if (!bus)
		return;

	close(bus->fd);
	free(bus);
}

const char *can_bus_error(void)
{
	return last_error[0] ? last_error : "unknown CAN bus error";
}

int can_bus_is_virtual(const can_bus_t *bus)
{
	return bus ? bus->virtual_bus : 0;
}
#endif

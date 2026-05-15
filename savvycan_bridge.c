#include "can_platform.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GVRET_PORT 23
#define GVRET_DISCOVERY_PORT 17222
#define DEFAULT_BITRATE 500000U
#define GVRET_EXT_FLAG 0x80000000U

static CRITICAL_SECTION client_lock;
static SOCKET client_sock = INVALID_SOCKET;
static can_bus_t *can_bus;
static DWORD start_ticks;
static LONG gvret_frames_out;
static LONG gvret_frames_in;
static LONG vcan_recv_errors;
static int show_stats;
static FILE *log_file;

static void log_event(const char *fmt, ...)
{
	va_list args;

	if (!log_file)
		return;

	va_start(args, fmt);
	vfprintf(log_file, fmt, args);
	va_end(args);
	fputc('\n', log_file);
	fflush(log_file);
}

static uint32_t read_le32(const unsigned char *p)
{
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)(value & 0xffU);
	p[1] = (unsigned char)((value >> 8) & 0xffU);
	p[2] = (unsigned char)((value >> 16) & 0xffU);
	p[3] = (unsigned char)((value >> 24) & 0xffU);
}

static uint32_t gvret_timestamp(void)
{
	return (uint32_t)(GetTickCount() - start_ticks) * 1000U;
}

static int send_all(SOCKET sock, const unsigned char *data, int len)
{
	int sent = 0;

	while (sent < len) {
		int ret = send(sock, (const char *)data + sent, len - sent, 0);
		if (ret == SOCKET_ERROR || ret == 0)
			return -1;
		sent += ret;
	}

	return 0;
}

static void send_to_client(const unsigned char *data, int len)
{
	EnterCriticalSection(&client_lock);
	if (client_sock != INVALID_SOCKET && send_all(client_sock, data, len) < 0) {
		closesocket(client_sock);
		client_sock = INVALID_SOCKET;
	}
	LeaveCriticalSection(&client_lock);
}

static void gvret_reply_validation(void)
{
	static const unsigned char reply[] = { 0xf1, 0x09 };
	send_to_client(reply, (int)sizeof(reply));
}

static void gvret_reply_time(void)
{
	unsigned char reply[6] = { 0xf1, 0x01, 0, 0, 0, 0 };
	write_le32(&reply[2], gvret_timestamp());
	send_to_client(reply, (int)sizeof(reply));
}

static void gvret_reply_bus_params(void)
{
	unsigned char reply[12] = { 0xf1, 0x06 };

	reply[2] = 0x01;
	write_le32(&reply[3], DEFAULT_BITRATE);
	reply[7] = 0x00;
	write_le32(&reply[8], DEFAULT_BITRATE);
	send_to_client(reply, (int)sizeof(reply));
}

static void gvret_reply_device_info(void)
{
	unsigned char reply[8] = { 0xf1, 0x07, 0x57, 0x01, 0x01, 0x00, 0x00, 0x00 };
	send_to_client(reply, (int)sizeof(reply));
}

static void gvret_reply_num_buses(void)
{
	static const unsigned char reply[] = { 0xf1, 0x0c, 0x01 };
	send_to_client(reply, (int)sizeof(reply));
}

static void gvret_reply_ext_buses(void)
{
	unsigned char reply[17] = { 0xf1, 0x0d };
	send_to_client(reply, (int)sizeof(reply));
}

static void forward_can_to_gvret(const struct canfd_frame *frame)
{
	unsigned char packet[2 + 4 + 4 + 1 + CAN_MAX_DLEN];
	uint32_t id = frame->can_id;
	unsigned int len = frame->len;

	if (id & CAN_ERR_FLAG)
		return;
	if (id & CAN_EFF_FLAG)
		id = (id & CAN_EFF_MASK) | GVRET_EXT_FLAG;
	else
		id &= CAN_SFF_MASK;
	if (len > CAN_MAX_DLEN)
		len = CAN_MAX_DLEN;

	packet[0] = 0xf1;
	packet[1] = 0x00;
	write_le32(&packet[2], gvret_timestamp());
	write_le32(&packet[6], id);
	packet[10] = (unsigned char)len;
	memcpy(&packet[11], frame->data, len);

	send_to_client(packet, 11 + (int)len);
	InterlockedIncrement(&gvret_frames_out);
}

static DWORD WINAPI can_rx_thread(LPVOID unused)
{
	(void)unused;

	for (;;) {
		struct canfd_frame frame;
		size_t mtu = 0;

		if (can_bus_recv(can_bus, &frame, &mtu) == 0) {
			if (mtu >= CAN_MTU)
				forward_can_to_gvret(&frame);
		} else {
			InterlockedIncrement(&vcan_recv_errors);
			Sleep(10);
		}
	}

	return 0;
}

static DWORD WINAPI stats_thread(LPVOID unused)
{
	LONG last_out = 0;
	LONG last_in = 0;
	LONG last_errors = 0;

	(void)unused;

	for (;;) {
		LONG out_now;
		LONG in_now;
		LONG errors_now;
		int connected;

		Sleep(1000);
		out_now = gvret_frames_out;
		in_now = gvret_frames_in;
		errors_now = vcan_recv_errors;

		EnterCriticalSection(&client_lock);
		connected = client_sock != INVALID_SOCKET;
		LeaveCriticalSection(&client_lock);

		printf("stats: connected=%d vcan->gvret=%ld/s gvret->vcan=%ld/s recv_errors=%ld/s\n",
		       connected,
		       out_now - last_out,
		       in_now - last_in,
		       errors_now - last_errors);
		last_out = out_now;
		last_in = in_now;
		last_errors = errors_now;
	}

	return 0;
}

static void send_discovery_to(SOCKET sock, const char *address)
{
	struct sockaddr_in dest;
	const char payload[] = "ICSim GVRET";

	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(GVRET_DISCOVERY_PORT);
	inet_pton(AF_INET, address, &dest.sin_addr);

	sendto(sock, payload, (int)sizeof(payload) - 1, 0,
	       (const struct sockaddr *)&dest, sizeof(dest));
}

static DWORD WINAPI discovery_thread(LPVOID unused)
{
	SOCKET sock;
	int broadcast = 1;

	(void)unused;

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return 1;

	setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
	           (const char *)&broadcast, sizeof(broadcast));

	for (;;) {
		send_discovery_to(sock, "127.0.0.1");
		send_discovery_to(sock, "255.255.255.255");
		Sleep(1000);
	}
}

enum parser_state {
	WAIT_F1,
	WAIT_COMMAND,
	READ_FRAME,
	SKIP_PAYLOAD
};

struct gvret_parser {
	enum parser_state state;
	unsigned char command;
	unsigned char data[80];
	int data_len;
	int needed;
};

static void send_gvret_frame_to_can(const unsigned char *data, int len)
{
	struct canfd_frame frame;
	uint32_t id;
	unsigned int dlc;

	if (len < 5)
		return;

	id = read_le32(data);
	dlc = data[4];
	if (dlc > CAN_MAX_DLEN || len < 5 + (int)dlc)
		return;

	memset(&frame, 0, sizeof(frame));
	if (id & GVRET_EXT_FLAG)
		frame.can_id = (id & CAN_EFF_MASK) | CAN_EFF_FLAG;
	else
		frame.can_id = id & CAN_SFF_MASK;
	frame.len = (uint8_t)dlc;
	memcpy(frame.data, &data[5], dlc);

	if (can_bus_send(can_bus, &frame, CAN_MTU) < 0)
		fprintf(stderr, "vcan send failed: %s\n", can_bus_error());
	else {
		InterlockedIncrement(&gvret_frames_in);
		log_event("RX id=0x%lx len=%u", (unsigned long)frame.can_id, dlc);
	}
}

static int command_payload_len(unsigned char command)
{
	switch (command) {
	case 0x05:
		return 9;
	case 0x08:
		return 1;
	case 0x0e:
		return 13;
	default:
		return 0;
	}
}

static void parser_init(struct gvret_parser *parser)
{
	memset(parser, 0, sizeof(*parser));
	parser->state = WAIT_F1;
}

static void parser_feed(struct gvret_parser *parser, unsigned char c)
{
	switch (parser->state) {
	case WAIT_F1:
		if (c == 0xf1)
			parser->state = WAIT_COMMAND;
		break;
	case WAIT_COMMAND:
		parser->command = c;
		parser->data_len = 0;
		parser->needed = command_payload_len(c);
		switch (c) {
		case 0x00:
			parser->state = READ_FRAME;
			parser->needed = 5;  /* ID(4) + DLC(1) */
			break;
		case 0x01:
			gvret_reply_time();
			parser->state = WAIT_F1;
			break;
		case 0x06:
			gvret_reply_bus_params();
			parser->state = WAIT_F1;
			break;
		case 0x07:
			gvret_reply_device_info();
			parser->state = WAIT_F1;
			break;
		case 0x09:
			gvret_reply_validation();
			parser->state = WAIT_F1;
			break;
		case 0x0c:
			gvret_reply_num_buses();
			parser->state = WAIT_F1;
			break;
		case 0x0d:
			gvret_reply_ext_buses();
			parser->state = WAIT_F1;
			break;
		default:
			parser->state = parser->needed ? SKIP_PAYLOAD : WAIT_F1;
			break;
		}
		break;
	case READ_FRAME:
		if (parser->data_len < (int)sizeof(parser->data))
			parser->data[parser->data_len++] = c;
		if (parser->data_len == 5)
			parser->needed = 5 + (parser->data[4] & 0x0f);
		if (parser->data_len >= parser->needed) {
			send_gvret_frame_to_can(parser->data, parser->data_len);
			parser->state = WAIT_F1;
		}
		break;
	case SKIP_PAYLOAD:
		if (++parser->data_len >= parser->needed)
			parser->state = WAIT_F1;
		break;
	}
}

static SOCKET create_server_socket(void)
{
	SOCKET server;
	struct sockaddr_in addr;
	int reuse = 1;

	server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server == INVALID_SOCKET)
		return INVALID_SOCKET;

	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(GVRET_PORT);

	if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		closesocket(server);
		return INVALID_SOCKET;
	}
	if (listen(server, 1) == SOCKET_ERROR) {
		closesocket(server);
		return INVALID_SOCKET;
	}

	return server;
}

static void usage(const char *name)
{
	printf("Usage: %s [--stats] [bus]\n", name);
	printf("Example: %s --stats vcan0\n", name);
}

int main(int argc, char **argv)
{
	const char *bus_name = "vcan0";
	WSADATA wsa;
	SOCKET server;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--stats") == 0) {
			show_stats = 1;
		} else if (argv[i][0] == '-') {
			usage(argv[0]);
			return 1;
		} else {
			bus_name = argv[i];
		}
	}

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "WSAStartup failed\n");
		return 1;
	}

	if (can_bus_open(&can_bus, bus_name) < 0) {
		fprintf(stderr, "%s\n", can_bus_error());
		WSACleanup();
		return 1;
	}

	InitializeCriticalSection(&client_lock);
	start_ticks = GetTickCount();
	log_file = fopen("savvycan_bridge.log", "a");
	log_event("bridge start bus=%s", bus_name);

	server = create_server_socket();
	if (server == INVALID_SOCKET) {
		fprintf(stderr, "Failed to listen on 0.0.0.0:%d: WSA error %d\n",
		        GVRET_PORT, WSAGetLastError());
		can_bus_close(can_bus);
		WSACleanup();
		return 1;
	}

	CreateThread(NULL, 0, can_rx_thread, NULL, 0, NULL);
	CreateThread(NULL, 0, discovery_thread, NULL, 0, NULL);
	if (show_stats)
		CreateThread(NULL, 0, stats_thread, NULL, 0, NULL);

	printf("SavvyCAN GVRET bridge listening on 0.0.0.0:%d, bus %s\n",
	       GVRET_PORT, bus_name);
	printf("In SavvyCAN add Network Connection (GVRET) and select 127.0.0.1.\n");

	for (;;) {
		SOCKET accepted = accept(server, NULL, NULL);
		struct gvret_parser parser;
		int one = 1;
		int send_buf = 65536;

		if (accepted == INVALID_SOCKET)
			continue;

		setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
		           (const char *)&one, sizeof(one));
		setsockopt(accepted, SOL_SOCKET, SO_SNDBUF,
		           (const char *)&send_buf, sizeof(send_buf));

		EnterCriticalSection(&client_lock);
		if (client_sock != INVALID_SOCKET)
			closesocket(client_sock);
		client_sock = accepted;
		LeaveCriticalSection(&client_lock);

		printf("SavvyCAN connected\n");
		log_event("client connected");
		parser_init(&parser);

		for (;;) {
			unsigned char buf[256];
			int got = recv(accepted, (char *)buf, sizeof(buf), 0);
			int i;

			if (got <= 0)
				break;
			for (i = 0; i < got; i++)
				parser_feed(&parser, buf[i]);
		}

		EnterCriticalSection(&client_lock);
		if (client_sock == accepted)
			client_sock = INVALID_SOCKET;
		LeaveCriticalSection(&client_lock);
		closesocket(accepted);
		printf("SavvyCAN disconnected\n");
		log_event("client disconnected");
	}
}
#else
#include <stdio.h>

int main(void)
{
	fprintf(stderr, "savvycan_bridge is currently implemented for Windows only.\n");
	return 1;
}
#endif

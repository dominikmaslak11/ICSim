#include "can_platform.h"

#include <stdio.h>

#include "lib.h"

static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s <bus> <can-frame>\n", name);
	fprintf(stderr, "Example: %s vcan0 19B#000001\n", name);
}

int main(int argc, char **argv)
{
	can_bus_t *bus;
	struct canfd_frame frame;
	int mtu;

	if (argc != 3) {
		usage(argv[0]);
		return 1;
	}

	mtu = parse_canframe(argv[2], &frame);
	if (!mtu) {
		fprintf(stderr, "Invalid CAN frame: %s\n", argv[2]);
		return 1;
	}

	if (can_bus_open(&bus, argv[1]) < 0) {
		fprintf(stderr, "%s\n", can_bus_error());
		return 1;
	}

	if (can_bus_send(bus, &frame, (size_t)mtu) < 0) {
		fprintf(stderr, "%s\n", can_bus_error());
		can_bus_close(bus);
		return 1;
	}

	can_bus_close(bus);
	return 0;
}

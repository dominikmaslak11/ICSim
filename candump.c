#include "can_platform.h"

#include <stdio.h>

#include "lib.h"

static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s <bus>\n", name);
	fprintf(stderr, "Example: %s vcan0\n", name);
}

int main(int argc, char **argv)
{
	can_bus_t *bus;

	if (argc != 2) {
		usage(argv[0]);
		return 1;
	}

	if (can_bus_open(&bus, argv[1]) < 0) {
		fprintf(stderr, "%s\n", can_bus_error());
		return 1;
	}

	for (;;) {
		struct canfd_frame frame;
		size_t mtu = 0;
		int maxdlen;

		if (can_bus_recv(bus, &frame, &mtu) < 0) {
			fprintf(stderr, "%s\n", can_bus_error());
			can_bus_close(bus);
			return 1;
		}

		if (mtu == CAN_MTU)
			maxdlen = CAN_MAX_DLEN;
		else if (mtu == CANFD_MTU)
			maxdlen = CANFD_MAX_DLEN;
		else
			continue;

		fprint_canframe(stdout, &frame, "\n", 0, maxdlen);
		fflush(stdout);
	}
}

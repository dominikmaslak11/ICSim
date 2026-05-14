CC=gcc
CFLAGS=-I/usr/include/SDL2 -Wall -Wextra
LDFLAGS=-lSDL2 -lSDL2_image
COMMON=can_bus.c getopt_compat.c lib.c
COMMON_OBJS=can_bus.o getopt_compat.o lib_src.o

all: icsim controls

icsim: icsim.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o icsim icsim.o $(COMMON_OBJS) $(LDFLAGS)

controls: controls.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o controls controls.o $(COMMON_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

lib_src.o: lib.c lib.h can_platform.h
	$(CC) $(CFLAGS) -c -o $@ lib.c

clean:
	rm -rf icsim controls icsim.o controls.o can_bus.o getopt_compat.o lib_src.o

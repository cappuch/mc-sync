CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
LDFLAGS ?=

COMMON_OBJS = src/common.o src/fs_utils.o

all: mcsync mcsync-server

mcsync: src/mcsync_client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mcsync-server: src/mcsync_server.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f mcsync mcsync-server $(COMMON_OBJS) src/mcsync_client.o src/mcsync_server.o

.PHONY: all clean

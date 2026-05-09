# vmmd — KVM VMM from scratch
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -I include/
LDFLAGS = -lpthread

# vmmd daemon/runner
DAEMON_SRCS = src/main.c \
              src/kvm_core.c \
              src/boot.c \
              src/serial.c \
              src/virtio_blk.c \
              src/resources.c \
              src/snapshot.c \
              src/api.c

DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)

all: vmmd

vmmd: $(DAEMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Phase 1 quick test
test-kvm: vmmd
	./vmmd --test-kvm

clean:
	rm -f src/*.o vmmd

.PHONY: all clean test-kvm

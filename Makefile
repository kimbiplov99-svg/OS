# FuseFS Master Makefile
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g \
           $(shell pkg-config --cflags fuse3) \
           -Iinclude
LDFLAGS = $(shell pkg-config --libs fuse3) -lpthread

SRCS    = src/fs_core.c src/bitmap.c src/inode.c src/dir.c src/path.c src/fuse_ops.c
OBJS    = $(SRCS:.c=.o)

TOOL_SRCS_MKFS = src/fs_core.c src/bitmap.c src/inode.c src/dir.c tools/mkfs.c
TOOL_SRCS_FSCK = src/fs_core.c src/bitmap.c src/inode.c src/dir.c src/path.c tools/fsck.c

.PHONY: all clean test crash bench viva

all: fusefs tools/mkfs_tool tools/fsck_tool

fusefs: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tools/mkfs_tool: $(TOOL_SRCS_MKFS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tools/fsck_tool: $(TOOL_SRCS_FSCK)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) fusefs tools/mkfs_tool tools/fsck_tool
	rm -f *.img /tmp/fusefs_*.img /tmp/test*.img 2>/dev/null || true

test: all
	cd tests && bash test_suite.sh

crash: all
	cd tests && bash test_crash.sh

bench: all
	cd scripts && bash benchmark.sh

viva: all
	bash scripts/demo_viva.sh

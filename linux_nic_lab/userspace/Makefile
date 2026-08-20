# SPDX-License-Identifier: GPL-2.0
#
# Userspace test tools for exercising the edunic driver.

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -g

BINS := udp_server udp_client tcp_server tcp_client

all: $(BINS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS)

.PHONY: all clean

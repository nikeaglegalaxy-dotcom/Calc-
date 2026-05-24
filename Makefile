CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L

syslab: syslab.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f syslab

.PHONY: clean

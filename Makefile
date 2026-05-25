CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11

all: syslab

syslab: syslab.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f syslab

.PHONY: all clean

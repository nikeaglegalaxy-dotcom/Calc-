CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L

TARGET  := syslab
SRC     := syslab.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: clean

# soulc - minimalist soulseek client
.POSIX:

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS ?= -lz

SRC = main.c utils.c net.c slsk.c md5.c
OBJ = main.o utils.o net.o slsk.o md5.o
BIN = soulc

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all clean

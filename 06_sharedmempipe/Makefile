CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2

all: sharedmempipe
sharedmempipe: Rasul\ Mohammad\ -\ Command\ 6.c
	$(CC) $(CFLAGS) -o "$@" "$<"

test: all
	bash test.sh

clean:
	rm -f sharedmempipe
.PHONY: all test clean

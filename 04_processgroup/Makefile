CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2

all: processgroup
processgroup: Thierno\ Diallo\ -\ Command\ 4.c
	$(CC) $(CFLAGS) -o "$@" "$<"

test: all
	bash test.sh

clean:
	rm -f processgroup
.PHONY: all test clean

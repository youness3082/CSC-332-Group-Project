CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2

all: processgroup
processgroup: processgroup.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	bash test.sh

clean:
	rm -f processgroup
.PHONY: all test clean

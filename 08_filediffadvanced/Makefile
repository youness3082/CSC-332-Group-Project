CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2

all: filediffadvanced
filediffadvanced: filediffadvanced.c
	$(CC) $(CFLAGS) -o $@ $< 

test: all
	bash test.sh

clean:
	rm -f filediffadvanced
.PHONY: all test clean

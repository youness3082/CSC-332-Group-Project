CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -O2
all: loganalyzer
loganalyzer: loganalyzer.c
	$(CC) $(CFLAGS) -o $@ $<
test: all
	bash test.sh
clean:
	rm -f loganalyzer
.PHONY: all test clean

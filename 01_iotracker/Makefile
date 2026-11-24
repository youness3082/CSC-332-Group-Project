CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2

all: iotracker

iotracker: Rosina\ Zhou\ -\ iotracker.c
	$(CC) $(CFLAGS) -o iotracker "Rosina Zhou - iotracker.c"

test: all
	bash test.sh

man:
	man ./iotracker.1

clean:
	rm -f iotracker

.PHONY: all test man clean

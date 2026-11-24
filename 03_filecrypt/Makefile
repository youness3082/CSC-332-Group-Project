CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
LDLIBS = -lcrypto

all: filecrypt

filecrypt: James\ Lu\ -\ Command\ 3.c
	$(CC) $(CFLAGS) -o filecrypt "James Lu - Command 3.c" $(LDLIBS)

test: all
	bash test.sh

man:
	man ./filecrypt.1

clean:
	rm -f filecrypt

.PHONY: all test man clean
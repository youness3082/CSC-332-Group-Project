CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
LDLIBS = -lcrypto

# Each team command is compiled with -D<NAME>_NO_MAIN so only its
# <name>_main() function is linked into the shell.
OBJS = obj/cshell.o obj/iotracker.o obj/filecrypt.o obj/processgroup.o \
       obj/loganalyzer.o obj/sharedmempipe.o obj/filediffadvanced.o

all: cshell

cshell: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

obj:
	mkdir -p obj

obj/cshell.o: cshell.c commands.h | obj
	$(CC) $(CFLAGS) -c -o $@ cshell.c

obj/iotracker.o: ../01_iotracker/Rosina\ Zhou\ -\ iotracker.c | obj
	$(CC) $(CFLAGS) -DIOTRACKER_NO_MAIN -c -o $@ "$<"

obj/filecrypt.o: ../03_filecrypt/James\ Lu\ -\ Command\ 3.c | obj
	$(CC) $(CFLAGS) -DFILECRYPT_NO_MAIN -c -o $@ "$<"

obj/processgroup.o: ../04_processgroup/Thierno\ Diallo\ -\ Command\ 4.c | obj
	$(CC) $(CFLAGS) -DPROCESSGROUP_NO_MAIN -c -o $@ "$<"

obj/loganalyzer.o: ../05_loganalyzer/loganalyzer.c | obj
	$(CC) $(CFLAGS) -DLOGANALYZER_NO_MAIN -c -o $@ "$<"

obj/sharedmempipe.o: ../06_sharedmempipe/Rasul\ Mohammad\ -\ Command\ 6.c | obj
	$(CC) $(CFLAGS) -DSHAREDMEMPIPE_NO_MAIN -c -o $@ "$<"

obj/filediffadvanced.o: ../08_filediffadvanced/filediffadvanced.c | obj
	$(CC) $(CFLAGS) -DFILEDIFFADVANCED_NO_MAIN -c -o $@ "$<"

test: all
	bash test.sh

man:
	man ./cshell.1

clean:
	rm -rf cshell obj

.PHONY: all test man clean

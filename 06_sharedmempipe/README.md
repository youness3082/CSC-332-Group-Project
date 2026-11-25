# sharedmempipe

Moves data from one process to another through shared memory. Two pipes carry
1-byte signals so the two processes take turns using the memory.

## Live demo (run in this order)

Start in the repository folder (`CSC-332-Group-Project`).

**1. Build and run the tests.** All 13 tests should print `PASS`.

```bash
cd 06_sharedmempipe
make
make test
```

**2. Send one message.** Prints `hello`.

```bash
./sharedmempipe -m "hello"
```

**3. Send lines through standard input.** Prints `one` and `two`.

```bash
printf "one\ntwo\n" | ./sharedmempipe
```

**4. Send a 1 MB binary file (about 245 chunks).** Prints `IDENTICAL`.

```bash
head -c 1000000 /dev/urandom > big.bin
./sharedmempipe < big.bin > copy.bin
cmp big.bin copy.bin && echo IDENTICAL
```

**5. Watch every step with `-v` (verbose).** Uses a smaller 20,000-byte file,
so the output is short: 5 chunks (four of 4096 bytes, one of 3616 bytes). For
each chunk you see the producer put it in shared memory and send `D`, the
consumer print it and send `A`, and the producer get `A` back. The status
messages go to standard error, so they show on the screen but do not go into
`copy.bin`. `diff -s` then prints `Files small.bin and copy.bin are identical`.

```bash
head -c 20000 /dev/urandom > small.bin
./sharedmempipe -v < small.bin > copy.bin
diff -s small.bin copy.bin
```

`-v` also works with a message: `./sharedmempipe -v -m "hello"`.

**6. Show the help, then a bad option.** The bad option prints the usage and `exit=2`.

```bash
./sharedmempipe -h
./sharedmempipe -x; echo "exit=$?"
```

**7. Stop with Ctrl+C.** Type a few lines. They do not print yet, because the
program waits until it has a full 4096-byte chunk or the input ends. Press
Ctrl+C: the lines print and the program stops cleanly. Then run the `echo`. It
prints `exit=1`, which means it was interrupted but shut down properly.

```bash
cat | ./sharedmempipe
echo "exit=$?"
```

**8. Open the man page.** Press `q` to close it.

```bash
man ./sharedmempipe.1
```

**9. Clean up.**

```bash
make clean
rm -f big.bin small.bin copy.bin
```

## Options

| Option    | Meaning                                       |
|-----------|-----------------------------------------------|
| `-m text` | Send `text`. Without `-m`, read standard input. |
| `-v`      | Verbose: print each step to standard error.   |
| `-h`      | Show help and exit.                           |

Exit status: `0` success, `1` runtime error or interrupted (Ctrl+C), `2` bad usage.

## Design

- `mmap(MAP_SHARED | MAP_ANONYMOUS)` creates a 4096-byte block that the parent
  and child both see after `fork()`.
- The parent (producer) copies up to 4096 bytes into the block, sends `D` on
  the *ready* pipe, then blocks until the child sends `A` on the *ack* pipe.
- The child (consumer) prints the block, then sends `A`.
- Input larger than 4096 bytes is sent in several chunks. There is no size limit.
- When input ends, the parent sends `Q`, and then calls `waitpid()` on the child.

## Files

`Rasul Mohammad - Command 6.c` (source), `Makefile`, `test.sh` (tests),
`sharedmempipe.1` (man page), `sharedmempipe.pdf` (man page as PDF).

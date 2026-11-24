# sharedmempipe

Moves data from one process to another through shared memory. Two pipes carry
1-byte signals so the two processes take turns using the memory.

## Build and test

```bash
make
make test
```

## Run

```bash
./sharedmempipe -m "hello"                 # send one message
printf "one\ntwo\n" | ./sharedmempipe      # send standard input
./sharedmempipe < bigfile > copy           # works for any file, including binary
man ./sharedmempipe.1                      # read the man page
```

## Options

| Option    | Meaning                                       |
|-----------|-----------------------------------------------|
| `-m text` | Send `text`. Without `-m`, read standard input. |
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
`sharedmempipe.1` (man page).
